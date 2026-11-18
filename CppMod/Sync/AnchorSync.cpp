#include "AnchorSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace AnchorSync
{
    namespace
    {
        constexpr uint64_t PollMs = 300, SettleMs = 8000;

        struct Vec
        {
            float X, Y, Z;
        };

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        uint64_t g_loaded_ms = 0, g_last_poll_ms = 0;
        bool g_is_host = false;
        std::map<UObject*, bool> g_taken; // основание → якорь уже взят (на прошлом опросе); только для сравнения

        std::vector<UObject*> bases()
        {
            std::vector<UObject*> out;
            UObjectGlobals::FindAllOf(STR("BP_Spawn_Anchor_Base_01_C"), out);
            return out;
        }

        bool taken(UObject* base)
        {
            auto* flag = property<bool>(base, STR("AnchorRemoved"));
            auto** instance = property<UObject*>(base, STR("ItemInstance"));
            return (flag && *flag) || !instance || !*instance;
        }

        Vec location(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {NAN, NAN, NAN};
            fn.call();
            return *ret;
        }

        // Якорь у собеседника взят — у нас его больше нет: экземпляр убрать, флаг поднять
        void take_here(UObject* base)
        {
            if (auto** instance = property<UObject*>(base, STR("ItemInstance")); instance && *instance)
            {
                UFunctionCall remove(*instance, STR("Remove"));
                if (auto* actors = remove ? remove.param<bool>(STR("bRemoveActors")) : nullptr)
                {
                    *actors = true;
                    remove.call();
                }
            }
            if (auto* flag = property<bool>(base, STR("AnchorRemoved"))) *flag = true;
            if (auto* grabbed = property<bool>(base, STR("SuccessfullyGrabbed"))) *grabbed = true;
            // Взятие идёт через Highlight основания и выдаёт новый предмет из ItemToGrab (тест 2026-11-06 08:24: после снятия
            // ItemInstance Host всё равно взял якорь) — убираем точку взятия, сам якорь и луч; «вилка» (SM_Anchor_Base) остаётся.
            // Тест 2026-11-07: оставались свечение и метка на карте гоблера — это свет/FX основания и MapIcon/MapIconCenter
            constexpr const TCHAR* Parts[] = {STR("Highlight"), STR("GrabbableMesh"), STR("NS_AnchorBeam"), STR("NS_Anchor_Ambient"), STR("MapIcon"),
                                              STR("MapIconCenter"), STR("PointLight"), STR("DetailLight_Large"), STR("DetailLight_Med"), STR("DetailLight_Small"),
                                              STR("NS_AnchorBase_FX_Large"), STR("NS_AnchorBase_FX_Medium"), STR("NS_AnchorBase_FX_Small")};
            int removed = 0;
            for (auto* name : Parts)
            {
                auto** comp = property<UObject*>(base, name);
                if (!comp || !*comp) continue;
                UFunctionCall destroy(*comp, STR("K2_DestroyComponent"));
                if (auto* obj = destroy ? destroy.param<UObject*>(STR("Object")) : nullptr)
                {
                    *obj = *comp;
                    destroy.call();
                    ++removed;
                }
            }
            // Из AnchorTaken() (байткод BP_Spawn_Anchor_Base_01, 2026-11-18) без возмущения зоны: объём радиации вокруг якоря
            // (RadActor, BP_VolCascade_Radiation — иначе у собеседника радиация у пустой вилки остаётся) и тег «радар якорей не видит»
            bool rad = false;
            if (auto** rad_actor = property<UObject*>(base, STR("RadActor")); rad_actor && *rad_actor)
            {
                UFunctionCall destroy(*rad_actor, STR("K2_DestroyActor"));
                if ((rad = static_cast<bool>(destroy))) destroy.call();
                *rad_actor = nullptr;
            }
            UFunctionCall tag(base, STR("AddAnchorPulledTag"));
            if (tag) tag.call();
            Output::send<LogLevel::Verbose>(STR("[Anchor] у основания убрано компонентов: {} из {}, радиация {}, тег радара {}\n"), removed, std::size(Parts),
                                            rad ? STR("убрана") : STR("нет"), tag ? STR("поставлен") : STR("НЕТ функции"));
            g_taken[base] = true; // не принять за своё взятие
            if (!g_is_host) return;
            // Host решает за зону: «возмущение» от взятия якоря (аномалии) — как если бы взял он сам.
            // У BP_HazardManager_C CauseHazardDisturbance — делегат, не функция (тест 2026-11-07); спавнит BP_HazardAnchorPull_C.HazardDisturbance
            auto* type = property<uint8_t>(base, STR("AnchorType"));
            auto* size = property<uint8_t>(base, STR("AnchorSize"));
            UObject* pull = UObjectGlobals::FindFirstOf(STR("BP_HazardAnchorPull_C"));
            UFunctionCall fn(pull, STR("HazardDisturbance"));
            auto* p_type = fn ? fn.param<uint8_t>(STR("Type")) : nullptr;
            auto* p_size = fn ? fn.param<uint8_t>(STR("Size")) : nullptr;
            if (!p_type || !p_size || !type || !size)
            {
                Output::send<LogLevel::Warning>(STR("[Anchor] возмущение зоны не вызвано: BP_HazardAnchorPull_C {}, функция {}, параметры Type/Size {}/{}, у основания AnchorType/AnchorSize {}/{}\n"),
                                                pull ? STR("есть") : STR("НЕТ"), fn ? STR("есть") : STR("НЕТ"), p_type ? STR("есть") : STR("НЕТ"),
                                                p_size ? STR("есть") : STR("НЕТ"), type ? STR("есть") : STR("НЕТ"), size ? STR("есть") : STR("НЕТ"));
                return;
            }
            *p_type = *type;
            *p_size = *size;
            fn.call();
            Output::send<LogLevel::Verbose>(STR("[Anchor] возмущение зоны вызвано: тип {}, размер {}\n"), *type, *size);
        }

        // Порталы выхода (BP_LevelTransition_ExitPortal_C) тоже неподвижны и пришли в сейве Host — опознаются по позиции.
        // Какой флаг значит «активирован», пока не разведано (тест 2026-11-07): пишем в лог смену всех четырёх, активным
        // считаем любой из трёх первых. shortcut: признак по догадке, уточнить по логу следующего теста
        constexpr const TCHAR* PortalFlags[] = {STR("Active"), STR("DataBlock_IsActive"), STR("DataBlock_IsOpened"), STR("IsPickedExit")};
        std::map<UObject*, uint8_t> g_portal;   // портал → биты PortalFlags на прошлом опросе
        std::map<UObject*, bool> g_portal_echo; // активирован по слову собеседника — не слать обратно

        std::vector<UObject*> portals()
        {
            std::vector<UObject*> out;
            UObjectGlobals::FindAllOf(STR("BP_LevelTransition_ExitPortal_C"), out);
            return out;
        }

        uint8_t portal_bits(UObject* portal)
        {
            uint8_t bits = 0;
            for (size_t i = 0; i < std::size(PortalFlags); ++i)
                if (auto* f = property<bool>(portal, PortalFlags[i]); f && *f) bits |= 1u << i;
            return bits;
        }

        bool portal_active(uint8_t bits) { return bits & 0b0111; }

        UObject* nearest(const std::vector<UObject*>& actors, const Coop::AnchorBody& body)
        {
            UObject* best = nullptr;
            float best_d2 = static_cast<float>(Coop::AnchorMatchCm) * Coop::AnchorMatchCm;
            for (auto* a : actors)
            {
                Vec p = location(a);
                float dx = p.X - body.X, dy = p.Y - body.Y, dz = p.Z - body.Z, d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best_d2) best = a, best_d2 = d2;
            }
            return best;
        }

        void on_portal(const Coop::AnchorBody& body)
        {
            UObject* portal = nearest(portals(), body);
            if (!portal)
            {
                Output::send<LogLevel::Verbose>(STR("[Portal] собеседник активировал портал у ({}, {}, {}), у меня там портала нет\n"), body.X, body.Y, body.Z);
                return;
            }
            uint8_t bits = portal_bits(portal);
            if (portal_active(bits))
            {
                Output::send<LogLevel::Verbose>(STR("[Portal] портал у ({}, {}, {}) у меня уже активен (флаги {:04b})\n"), body.X, body.Y, body.Z, bits);
                return;
            }
            g_portal_echo[portal] = true;
            UFunctionCall fn(portal, STR("Activate"));
            if (fn) fn.call();
            Output::send<LogLevel::Verbose>(STR("[Portal] собеседник активировал портал у ({}, {}, {}) → Activate() {}, флаги {:04b} → {:04b}\n"), body.X, body.Y, body.Z,
                                            fn ? STR("вызван") : STR("НЕ НАЙДЕН"), bits, portal_bits(portal));
        }

        void tick_portals(bool settled)
        {
            for (auto* portal : portals())
            {
                uint8_t bits = portal_bits(portal);
                auto it = g_portal.find(portal);
                if (it == g_portal.end())
                {
                    g_portal[portal] = bits;
                    Vec p = location(portal);
                    Output::send<LogLevel::Verbose>(STR("[Portal] портал у ({:.0f}, {:.0f}, {:.0f}): флаги {:04b} (IsPickedExit/IsOpened/IsActive/Active)\n"),
                                                    static_cast<double>(p.X), static_cast<double>(p.Y), static_cast<double>(p.Z), bits);
                    continue;
                }
                if (bits == it->second) continue;
                Vec p = location(portal);
                Output::send<LogLevel::Verbose>(STR("[Portal] портал у ({:.0f}, {:.0f}, {:.0f}): флаги {:04b} → {:04b}\n"), static_cast<double>(p.X),
                                                static_cast<double>(p.Y), static_cast<double>(p.Z), it->second, bits);
                bool became_active = portal_active(bits) && !portal_active(it->second);
                it->second = bits;
                if (!became_active || !settled || !Net::connected() || g_portal_echo.erase(portal) || std::isnan(p.X)) continue;
                Net::send_anchor({static_cast<int32_t>(std::lround(p.X)), static_cast<int32_t>(std::lround(p.Y)), static_cast<int32_t>(std::lround(p.Z))},
                                 Coop::PortalActive);
                Output::send<LogLevel::Verbose>(STR("[Portal] я активировал портал → сообщаю собеседнику\n"));
            }
        }
        // Плазменный генератор (docs/recon/reference/gameplay-events.md): ломается при появлении — RandomState (у каждого ПК свои сиды) против
        // ChanceToBreakOnSpawn — и всегда при сборе плазмы (OnHarvested). Кто увидел новый сломанный — шлёт позицию (GeneratorBroken),
        // второй ломает свой: Broken = true + BeginPlay_FromLoad (ветка «сломан» из сейва: модель, свет, подсветка, звук, свойства предмета —
        // проверено через мост 2026-11-17). Client сам при появлении не ломает (шанс 0 до BeginPlay) — поломки при появлении решает Host
        constexpr uint64_t GeneratorPollMs = 2000;
        uint64_t g_gen_poll_ms = 0;
        std::map<UObject*, bool> g_gen_broken;         // генератор → сломан на прошлом опросе
        std::vector<Coop::AnchorBody> g_gen_wanted;    // присланные поломки, генератора ещё нет (Client получает зону раньше, чем доспавнит)

        bool is_generator(UObject* actor) { return actor->GetClassPrivate()->GetName() == STR("BP_Spawn_Resource_Plasma_PlasmaGenerator_C"); }

        std::vector<UObject*> generators()
        {
            std::vector<UObject*> out;
            UObjectGlobals::FindAllOf(STR("BP_Spawn_Resource_Plasma_PlasmaGenerator_C"), out);
            return out;
        }

        bool generator_broken(UObject* g)
        {
            auto* broken = property<bool>(g, STR("Broken"));
            return broken && *broken;
        }

        void break_generator(UObject* g)
        {
            g_gen_broken[g] = true; // не принять за свою поломку
            if (auto* broken = property<bool>(g, STR("Broken"))) *broken = true;
            UFunctionCall show(g, STR("BeginPlay_FromLoad"));
            if (show) show.call();
            Vec p = location(g);
            Output::send<LogLevel::Verbose>(STR("[Generator] генератор у ({:.0f}, {:.0f}, {:.0f}) сломан у собеседника → ломаю у себя: {}\n"), static_cast<double>(p.X),
                                            static_cast<double>(p.Y), static_cast<double>(p.Z), show ? STR("готово") : STR("нет BeginPlay_FromLoad"));
        }

        void tick_generators(bool settled, uint64_t now)
        {
            if (!Net::connected())
            {
                g_gen_broken.clear(); // после подключения Host перешлёт все сломанные заново
                return;
            }
            if (now - g_gen_poll_ms < GeneratorPollMs) return;
            g_gen_poll_ms = now;
            std::vector<UObject*> all = generators();
            for (size_t i = 0; i < g_gen_wanted.size();)
            {
                UObject* g = nearest(all, g_gen_wanted[i]);
                if (!g)
                {
                    ++i;
                    continue;
                }
                if (!generator_broken(g)) break_generator(g);
                g_gen_wanted.erase(g_gen_wanted.begin() + static_cast<std::ptrdiff_t>(i));
            }
            if (!settled) return;
            for (UObject* g : all)
            {
                bool broken = generator_broken(g);
                bool& was = g_gen_broken[g]; // новый в учёте — false: сломанный при появлении тоже уходит собеседнику
                if (!broken || was) continue;
                was = true;
                Vec p = location(g);
                if (std::isnan(p.X)) continue;
                Net::send_anchor({static_cast<int32_t>(std::lround(p.X)), static_cast<int32_t>(std::lround(p.Y)), static_cast<int32_t>(std::lround(p.Z))},
                                 Coop::GeneratorBroken);
                Output::send<LogLevel::Verbose>(STR("[Generator] генератор у ({:.0f}, {:.0f}, {:.0f}) сломан → сообщаю собеседнику\n"), static_cast<double>(p.X),
                                                static_cast<double>(p.Y), static_cast<double>(p.Z));
            }
        }

        // Аномалии от взятия якоря: у каждого ПК свой BP_HazardAnchorPull_C спавнит случайный набор (тесты 0.17.6/0.17.7):
        // Bollard_AnchorPull, Movement_Prong_AnchorSpawn и радиационные пятна RadiatedZone_SpawnEvent (TryRadSpotSpawn).
        // Перехват GetNextSpawnLocation не годится (0.17.7: позиция читалась нулём, функция зовётся по нескольку раз на одну аномалию) —
        // ловим уже появившийся актёр (BeginPlay): Host шлёт класс и место, Client свой такой сразу убирает и создаёт присланный.
        // ConductiveLink не трогаем — их порождают сами аномалии у каждого
        struct Rot
        {
            float Pitch, Yaw, Roll;
        };
        Hook::GlobalCallbackId g_begin_play{}, g_begin_play_pre{};
        bool g_spawning = false; // Client: идёт наш спавн присланной аномалии — её не убирать
        // Идёт LoadMap: BeginPlay актёров из сейва приходит раньше, чем on_map_loaded обновит g_loaded_ms, — по старой отметке
        // «карта давно загружена», и Client убирал аномалию из сейва Host (тест 2026-11-10 07:26:04, пузырь пиявки)
        bool g_loading = false;

        bool synced_hazard(const StringType& cls)
        {
            return cls.find(STR("_AnchorPull_")) != StringType::npos || cls.find(STR("_AnchorSpawn_")) != StringType::npos ||
                   cls == STR("BP_Hazard_RadiatedZone_SpawnEvent_C") ||
                   // «шагающая» цепочка: у каждого ПК шла своим путём (тест 2026-11-10 07:34: Host 8128,28781 — Client 9684,29435);
                   // вариант _AnchorSpawn_ того же класса так уже синхронизируется (тест 2026-11-09)
                   cls == STR("BP_Hazard_Movement_Prong_C") ||
                   // шторм и портал выхода (экран гоблера создаёт их только у выбравшего выход, тест 0.17.9 19:29)
                   cls == STR("BP_ExitPortalSpawn_C") || cls == STR("BP_ZoneStormRoyale_C") ||
                   // пузыри пиявок; сами пиявки не шлём — их порождает пузырь. Зону события директора BP_LocationalEvent_C не шлём:
                   // её создаёт само событие GEV, а событие Host-а Client повторяет целиком (WeatherSync, [Gev])
                   cls.find(STR("BP_Hazard_SpawnBubble_Leech_")) == 0;
        }

        Rot rotation(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorRotation"));
            auto* ret = fn ? fn.param<Rot>(STR("ReturnValue")) : nullptr;
            if (!ret) return {};
            fn.call();
            return *ret;
        }

        void on_begin_play(AActor* actor)
        {
            // загрузка карты: аномалии из сейва есть у обоих, их не трогаем
            if (!actor || g_spawning || g_loading || !Net::connected() || now_ms() - g_loaded_ms < SettleMs) return;
            auto cls = actor->GetClassPrivate()->GetName();
            if (!synced_hazard(cls)) return;
            Vec loc = location(actor);
            if (!g_is_host)
            {
                UFunctionCall destroy(actor, STR("K2_DestroyActor"));
                if (destroy) destroy.call();
                Output::send<LogLevel::Verbose>(STR("[Hazard] своя аномалия {} у ({:.0f}, {:.0f}, {:.0f}) убрана — ждём от Host\n"), cls, static_cast<double>(loc.X),
                                                static_cast<double>(loc.Y), static_cast<double>(loc.Z));
                return;
            }
            Rot rot = rotation(actor);
            char where[160];
            int n = std::snprintf(where, sizeof where, "%.0f %.0f %.0f %.1f %.1f %.1f", loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll);
            if (cls == STR("BP_ZoneStormRoyale_C"))
            {
                // параметры SpawnRoyaleStorm: игра пишет их в шторм до BeginPlay (разбор BP_StormManager, 2026-11-09)
                auto* timer = property<bool>(actor, STR("TimerSpawn"));
                auto** gate = property<UObject*>(actor, STR("SummonedExitGateway"));
                Vec g = gate && *gate ? location(*gate) : Vec{NAN, NAN, NAN};
                std::snprintf(where + n, sizeof where - n, " %d %.0f %.0f %.0f", timer && *timer ? 1 : 0, g.X, g.Y, g.Z);
            }
            bool sent = Net::send_part(where, Items::narrow(actor->GetClassPrivate()->GetPathName()), Coop::HazardSpawn);
            Output::send<LogLevel::Verbose>(STR("[Hazard] аномалия {} у ({:.0f}, {:.0f}, {:.0f}) → Client {}\n"), cls, static_cast<double>(loc.X), static_cast<double>(loc.Y),
                                            static_cast<double>(loc.Z), sent ? STR("отправлена") : STR("НЕ ВЛЕЗЛА"));
        }

        // FRotator → FQuat (как FRotator::Quaternion в UE4)
        void quat_of(const Rot& r, float q[4])
        {
            constexpr float H = 3.14159265f / 360.f;
            float sp = std::sin(r.Pitch * H), cp = std::cos(r.Pitch * H), sy = std::sin(r.Yaw * H), cy = std::cos(r.Yaw * H), sr = std::sin(r.Roll * H),
                  cr = std::cos(r.Roll * H);
            q[0] = cr * sp * sy - sr * cp * cy;
            q[1] = -cr * sp * cy - sr * cp * sy;
            q[2] = cr * cp * sy - sr * sp * cy;
            q[3] = cr * cp * cy + sr * sp * sy;
        }
    } // namespace

    void on_hazard(const std::string& where, const std::string& class_path)
    {
        Vec loc{};
        Rot rot{};
        int timer = 0;
        Vec gate{NAN, NAN, NAN};
        int got = std::sscanf(where.c_str(), "%f %f %f %f %f %f %d %f %f %f", &loc.X, &loc.Y, &loc.Z, &rot.Pitch, &rot.Yaw, &rot.Roll, &timer, &gate.X, &gate.Y, &gate.Z);
        if (got < 6) return;
        if (class_path.find("BP_ZoneStormRoyale_C") != std::string::npos)
        {
            // Шторм: зовём родной SpawnRoyaleStorm — он сам ставит TimerSpawn, портал-цель и флаг WorldState
            UObject* mgr = UObjectGlobals::FindFirstOf(STR("BP_StormManager_C"));
            UObject* gateway = nullptr;
            if (got >= 10 && !std::isnan(gate.X))
            {
                Coop::AnchorBody at{static_cast<int32_t>(gate.X), static_cast<int32_t>(gate.Y), static_cast<int32_t>(gate.Z)};
                std::vector<UObject*> cand = portals();
                UObjectGlobals::FindAllOf(STR("BP_ExitPortalSpawn_C"), cand);
                gateway = nearest(cand, at);
            }
            UFunctionCall fn(mgr, STR("SpawnRoyaleStorm"));
            auto* p_loc = fn ? fn.param<Vec>(STR("Location")) : nullptr;
            auto* p_timer = fn ? fn.param<bool>(STR("FromTimer")) : nullptr;
            auto* p_gate = fn ? fn.param<UObject*>(STR("SummonedGatewayExit")) : nullptr;
            if (p_loc && p_timer && p_gate)
            {
                *p_loc = loc;
                *p_timer = timer != 0;
                *p_gate = gateway;
                g_spawning = true;
                fn.call();
                g_spawning = false;
            }
            Output::send<LogLevel::Verbose>(STR("[Hazard] шторм Host у ({:.0f}, {:.0f}, {:.0f}), от таймера {}, портал {}: SpawnRoyaleStorm {}\n"), static_cast<double>(loc.X),
                                            static_cast<double>(loc.Y), static_cast<double>(loc.Z), timer, gateway ? STR("найден") : STR("нет"),
                                            p_loc && p_timer && p_gate ? STR("вызван") : STR("НЕ НАЙДЕН (менеджер/параметры)"));
            return;
        }
        // класс может быть ещё не загружен (тест: «класс НЕ ЗАГРУЖЕН» — аномалия пришла до загрузки зоны у Client) — грузим из реестра
        auto* cls = static_cast<UClass*>(Items::find_or_load(class_path));
        UObject* ctx = UObjectGlobals::FindFirstOf(STR("BP_HazardAnchorPull_C"));
        if (!ctx) ctx = UObjectGlobals::FindFirstOf(STR("BP_StormManager_C")); // шторм/портал: любой живой объект мира как контекст
        auto* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        UObject* actor = nullptr;
        if (cls && ctx && statics)
        {
            float tf[12] = {0, 0, 0, 1, loc.X, loc.Y, loc.Z, 0, 1, 1, 1, 0}; // FTransform: кватернион, позиция, масштаб
            quat_of(rot, tf);
            UFunctionCall begin(statics, STR("BeginDeferredActorSpawnFromClass"));
            auto* p_ctx = begin ? begin.param<UObject*>(STR("WorldContextObject")) : nullptr;
            auto* p_class = begin ? begin.param<UClass*>(STR("ActorClass")) : nullptr;
            auto* p_tf = begin ? begin.param<float[12]>(STR("SpawnTransform")) : nullptr;
            auto* p_coll = begin ? begin.param<uint8_t>(STR("CollisionHandlingOverride")) : nullptr;
            auto* p_ret = begin ? begin.param<UObject*>(STR("ReturnValue")) : nullptr;
            UFunctionCall finish(statics, STR("FinishSpawningActor"));
            auto* f_actor = finish ? finish.param<UObject*>(STR("Actor")) : nullptr;
            auto* f_tf = finish ? finish.param<float[12]>(STR("SpawnTransform")) : nullptr;
            if (p_ctx && p_class && p_tf && p_coll && p_ret && f_actor && f_tf)
            {
                *p_ctx = ctx;
                *p_class = cls;
                std::memcpy(*p_tf, tf, sizeof tf);
                *p_coll = 1; // AlwaysSpawn: место уже выбрал Host
                begin.call();
                if ((actor = *p_ret))
                {
                    *f_actor = actor;
                    std::memcpy(*f_tf, tf, sizeof tf);
                    g_spawning = true; // BeginPlay придёт внутри FinishSpawningActor
                    finish.call();
                    g_spawning = false;
                }
            }
        }
        auto slash = class_path.rfind('.');
        StringType name(class_path.begin() + (slash == std::string::npos ? 0 : slash + 1), class_path.end());
        Output::send<LogLevel::Verbose>(STR("[Hazard] аномалия Host {} у ({:.0f}, {:.0f}, {:.0f}): {}\n"), name, static_cast<double>(loc.X), static_cast<double>(loc.Y),
                                        static_cast<double>(loc.Z), actor ? STR("создана") : !cls ? STR("класс НЕ ЗАГРУЖЕН") : STR("НЕ СОЗДАНА"));
    }

    void install()
    {
        g_begin_play = Hook::RegisterBeginPlayPostCallback([](Hook::TCallbackIterationData<void>&, AActor* actor) { on_begin_play(actor); },
                                                           {false, false, STR("PacificDriveCoop"), STR("AnchorHazards")});
        Output::send<LogLevel::Verbose>(STR("[Hazard] перехват появления аномалий {}\n"), g_begin_play ? STR("установлен") : STR("НЕ УСТАНОВЛЕН"));
        // Client: генератор не ломается сам при появлении. Шанс < 0 — DeterminePlasmaBreakChance считает его по карте, 0 — пропускает,
        // а «случайное < 0» не бывает (байткод BeginPlay, 2026-11-17).
        // FTuningFloat (CXXHeaderDump/PenDriverUtils.hpp): 0x0 bool bOverride_TuningFloat, 0x4 FGameplayTag TuningTag, 0xC float TuningFloat
        g_begin_play_pre = Hook::RegisterBeginPlayPreCallback(
                [](Hook::TCallbackIterationData<void>&, AActor* actor) {
                    if (!actor || g_is_host || !Net::connected() || !is_generator(actor)) return;
                    // Заданный тег побеждает значение даже при bOverride_TuningFloat = true (мост 2026-11-17: тег леса → 0,75 при значении 0,25),
                    // поэтому тег — None (FName {0,0}), значение 0. Без тега Conv_TuningFloatToFloat отдаёт значение (на этом держится «шанс < 0» в игре)
                    if (auto* chance = property<uint8_t>(actor, STR("ChanceToBreakOnSpawn")))
                    {
                        std::memset(chance + 0x4, 0, 8);
                        *reinterpret_cast<float*>(chance + 0xC) = 0.0f;
                    }
                },
                {false, false, STR("PacificDriveCoop"), STR("GeneratorNoBreak")});
    }

    void on_map_loading() { g_loading = true; }

    void on_map_loaded()
    {
        g_loading = false;
        g_loaded_ms = now_ms();
        g_taken.clear();
        g_portal.clear();
        g_portal_echo.clear();
        g_gen_broken.clear();
        g_gen_wanted.clear();
    }

    void on_anchor(Coop::PacketId id, const Coop::AnchorBody& body)
    {
        if (id == Coop::PortalActive) return on_portal(body);
        if (id == Coop::GeneratorBroken)
        {
            if (UObject* g = nearest(generators(), body); g)
            {
                if (!generator_broken(g)) break_generator(g);
            }
            else g_gen_wanted.push_back(body); // ещё не появился — сломаем на опросе, когда появится
            return;
        }
        UObject* best = nearest(bases(), body);
        if (!best)
        {
            Output::send<LogLevel::Verbose>(STR("[Anchor] собеседник взял якорь у ({}, {}, {}), у меня там основания нет\n"), body.X, body.Y, body.Z);
            return;
        }
        if (taken(best))
        {
            // Оба взяли почти одновременно — у каждого свой якорь; Host не отбирает (shortcut: редкий случай, отбирать — если станет мешать)
            Output::send<LogLevel::Verbose>(STR("[Anchor] этот якорь я тоже уже взял — оставляю\n"));
            g_taken[best] = true;
            return;
        }
        take_here(best);
        Output::send<LogLevel::Verbose>(STR("[Anchor] {} взял якорь у ({}, {}, {}) — у меня снят\n"), g_is_host ? STR("пассажир") : STR("Host"), body.X, body.Y, body.Z);
    }

    void tick(bool is_host)
    {
        g_is_host = is_host;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs) return;
        g_last_poll_ms = now;
        bool settled = now - g_loaded_ms >= SettleMs;
        tick_portals(settled);
        tick_generators(settled, now);
        for (auto* base : bases())
        {
            bool now_taken = taken(base);
            auto it = g_taken.find(base);
            // впервые видим основание или карта ещё грузится — только запоминаем: взятое до нас взято в сейве
            if (it == g_taken.end() || !settled || !Net::connected())
            {
                g_taken[base] = now_taken;
                continue;
            }
            if (now_taken && !it->second)
            {
                Vec p = location(base);
                if (!std::isnan(p.X))
                {
                    Net::send_anchor({static_cast<int32_t>(std::lround(p.X)), static_cast<int32_t>(std::lround(p.Y)), static_cast<int32_t>(std::lround(p.Z))});
                    Output::send<LogLevel::Verbose>(STR("[Anchor] я взял якорь у ({:.0f}, {:.0f}, {:.0f}) → сообщаю собеседнику\n"), static_cast<double>(p.X),
                                                    static_cast<double>(p.Y), static_cast<double>(p.Z));
                }
            }
            it->second = now_taken;
        }
    }
} // namespace AnchorSync
