#include "LeechSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Cached.h"
#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/WorldItemSync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace LeechSync
{
    namespace
    {
        constexpr uint64_t StateMs = 100;       // 10 Гц, как в плане
        constexpr uint64_t HelloAfterMs = 3000; // Client: столько после загрузки карты — и просим всех пиявок
        constexpr size_t MaxPerPacket = 48;     // 48 × 21 байт ≈ 1 КБ — в одну датаграмму
        constexpr float SmoothTau = 0.08f;      // с: сглаживание марионетки между пакетами (10 Гц)

        struct Vec
        {
            float X, Y, Z;
        };
        struct Rot
        {
            float Pitch, Yaw, Roll;
        };

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        bool is_leech(UObject* actor) { return actor->GetClassPrivate()->GetName().starts_with(STR("BP_Hazard_Leech")); }

        // Тело пиявки: физический шар, на нём сетка; от корня актёра отсоединён (разведка 2026-11-17) — место пиявки = место тела
        UObject* body_of(UObject* actor)
        {
            auto** body = property<UObject*>(actor, STR("SM_Prototype_BlockySphere"));
            return body ? *body : nullptr;
        }

        UObject* chassis()
        {
            auto** c = property<UObject*>(Cached::car(), STR("Chassis"));
            return c ? *c : nullptr;
        }

        UObject* attach_parent(UObject* component)
        {
            UFunctionCall fn(component, STR("GetAttachParent"));
            auto* ret = fn ? fn.param<UObject*>(STR("ReturnValue")) : nullptr;
            if (!ret) return nullptr;
            fn.call();
            return *ret;
        }

        template <typename T>
        bool call_get(UObject* object, const TCHAR* name, T& out)
        {
            UFunctionCall fn(object, name);
            auto* ret = fn ? fn.param<T>(STR("ReturnValue")) : nullptr;
            if (!ret) return false;
            fn.call();
            out = *ret;
            return true;
        }

        void remove_leech(UObject* actor)
        {
            if (auto** instance = property<UObject*>(actor, STR("Instance")); instance && *instance)
            {
                UFunctionCall remove(*instance, STR("Remove"));
                if (auto* p = remove ? remove.param<bool>(STR("bRemoveActors")) : nullptr) *p = true, remove.call();
            }
            UFunctionCall destroy(actor, STR("K2_DestroyActor"));
            if (destroy) destroy.call();
        }

        // Марионетка: без машины состояний, тика и физики стоит, где поставили (мост 2026-11-17)
        void puppetize(UObject* actor)
        {
            if (auto** sm = property<UObject*>(actor, STR("SMStateMachine")); sm && *sm)
                if (UFunctionCall stop(*sm, STR("Stop")); stop) stop.call();
            UFunctionCall tick(actor, STR("SetActorTickEnabled"));
            if (auto* p = tick ? tick.param<bool>(STR("bEnabled")) : nullptr) *p = false, tick.call();
            if (UObject* body = body_of(actor))
            {
                UFunctionCall sim(body, STR("SetSimulatePhysics"));
                if (auto* p = sim ? sim.param<bool>(STR("bSimulate")) : nullptr) *p = false, sim.call();
            }
        }

        // ---- провод ----
        template <typename T>
        void put(std::vector<uint8_t>& out, const T& v)
        {
            auto* p = reinterpret_cast<const uint8_t*>(&v);
            out.insert(out.end(), p, p + sizeof(T));
        }
        template <typename T>
        bool get(const uint8_t*& p, const uint8_t* end, T& v)
        {
            if (end - p < static_cast<std::ptrdiff_t>(sizeof(T))) return false;
            std::memcpy(&v, p, sizeof(T));
            p += sizeof(T);
            return true;
        }
        void put_rot(std::vector<uint8_t>& out, const Rot& r) { put(out, Coop::pack_angle(r.Pitch)), put(out, Coop::pack_angle(r.Yaw)), put(out, Coop::pack_angle(r.Roll)); }
        bool get_rot(const uint8_t*& p, const uint8_t* end, Rot& r)
        {
            int16_t a, b, c;
            if (!get(p, end, a) || !get(p, end, b) || !get(p, end, c)) return false;
            r = {Coop::unpack_angle(a), Coop::unpack_angle(b), Coop::unpack_angle(c)};
            return true;
        }

        Hook::GlobalCallbackId g_begin_play{};
        bool g_is_host = false, g_loading = false, g_spawning = false;
        uint64_t g_loaded_ms = 0;

        // ---- Host ----
        struct HostLeech
        {
            FWeakObjectPtr Actor;
            uint16_t Id;
            bool Announced = false;
        };
        std::vector<HostLeech> g_host;
        uint16_t g_next_id = 1;
        uint64_t g_state_ms = 0, g_log_ms = 0;

        // ---- Client ----
        struct Copy
        {
            FWeakObjectPtr Actor;
            bool HaveTarget = false, OnChassis = false;
            Vec Loc{};
            Rot Rotation{};
        };
        std::map<uint16_t, Copy> g_copies;
        struct PendingSpawn
        {
            uint16_t Id;
            Vec Loc;
            Rot Rotation;
            std::string Path;
        };
        std::vector<PendingSpawn> g_pending;     // пришли до готовности карты
        std::vector<FWeakObjectPtr> g_own;       // свои пиявки Client — убрать на тике (не внутри BeginPlay)
        bool g_hello_sent = false;
        uint64_t g_tick_ms = 0;

        bool client_ready() { return !g_loading && g_loaded_ms && now_ms() - g_loaded_ms >= HelloAfterMs && Net::connected(); }

        // ---- этап 3: действия Client с марионеткой (байткод BP_Hazard_Leech, 2026-11-18) ----
        // Почти все действия сходятся в KnockOff (пинок, монтировка, освободитель, молот, взрыв, скрапер, TryDIslodge); смерть —
        // TakeDamage компонента здоровья → OnHealthZero. Подклассы их не переопределяют — перехват на функции базового класса ловит всех.
        constexpr const TCHAR* KnockOffFn = STR("KnockOff");
        constexpr const TCHAR* DiedFn = STR("BndEvt__BP_HealthComponent_K2Node_ComponentBoundEvent_4_OnHealthZero__DelegateSignature");
        constexpr const TCHAR* LeechClassPath = STR("/Game/Gameplay/Hazards/Leech/BP_Hazard_Leech.BP_Hazard_Leech_C");
        constexpr float NearCm = 450; // подобрал Client: копия пропала рядом с ним (как WorldItemSync — «из рук игрока»)
        bool g_action_hooks = false;
        uint64_t g_hook_try_ms = 0;
        std::vector<UObject*> g_dropped; // Client: свои пиявки, выброшенные из рук (OnDropThrowItem) — отдать Host; только сравнение адресов

        bool player_location(Vec& out)
        {
            static Cached::First player{STR("PDP_MainCharacter_C")};
            return call_get(player.get(), STR("K2_GetActorLocation"), out);
        }

        void send_action(UObject* actor, uint8_t action)
        {
            if (g_is_host || !actor || !Net::connected()) return;
            for (auto& [id, c] : g_copies)
            {
                if (c.Actor.Get() != actor) continue;
                std::vector<uint8_t> body;
                put(body, id);
                body.push_back(action);
                Net::send(Coop::LeechAction, body);
                Output::send<LogLevel::Verbose>(STR("[Leech] марионетка #{} {} мной → прошу Host\n"), id, action == Coop::LeechKill ? STR("убита") : STR("сбита"));
                return;
            }
        }

        // Ставим, как только загружен класс пиявки (раз в секунду пробуем). Функции базового класса — подклассы их не переопределяют
        void install_action_hooks()
        {
            uint64_t now = now_ms();
            if (g_action_hooks || now - g_hook_try_ms < 1000) return;
            g_hook_try_ms = now;
            auto* cls = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, LeechClassPath);
            if (!cls) return; // пиявок на карте ещё не было
            g_action_hooks = true;
            using Post = void (*)(UnrealScriptFunctionCallableContext&, void*);
            struct H
            {
                const TCHAR* Name;
                Post Fn;
            };
            const H hooks[] = {
                    {KnockOffFn, [](UnrealScriptFunctionCallableContext& ctx, void*) { send_action(ctx.Context, Coop::LeechKnockOff); }},
                    {DiedFn, [](UnrealScriptFunctionCallableContext& ctx, void*) { send_action(ctx.Context, Coop::LeechKill); }},
                    // бросок из рук: у новой пиявки в мире (BeginPlay уже был) — отдаём Host на ближайшем тике
                    {STR("OnDropThrowItem"), [](UnrealScriptFunctionCallableContext& ctx, void*) {
                         if (!g_is_host && Net::connected() && ctx.Context) g_dropped.push_back(ctx.Context);
                     }},
            };
            for (auto& h : hooks)
            {
                if (!cls->GetFunctionByNameInChain(h.Name))
                {
                    Output::send<LogLevel::Warning>(STR("[Leech] нет функции {} у пиявки — это действие Client до Host не дойдёт\n"), h.Name);
                    continue;
                }
                UObjectGlobals::RegisterHook(StringType(LeechClassPath) + STR(":") + h.Name, [](UnrealScriptFunctionCallableContext&, void*) {}, h.Fn, nullptr);
                Output::send<LogLevel::Verbose>(STR("[Leech] перехват {} установлен\n"), h.Name);
            }
        }

        // Client выбросил свою пиявку из рук → Host создаёт настоящую (её он и поведёт), у себя убираем
        void give_to_host(UObject* actor)
        {
            UObject* body = body_of(actor);
            auto** instance = property<UObject*>(actor, STR("Instance"));
            std::string path = instance && *instance ? Items::archetype_path(*instance) : std::string{};
            Vec loc{};
            Rot rot{};
            if (!body || path.empty() || !call_get(body, STR("K2_GetComponentLocation"), loc) || !call_get(body, STR("K2_GetComponentRotation"), rot)) return;
            std::vector<uint8_t> spawn;
            put(spawn, uint16_t{0}), put(spawn, loc), put_rot(spawn, rot);
            spawn.insert(spawn.end(), path.begin(), path.end());
            Net::send(Coop::LeechSpawn, spawn);
            Output::send<LogLevel::Verbose>(STR("[Leech] я выбросил пиявку {} у ({:.0f}, {:.0f}, {:.0f}) → Host создаст её у себя\n"), actor->GetClassPrivate()->GetName(), loc.X, loc.Y, loc.Z);
        }

        void on_begin_play(AActor* actor)
        {
            if (!actor || g_spawning || !is_leech(actor)) return;
            if (g_is_host)
            {
                g_host.push_back({FWeakObjectPtr(actor), g_next_id++});
                return;
            }
            if (Net::connected()) g_own.emplace_back(actor);
        }

        // FQuat ← FRotator, как FRotator::Quaternion в UE4 (то же, что в AnchorSync)
        void transform_of(const Vec& l, const Rot& r, float tf[12])
        {
            constexpr float H = 3.14159265f / 360.f;
            float sp = std::sin(r.Pitch * H), cp = std::cos(r.Pitch * H), sy = std::sin(r.Yaw * H), cy = std::cos(r.Yaw * H), sr = std::sin(r.Roll * H),
                  cr = std::cos(r.Roll * H);
            float out[12] = {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy, l.X, l.Y, l.Z, 0, 1, 1, 1, 0};
            std::memcpy(tf, out, sizeof out);
        }

        void create_copy(const PendingSpawn& s)
        {
            if (g_copies.count(s.Id)) return;
            float tf[12];
            transform_of(s.Loc, s.Rotation, tf);
            g_spawning = true;
            auto* instance = static_cast<UObject*>(WorldItemSync::spawn_in_world(s.Path, tf, 0));
            g_spawning = false;
            UObject* actor = nullptr;
            if (instance) call_get(instance, STR("GetActorManifestation"), actor);
            if (actor) puppetize(actor);
            g_copies[s.Id] ={FWeakObjectPtr(actor), false, false, s.Loc, s.Rotation};
            Output::send<LogLevel::Verbose>(STR("[Leech] пиявка Host #{} {} у ({:.0f}, {:.0f}, {:.0f}): {}\n"), s.Id, Items::wide(s.Path.substr(s.Path.rfind('.') + 1)), s.Loc.X,
                                            s.Loc.Y, s.Loc.Z, actor ? STR("марионетка создана") : instance ? STR("предмет есть, актёра НЕТ") : STR("НЕ СОЗДАНА"));
        }

        void host_tick()
        {
            uint64_t now = now_ms();
            if (!Net::connected() || g_loading || now - g_state_ms < StateMs) return;
            g_state_ms = now;
            UObject* car_chassis = chassis();
            std::vector<uint8_t> state;
            uint8_t count = 0;
            state.push_back(0);
            for (size_t i = 0; i < g_host.size();)
            {
                HostLeech& l = g_host[i];
                UObject* actor = l.Actor.Get();
                UObject* body = actor ? body_of(actor) : nullptr;
                if (!body)
                {
                    if (l.Announced)
                    {
                        std::vector<uint8_t> gone;
                        put(gone, l.Id);
                        Net::send(Coop::LeechGone, gone);
                        Output::send<LogLevel::Verbose>(STR("[Leech] моей пиявки #{} больше нет → сообщаю\n"), l.Id);
                    }
                    g_host.erase(g_host.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
                Vec loc{};
                Rot rot{};
                bool on_chassis = car_chassis && attach_parent(body) == car_chassis;
                if (on_chassis)
                {
                    auto* rl = property<Vec>(body, STR("RelativeLocation"));
                    auto* rr = property<Rot>(body, STR("RelativeRotation"));
                    if (!rl || !rr) continue;
                    loc = *rl, rot = *rr;
                }
                else if (!call_get(body, STR("K2_GetComponentLocation"), loc) || !call_get(body, STR("K2_GetComponentRotation"), rot)) continue;
                if (!l.Announced)
                {
                    auto** instance = property<UObject*>(actor, STR("Instance"));
                    std::string path = instance && *instance ? Items::archetype_path(*instance) : std::string{};
                    if (path.empty()) continue;
                    Vec world = loc;
                    Rot world_rot = rot;
                    if (on_chassis) call_get(body, STR("K2_GetComponentLocation"), world), call_get(body, STR("K2_GetComponentRotation"), world_rot);
                    std::vector<uint8_t> spawn;
                    put(spawn, l.Id), put(spawn, world), put_rot(spawn, world_rot);
                    spawn.insert(spawn.end(), path.begin(), path.end());
                    Net::send(Coop::LeechSpawn, spawn);
                    l.Announced = true;
                    Output::send<LogLevel::Verbose>(STR("[Leech] моя пиявка #{} {} у ({:.0f}, {:.0f}, {:.0f}) → Client\n"), l.Id, actor->GetClassPrivate()->GetName(), world.X,
                                                    world.Y, world.Z);
                }
                if (count == MaxPerPacket) continue;
                put(state, l.Id);
                state.push_back(on_chassis ? Coop::LeechOnChassis : 0);
                put(state, loc), put_rot(state, rot);
                ++count;
            }
            if (!count) return;
            state[0] = count;
            Net::send(Coop::LeechState, state, 1); // последнее побеждает
            if (now - g_log_ms >= 10'000) g_log_ms = now, Output::send<LogLevel::Verbose>(STR("[Leech] веду пиявок: {}\n"), count);
        }

        void apply_copy(Copy& c, UObject* actor, float dt)
        {
            UObject* body = body_of(actor);
            if (!body || !c.HaveTarget) return;
            UObject* parent = attach_parent(body);
            if (c.OnChassis)
            {
                UObject* car_chassis = chassis();
                if (!car_chassis) return;
                if (parent != car_chassis)
                {
                    UFunctionCall attach(body, STR("K2_AttachToComponent"));
                    auto* p_parent = attach ? attach.param<UObject*>(STR("Parent")) : nullptr;
                    auto* p_loc = attach ? attach.param<uint8_t>(STR("LocationRule")) : nullptr;
                    auto* p_rot = attach ? attach.param<uint8_t>(STR("RotationRule")) : nullptr;
                    auto* p_scale = attach ? attach.param<uint8_t>(STR("ScaleRule")) : nullptr;
                    if (!p_parent || !p_loc || !p_rot || !p_scale) return;
                    *p_parent = car_chassis;
                    *p_loc = *p_rot = *p_scale = 0; // EAttachmentRule::KeepRelative — относительное место ставим сами ниже
                    attach.call();
                }
                UFunctionCall set(body, STR("K2_SetRelativeLocationAndRotation"));
                auto* p_loc = set ? set.param<Vec>(STR("NewLocation")) : nullptr;
                auto* p_rot = set ? set.param<Rot>(STR("NewRotation")) : nullptr;
                auto* p_tp = set ? set.param<bool>(STR("bTeleport")) : nullptr;
                if (p_loc && p_rot && p_tp) *p_loc = c.Loc, *p_rot = c.Rotation, *p_tp = true, set.call();
                return;
            }
            if (parent)
            {
                UFunctionCall detach(body, STR("K2_DetachFromComponent"));
                auto* p_loc = detach ? detach.param<uint8_t>(STR("LocationRule")) : nullptr;
                auto* p_rot = detach ? detach.param<uint8_t>(STR("RotationRule")) : nullptr;
                auto* p_scale = detach ? detach.param<uint8_t>(STR("ScaleRule")) : nullptr;
                if (p_loc && p_rot && p_scale) *p_loc = *p_rot = *p_scale = 1, detach.call(); // EDetachmentRule::KeepWorld
            }
            Vec cur{};
            if (!call_get(body, STR("K2_GetComponentLocation"), cur)) return;
            float dx = c.Loc.X - cur.X, dy = c.Loc.Y - cur.Y, dz = c.Loc.Z - cur.Z;
            float k = (dx * dx + dy * dy + dz * dz > 500.f * 500.f) ? 1.f : 1.f - std::exp(-dt / SmoothTau); // далеко (прыжок, отцеп) — сразу
            Vec to{cur.X + dx * k, cur.Y + dy * k, cur.Z + dz * k};
            UFunctionCall set(body, STR("K2_SetWorldLocationAndRotation"));
            auto* p_loc = set ? set.param<Vec>(STR("NewLocation")) : nullptr;
            auto* p_rot = set ? set.param<Rot>(STR("NewRotation")) : nullptr;
            auto* p_tp = set ? set.param<bool>(STR("bTeleport")) : nullptr;
            if (p_loc && p_rot && p_tp) *p_loc = to, *p_rot = c.Rotation, *p_tp = true, set.call();
        }

        void client_tick()
        {
            uint64_t now = now_ms();
            float dt = g_tick_ms ? static_cast<float>(now - g_tick_ms) / 1000.f : 0.f;
            g_tick_ms = now;
            if (!Net::connected())
            {
                g_hello_sent = false;
                g_own.clear();
                return;
            }
            install_action_hooks();
            for (auto& w : g_own)
                if (UObject* a = w.Get())
                {
                    if (std::find(g_dropped.begin(), g_dropped.end(), a) != g_dropped.end()) give_to_host(a);
                    else Output::send<LogLevel::Verbose>(STR("[Leech] своя пиявка {} убрана — пиявок ведёт Host\n"), a->GetClassPrivate()->GetName());
                    remove_leech(a);
                }
            g_own.clear();
            g_dropped.clear();
            if (!client_ready()) return;
            if (!g_hello_sent)
            {
                g_hello_sent = true;
                for (auto& [id, c] : g_copies)
                    if (UObject* a = c.Actor.Get()) remove_leech(a);
                g_copies.clear();
                Net::send(Coop::LeechHello, {});
                Output::send<LogLevel::Verbose>(STR("[Leech] карта готова — прошу у Host всех пиявок\n"));
            }
            for (auto& s : g_pending) create_copy(s);
            g_pending.clear();
            for (auto it = g_copies.begin(); it != g_copies.end();)
            {
                UObject* actor = it->second.Actor.Get();
                if (!actor)
                {
                    // Пропала не по нашей команде: рядом со мной — я её подобрал (Host убирает свою); далеко — выгрузка,
                    // Host пришлёт снова только после LeechHello. shortcut: «подобрал» по расстоянию, перехвата подбора нет (натив)
                    const Copy& c = it->second;
                    Vec me{};
                    if (c.HaveTarget && !c.OnChassis && player_location(me))
                    {
                        float dx = c.Loc.X - me.X, dy = c.Loc.Y - me.Y, dz = c.Loc.Z - me.Z;
                        if (dx * dx + dy * dy + dz * dz < NearCm * NearCm)
                        {
                            std::vector<uint8_t> body;
                            put(body, it->first);
                            body.push_back(Coop::LeechTake);
                            Net::send(Coop::LeechAction, body);
                            Output::send<LogLevel::Verbose>(STR("[Leech] марионетка #{} пропала рядом со мной (подобрал) → прошу Host убрать свою\n"), it->first);
                        }
                    }
                    it = g_copies.erase(it);
                    continue;
                }
                apply_copy(it->second, actor, dt);
                ++it;
            }
        }
    } // namespace

    void install()
    {
        g_begin_play = Hook::RegisterBeginPlayPostCallback([](Hook::TCallbackIterationData<void>&, AActor* actor) { on_begin_play(actor); },
                                                           {false, false, STR("PacificDriveCoop"), STR("Leeches")});
        Output::send<LogLevel::Verbose>(STR("[Leech] перехват появления пиявок {}\n"), g_begin_play ? STR("установлен") : STR("НЕ УСТАНОВЛЕН"));
    }

    void tick(bool is_host)
    {
        g_is_host = is_host;
        is_host ? host_tick() : client_tick();
    }

    void on_map_loading()
    {
        g_loading = true;
        g_host.clear(); // актёры старой карты; пиявки новой придут через BeginPlay (и из сейва тоже)
        g_copies.clear();
        g_pending.clear();
        g_own.clear();
        g_hello_sent = false;
    }

    void on_map_loaded()
    {
        g_loading = false;
        g_loaded_ms = now_ms();
    }

    void on_spawn(const uint8_t* body, size_t size)
    {
        const uint8_t *p = body, *end = body + size;
        PendingSpawn s{};
        if (!get(p, end, s.Id) || !get(p, end, s.Loc) || !get_rot(p, end, s.Rotation) || p == end) return;
        s.Path.assign(reinterpret_cast<const char*>(p), static_cast<size_t>(end - p));
        if (g_is_host)
        {
            // Client выбросил пиявку из рук: создаём настоящую — BeginPlay внесёт её в g_host, и Client получит её марионеткой
            if (s.Path.find("IA_Hazard_Leech") == std::string::npos) return; // только пиявки: пакет от Client — не доверяем пути
            float tf[12];
            transform_of(s.Loc, s.Rotation, tf);
            bool ok = WorldItemSync::spawn_in_world(s.Path, tf, 0) != nullptr;
            Output::send<LogLevel::Verbose>(STR("[Leech] пассажир выбросил пиявку {} у ({:.0f}, {:.0f}, {:.0f}) → у меня {}\n"), Items::wide(s.Path.substr(s.Path.rfind('.') + 1)),
                                            s.Loc.X, s.Loc.Y, s.Loc.Z, ok ? STR("создана") : STR("НЕ СОЗДАНА"));
            return;
        }
        if (client_ready() && g_hello_sent) create_copy(s);
        else g_pending.push_back(std::move(s));
    }

    void on_state(const uint8_t* body, size_t size)
    {
        const uint8_t *p = body, *end = body + size;
        uint8_t count = 0;
        if (!get(p, end, count)) return;
        for (uint8_t i = 0; i < count; ++i)
        {
            uint16_t id;
            uint8_t flags;
            Vec loc;
            Rot rot;
            if (!get(p, end, id) || !get(p, end, flags) || !get(p, end, loc) || !get_rot(p, end, rot)) return;
            auto it = g_copies.find(id);
            if (it == g_copies.end()) continue;
            it->second.HaveTarget = true;
            it->second.OnChassis = flags & Coop::LeechOnChassis;
            it->second.Loc = loc;
            it->second.Rotation = rot;
        }
    }

    void on_gone(const uint8_t* body, size_t size)
    {
        const uint8_t *p = body, *end = body + size;
        uint16_t id;
        if (!get(p, end, id)) return;
        std::erase_if(g_pending, [id](const PendingSpawn& s) { return s.Id == id; }); // ещё не создана — и не создавать
        auto it = g_copies.find(id);
        if (it == g_copies.end()) return;
        if (UObject* a = it->second.Actor.Get()) remove_leech(a);
        g_copies.erase(it);
        Output::send<LogLevel::Verbose>(STR("[Leech] пиявки Host #{} больше нет — убрал марионетку\n"), id);
    }

    void on_action(const uint8_t* body, size_t size)
    {
        const uint8_t *p = body, *end = body + size;
        uint16_t id;
        uint8_t action;
        if (!get(p, end, id) || !get(p, end, action)) return;
        auto it = std::find_if(g_host.begin(), g_host.end(), [id](const HostLeech& l) { return l.Id == id; });
        UObject* actor = it != g_host.end() ? it->Actor.Get() : nullptr;
        if (!actor)
        {
            Output::send<LogLevel::Verbose>(STR("[Leech] пассажир просит про пиявку #{}, у меня её уже нет\n"), id);
            return;
        }
        bool done = false;
        if (action == Coop::LeechTake) remove_leech(actor), done = true; // подобрал Client: пиявка теперь у него в руках
        else if (action == Coop::LeechKnockOff)
        {
            UFunctionCall fn(actor, KnockOffFn);
            if ((done = static_cast<bool>(fn))) fn.call();
        }
        else if (action == Coop::LeechKill)
        {
            // Как игра: TakeDamage рассылает OnHealthZero и вызывает HandleHealthZero (смерть, предмет-туша). Остальные параметры — нули
            auto** health = property<UObject*>(actor, STR("BP_HealthComponent"));
            UFunctionCall fn(health ? *health : nullptr, STR("TakeDamage"));
            auto* amount = fn ? fn.param<float>(STR("Amount")) : nullptr;
            auto* ignore = fn ? fn.param<bool>(STR("IgnoreMagicHealthPoint")) : nullptr;
            if ((done = amount && ignore)) *amount = 1e6f, *ignore = true, fn.call();
        }
        Output::send<LogLevel::Verbose>(STR("[Leech] пассажир {} пиявку #{} → у меня {}\n"),
                                        action == Coop::LeechKill ? STR("убил") : action == Coop::LeechTake ? STR("подобрал") : STR("сбил"), id,
                                        done ? STR("повторено") : STR("НЕ ВЫШЛО (нет функции/параметров)"));
    }

    void on_hello(const uint8_t*, size_t)
    {
        for (auto& l : g_host) l.Announced = false; // пришлём всех заново на ближайшем тике
        Output::send<LogLevel::Verbose>(STR("[Leech] Client готов — шлю всех пиявок ({})\n"), g_host.size());
    }
} // namespace LeechSync
