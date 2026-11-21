#include "Presence.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Cached.h"
#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <format>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Марионетка — SkeletalMeshActor с мешем нашего же персонажа (меш гарантированно загружен), без коллизии.
// Анимация — циклы стойка/ходьба/бег того же скелета (уже в памяти: ими пользуется наш персонаж), выбор по скорости.
// Не настоящий Character: GameMode/PlayerController одиночной игры второй pawn не ждут (docs/design/DECISIONS.md).
namespace Presence
{
    namespace
    {
        struct Vec
        {
            float X, Y, Z;
        };
        struct Rot
        {
            float Pitch, Yaw, Roll;
        };
        struct alignas(16) Transform // FTransform UE4: кватернион, позиция, масштаб — по 16 байт
        {
            float Rotation[4];
            float Translation[4];
            float Scale[4];
        };

        constexpr uint64_t StaleMs = 5000;    // нет поз дольше — марионетку убираем
        constexpr float SnapDistance = 1000;  // дальше — телепорт, а не плавное догоняние
        constexpr float SmoothRate = 12;      // 1/с: доля пути к цели за кадр = 1 - e^(-dt*rate)
        constexpr uint8_t AlwaysSpawn = 1;    // ESpawnActorCollisionHandlingMethod::AlwaysSpawn

        std::atomic<bool> g_map_loaded{false};
        bool g_have_target = false, g_spawn_error_logged = false;
        Coop::PoseBody g_target{};
        uint64_t g_target_ms = 0, g_last_send_ms = 0;
        UObject* g_marionette = nullptr;
        FWeakObjectPtr g_marionette_weak; // актёр могут уничтожить и без смены карты — см. Cached::drop_if_dead
        Vec g_pos{};
        float g_yaw = 0;
        float g_mesh_z = 0, g_mesh_yaw = 0; // смещение меша относительно капсулы (у персонажа меш стоит ниже центра и повёрнут)

        // Анимация по скорости. Опорные скорости — как у ALS (ходьба ~165, бег ~375 см/с): темп = скорость / опорная
        enum AnimState { Idle, Walk, Run, AnimStateCount };
        constexpr float ReferenceSpeed[AnimStateCount] = {0, 165, 375};
        constexpr float IdleToWalk = 40, WalkToIdle = 20, WalkToRun = 300, RunToWalk = 250; // гистерезис, см/с
        constexpr float SpeedSmoothRate = 5;
        UObject* g_component = nullptr;
        UObject* g_anims[AnimStateCount]{};
        int g_anim_state = -1;
        float g_speed = 0, g_play_rate = 1;
        Vec g_prev_pos{};
        bool g_seated = false; // наш персонаж посажен модом (игра об этом не знает: PercentInCar = 0)
        bool g_hidden = false; // водитель в машине — манекен прячем (Этап 3 посадит пассажира на сиденье)
        // Предмет в руке: свой путь меша шлём при изменении (g_hand_sent — что ушло, "\x01" — ещё не слали), чужой показываем у куклы
        std::string g_hand_sent = "\x01", g_hand_wanted;
        bool g_hand_dirty = false, g_hand_hidden = false, g_hand_attached = false;
        uint64_t g_hand_poll_ms = 0;
        UObject* g_hand = nullptr;
        FWeakObjectPtr g_hand_weak;
        UClass* g_hand_logged = nullptr; // для какого класса уже писали «без BaseMesh»
        // Части предмета (насадки, пламя, плазма — hand_fx): у себя — компоненты нашего предмета, у куклы — актёры на корпусе
        constexpr size_t MaxParts = 16;
        constexpr uint64_t StreamAfterUseMs = 1500; // после отпускания ещё шлём части: инструмент сбрасывает обороты ~1 с
        UObject* g_own_item = nullptr;
        FWeakObjectPtr g_own_item_weak;
        UObject* g_own_base = nullptr;
        std::vector<UObject*> g_own_parts;
        std::string g_parts_sent, g_parts_wanted;
        bool g_parts_dirty = false, g_hand_fx = false;
        uint64_t g_stream_until_ms = 0;
        // Части шлём ~24 раза в секунду (как кино): чаще глазу не нужно
        constexpr uint64_t PartsIntervalMs = 1000 / 24;
        uint64_t g_parts_poll_ms = 0;
        // Использование предмета: у куклы он поднимается по дуге. g_use_t — 0..1, приближается к цели долей остатка за шаг (геометрически)
        int g_use_sent = -1; // что ушло собеседнику (-1 — ещё не слали)
        bool g_use_wanted = false;
        float g_use_t = 0, g_hand_scale = 0.5f;
        UObject* g_hand_comp = nullptr; // StaticMeshComponent корпуса у куклы
        std::vector<UObject*> g_parts, g_part_comps;
        std::vector<FWeakObjectPtr> g_parts_weak;

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        UObject* own_character() { return Cached::character(); }

        void send_own_pose(UObject* character)
        {
            UFunctionCall get_loc(character, STR("K2_GetActorLocation"));
            UFunctionCall get_rot(character, STR("K2_GetActorRotation"));
            auto* loc = get_loc ? get_loc.param<Vec>(STR("ReturnValue")) : nullptr;
            auto* rot = get_rot ? get_rot.param<Rot>(STR("ReturnValue")) : nullptr;
            if (!loc || !rot) return;
            get_loc.call();
            get_rot.call();
            auto* in_car = property<float>(character, STR("PercentInCar")); // 1.0 = сидит в машине (RECON.md)
            Net::send_pose({loc->X, loc->Y, loc->Z, rot->Yaw, static_cast<uint8_t>(g_seated || (in_car && *in_car > 0.5f) ? Coop::PoseInCar : 0)});
        }

        std::wstring lower(std::wstring s)
        {
            for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
            return s;
        }

        // Циклы стойки/ходьбы/бега среди анимаций скелета меша. Имена ALS заранее не известны — подбор по подстрокам,
        // полный список пишется в лог, чтобы поправить шаблоны без отдельной разведки.
        void find_anims(UObject* mesh_asset)
        {
            auto** skeleton = property<UObject*>(mesh_asset, STR("Skeleton"));
            if (!skeleton || !*skeleton) return;
            std::vector<UObject*> all;
            UObjectGlobals::FindAllOf(STR("AnimSequence"), all);
            std::vector<std::pair<std::wstring, UObject*>> mine;
            std::wstring names;
            for (UObject* anim : all)
            {
                auto** s = property<UObject*>(anim, STR("Skeleton"));
                if (!s || *s != *skeleton) continue;
                mine.emplace_back(lower(anim->GetName()), anim);
                names += anim->GetName() + L" ";
            }
            Output::send<LogLevel::Verbose>(STR("[Presence] анимаций скелета {}: {} — {}\n"), (*skeleton)->GetName(), mine.size(), names);

            static const wchar_t* excluded[] = {L"turn", L"start", L"stop", L"pivot", L"crouch", L"add", L"lean", L"trans", L"jump",
                                                L"land", L"fall", L"rot", L"aim", L"mantle", L"roll"};
            // направление задают сами шаблоны (walk_f/run_f); из совпавших берётся самое короткое имя — базовое ALS_N_*
            static const std::vector<const wchar_t*> patterns[AnimStateCount] = {
                    {L"idle", L"pose_standing", L"stand"}, {L"walk_f", L"walk"}, {L"run_f", L"jog_f", L"run", L"jog"}};
            for (int state = 0; state < AnimStateCount; ++state)
            {
                for (const wchar_t* pattern : patterns[state])
                {
                    const std::pair<std::wstring, UObject*>* best = nullptr;
                    for (const auto& candidate : mine)
                    {
                        if (candidate.first.find(pattern) == std::wstring::npos) continue;
                        bool skip = false;
                        for (const wchar_t* bad : excluded)
                            skip = skip || (candidate.first.find(bad) != std::wstring::npos && std::wstring(pattern).find(bad) == std::wstring::npos);
                        if (!skip && (!best || candidate.first.size() < best->first.size())) best = &candidate;
                    }
                    if (best)
                    {
                        g_anims[state] = best->second;
                        break;
                    }
                }
            }
            Output::send<LogLevel::Verbose>(STR("[Presence] выбрано: стоит={}, идёт={}, бежит={}\n"), g_anims[Idle] ? g_anims[Idle]->GetName() : STR("<нет>"),
                                            g_anims[Walk] ? g_anims[Walk]->GetName() : STR("<нет>"), g_anims[Run] ? g_anims[Run]->GetName() : STR("<нет>"));
        }

        void play(int state)
        {
            UFunctionCall fn(g_component, STR("PlayAnimation"));
            auto* p_anim = fn ? fn.param<UObject*>(STR("NewAnimToPlay")) : nullptr;
            auto* p_loop = fn ? fn.param<bool>(STR("bLooping")) : nullptr;
            if (!p_anim || !p_loop || !g_anims[state]) return;
            *p_anim = g_anims[state];
            *p_loop = true;
            fn.call();
            g_play_rate = 1;
        }

        void set_play_rate(float rate)
        {
            if (std::fabs(rate - g_play_rate) < 0.05f) return;
            UFunctionCall fn(g_component, STR("SetPlayRate"));
            if (auto* p = fn ? fn.param<float>(STR("Rate")) : nullptr)
            {
                *p = rate;
                fn.call();
                g_play_rate = rate;
            }
        }

        void update_animation(float delta_seconds)
        {
            if (!g_component || delta_seconds <= 0) return;
            float dx = g_pos.X - g_prev_pos.X, dy = g_pos.Y - g_prev_pos.Y;
            g_prev_pos = g_pos;
            float instant = std::sqrt(dx * dx + dy * dy) / delta_seconds; // только по горизонтали
            g_speed += (instant - g_speed) * (1 - std::exp(-delta_seconds * SpeedSmoothRate));

            int state = g_anim_state < 0 ? Idle : g_anim_state;
            if (state == Idle && g_speed > IdleToWalk) state = Walk;
            else if (state == Walk && g_speed < WalkToIdle) state = Idle;
            else if (state == Walk && g_speed > WalkToRun) state = Run;
            else if (state == Run && g_speed < RunToWalk) state = Walk;
            if (state == Run && !g_anims[Run]) state = Walk; // нет бега — ходьба быстрее
            if (state != g_anim_state)
            {
                g_anim_state = state;
                play(state);
            }
            if (state != Idle) set_play_rate(std::clamp(g_speed / ReferenceSpeed[state], 0.5f, 1.8f));
        }

        Transform transform_at(const Vec& p)
        {
            return {{0, 0, 0, 1}, {p.X, p.Y, p.Z, 0}, {1, 1, 1, 0}};
        }

        // Актёр класса движка без коллизии (марионетка и предмет в её руке не должны толкать машину и игрока — физику ведёт Host)
        UObject* spawn_actor(const TCHAR* class_path, UObject* character, const Vec& at)
        {
            auto* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            auto* actor_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, class_path);
            UFunctionCall begin(statics, STR("BeginDeferredActorSpawnFromClass"));
            auto* p_ctx = begin ? begin.param<UObject*>(STR("WorldContextObject")) : nullptr;
            auto* p_class = begin ? begin.param<UClass*>(STR("ActorClass")) : nullptr;
            auto* p_tf = begin ? begin.param<Transform>(STR("SpawnTransform")) : nullptr;
            auto* p_coll = begin ? begin.param<uint8_t>(STR("CollisionHandlingOverride")) : nullptr;
            auto* p_ret = begin ? begin.param<UObject*>(STR("ReturnValue")) : nullptr;
            UFunctionCall finish(statics, STR("FinishSpawningActor"));
            auto* f_actor = finish ? finish.param<UObject*>(STR("Actor")) : nullptr;
            auto* f_tf = finish ? finish.param<Transform>(STR("SpawnTransform")) : nullptr;
            if (!actor_class || !p_ctx || !p_class || !p_tf || !p_coll || !p_ret || !f_actor || !f_tf) return nullptr;

            *p_ctx = character;
            *p_class = actor_class;
            *p_tf = transform_at(at);
            *p_coll = AlwaysSpawn;
            begin.call();
            UObject* actor = *p_ret;
            if (!actor) return nullptr;
            *f_actor = actor;
            *f_tf = transform_at(at);
            finish.call();

            UFunctionCall no_collision(actor, STR("SetActorEnableCollision"));
            if (auto* p = no_collision ? no_collision.param<bool>(STR("bNewActorEnableCollision")) : nullptr)
            {
                *p = false;
                no_collision.call();
            }
            return actor;
        }

        UObject* spawn_marionette(UObject* character, const Vec& at)
        {
            UObject* actor = spawn_actor(STR("/Script/Engine.SkeletalMeshActor"), character, at);
            if (!actor) return nullptr;
            auto** our_mesh = property<UObject*>(character, STR("Mesh"));
            auto** mesh_asset = our_mesh ? property<UObject*>(*our_mesh, STR("SkeletalMesh")) : nullptr;
            auto** component = property<UObject*>(actor, STR("SkeletalMeshComponent"));
            UFunctionCall set_mesh(component ? *component : nullptr, STR("SetSkeletalMesh"));
            auto* p_mesh = set_mesh ? set_mesh.param<UObject*>(STR("NewMesh")) : nullptr;
            auto* p_reinit = set_mesh ? set_mesh.param<bool>(STR("bReinitPose")) : nullptr;
            if (mesh_asset && *mesh_asset && p_mesh && p_reinit)
            {
                *p_mesh = *mesh_asset;
                *p_reinit = true;
                set_mesh.call();
            }
            g_component = component ? *component : nullptr;
            g_anim_state = -1;
            if (mesh_asset && *mesh_asset) find_anims(*mesh_asset);
            if (auto* rel = our_mesh ? property<Vec>(*our_mesh, STR("RelativeLocation")) : nullptr) g_mesh_z = rel->Z;
            if (auto* rel = our_mesh ? property<Rot>(*our_mesh, STR("RelativeRotation")) : nullptr) g_mesh_yaw = rel->Yaw;
            Output::send<LogLevel::Verbose>(STR("[Presence] марионетка создана: {} (меш {}, смещение z={}, yaw={})\n"), actor->GetFullName(),
                                            mesh_asset && *mesh_asset ? (*mesh_asset)->GetName() : STR("<нет>"), g_mesh_z, g_mesh_yaw);
            return actor;
        }

        void move_marionette(const Vec& p, float yaw)
        {
            UFunctionCall fn(g_marionette, STR("K2_SetActorLocationAndRotation"));
            auto* p_loc = fn ? fn.param<Vec>(STR("NewLocation")) : nullptr;
            auto* p_rot = fn ? fn.param<Rot>(STR("NewRotation")) : nullptr;
            auto* p_teleport = fn ? fn.param<bool>(STR("bTeleport")) : nullptr;
            if (!p_loc || !p_rot || !p_teleport) return;
            *p_loc = {p.X, p.Y, p.Z + g_mesh_z};
            *p_rot = {0, yaw + g_mesh_yaw, 0};
            *p_teleport = true;
            fn.call();
        }

        void set_hidden(bool hidden)
        {
            if (hidden == g_hidden) return;
            UFunctionCall fn(g_marionette, STR("SetActorHiddenInGame"));
            if (auto* p = fn ? fn.param<bool>(STR("bNewHidden")) : nullptr)
            {
                *p = hidden;
                fn.call();
                g_hidden = hidden;
            }
        }

        void hide_actor(UObject* actor, bool hidden)
        {
            UFunctionCall fn(actor, STR("SetActorHiddenInGame"));
            if (auto* p = fn ? fn.param<bool>(STR("bNewHidden")) : nullptr)
            {
                *p = hidden;
                fn.call();
            }
        }

        void destroy_actor(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_DestroyActor"));
            if (fn) fn.call();
        }

        void destroy_hand()
        {
            for (size_t i = 0; i < g_parts.size(); ++i)
                if (g_parts_weak[i].Get()) destroy_actor(g_parts[i]);
            g_parts.clear();
            g_parts_weak.clear();
            g_part_comps.clear();
            if (g_hand) destroy_actor(g_hand);
            g_hand = nullptr;
            g_hand_comp = nullptr;
        }

        std::vector<std::string> split(const std::string& s, char sep)
        {
            std::vector<std::string> out;
            size_t pos = 0;
            for (size_t end; (end = s.find(sep, pos)) != std::string::npos; pos = end + 1) out.push_back(s.substr(pos, end - pos));
            out.push_back(s.substr(pos));
            return out;
        }

        // Предмет в нашей руке (PDP_MainCharacter.GetActiveHandItem); nullptr — рука пуста
        UObject* own_hand_item(UObject* character)
        {
            UFunctionCall fn(character, STR("GetActiveHandItem"));
            auto* p_ignore = fn ? fn.param<bool>(STR("Ignore Lifespan")) : nullptr; // у параметров BP-функции пробелы в именах
            auto* p_item = fn ? fn.param<UObject*>(STR("Hand Item")) : nullptr;
            if (!p_ignore || !p_item) return nullptr;
            *p_ignore = true;
            fn.call();
            return *p_item;
        }

        // Запомнить корпус и части нашего предмета; «корпус;часть1;часть2…» — пути мешей, пусто — рука пуста или нет BaseMesh.
        // Части — остальные StaticMeshComponent блюпринта (насадки, пламя, плазма): их положение и видимость шлём, пока предмет работает
        std::string describe_own_item(UObject* item)
        {
            g_own_item = item;
            g_own_item_weak = item ? FWeakObjectPtr(item) : FWeakObjectPtr();
            g_own_base = nullptr;
            g_own_parts.clear();
            if (!item) return {};
            auto** base = property<UObject*>(item, STR("BaseMesh"));
            auto** mesh = base && *base ? property<UObject*>(*base, STR("StaticMesh")) : nullptr;
            if (!mesh || !*mesh)
            {
                if (g_hand_logged != item->GetClassPrivate())
                {
                    g_hand_logged = item->GetClassPrivate();
                    Output::send<LogLevel::Verbose>(STR("[Presence] предмет в руке {} без BaseMesh — собеседник его не увидит\n"), item->GetClassPrivate()->GetName());
                }
                return {};
            }
            g_own_base = *base;
            std::string out = Items::narrow((*mesh)->GetPathName());
            auto* comps = property<Items::RawArray>(item, STR("BlueprintCreatedComponents"));
            for (int32_t i = 0; comps && comps->Data && i < comps->Num && g_own_parts.size() < MaxParts; ++i)
            {
                UObject* c = comps->Data[i];
                if (!c || c == g_own_base || c->GetClassPrivate()->GetName() != STR("StaticMeshComponent")) continue;
                auto** part_mesh = property<UObject*>(c, STR("StaticMesh"));
                if (!part_mesh || !*part_mesh) continue;
                g_own_parts.push_back(c);
                out += ";" + Items::narrow((*part_mesh)->GetPathName());
            }
            return out;
        }

        bool world_of(UObject* component, Transform& out)
        {
            UFunctionCall fn(component, STR("K2_GetComponentToWorld"));
            auto* r = fn ? fn.param<Transform>(STR("ReturnValue")) : nullptr;
            if (!r) return false;
            fn.call();
            out = *r;
            return true;
        }

        // Видимость и положение частей относительно корпуса: «v x y z qx qy qz qw sx sy sz;…»; пусто — не прочиталось
        std::string own_parts_text()
        {
            static auto* math = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetMathLibrary"));
            Transform base{};
            if (!math || !g_own_base || !world_of(g_own_base, base)) return {};
            std::string out;
            for (auto* part : g_own_parts)
            {
                UFunctionCall rel(math, STR("MakeRelativeTransform"));
                auto* p_a = rel ? rel.param<Transform>(STR("A")) : nullptr;
                auto* p_to = rel ? rel.param<Transform>(STR("RelativeTo")) : nullptr;
                auto* p_ret = rel ? rel.param<Transform>(STR("ReturnValue")) : nullptr;
                UFunctionCall vis(part, STR("IsVisible"));
                auto* p_vis = vis ? vis.param<bool>(STR("ReturnValue")) : nullptr;
                if (!p_a || !p_to || !p_ret || !p_vis || !world_of(part, *p_a)) return {};
                *p_to = base;
                rel.call();
                vis.call();
                const Transform& t = *p_ret;
                out += std::format("{}{} {:.1f} {:.1f} {:.1f} {:.3f} {:.3f} {:.3f} {:.3f} {:.2f} {:.2f} {:.2f}", out.empty() ? "" : ";", *p_vis ? 1 : 0,
                                   t.Translation[0], t.Translation[1], t.Translation[2], t.Rotation[0], t.Rotation[1], t.Rotation[2], t.Rotation[3],
                                   t.Scale[0], t.Scale[1], t.Scale[2]);
            }
            return out;
        }

        // StaticMeshActor без коллизии с подвижным мешем; component — его StaticMeshComponent
        UObject* spawn_mesh_actor(UObject* character, const Vec& at, UObject* mesh, UObject*& component)
        {
            UObject* actor = spawn_actor(STR("/Script/Engine.StaticMeshActor"), character, at);
            auto** comp = actor ? property<UObject*>(actor, STR("StaticMeshComponent")) : nullptr;
            if (!comp || !*comp)
            {
                if (actor) destroy_actor(actor);
                return nullptr;
            }
            UFunctionCall mobility(*comp, STR("SetMobility")); // по умолчанию Static: перемещаться не будет
            if (auto* p = mobility ? mobility.param<uint8_t>(STR("NewMobility")) : nullptr)
            {
                *p = 2; // EComponentMobility::Movable
                mobility.call();
            }
            UFunctionCall set_mesh(*comp, STR("SetStaticMesh"));
            if (auto* p = set_mesh ? set_mesh.param<UObject*>(STR("NewMesh")) : nullptr)
            {
                *p = mesh;
                set_mesh.call();
            }
            component = *comp;
            return actor;
        }

        // Части у куклы: положение и видимость из последнего «parts» собеседника; прячем все, пока он в машине
        void apply_parts()
        {
            auto entries = split(g_parts_wanted, ';');
            for (size_t i = 0; i < g_parts.size(); ++i)
            {
                if (!g_parts_weak[i].Get()) continue;
                int visible = 0;
                Transform t{{0, 0, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 0}};
                if (i < entries.size() && !g_parts_wanted.empty())
                    std::sscanf(entries[i].c_str(), "%d %f %f %f %f %f %f %f %f %f %f", &visible, &t.Translation[0], &t.Translation[1], &t.Translation[2],
                                &t.Rotation[0], &t.Rotation[1], &t.Rotation[2], &t.Rotation[3], &t.Scale[0], &t.Scale[1], &t.Scale[2]);
                UFunctionCall set(g_part_comps[i], STR("K2_SetRelativeTransform"));
                auto* p_tf = set ? set.param<Transform>(STR("NewTransform")) : nullptr;
                auto* p_tp = set ? set.param<bool>(STR("bTeleport")) : nullptr;
                if (p_tf && p_tp)
                {
                    *p_tf = t;
                    *p_tp = true;
                    set.call();
                }
                hide_actor(g_parts[i], g_hand_hidden || !visible);
            }
        }

        // Положение предмета в кисти куклы: покой (подобрано на глаз через мост на Scrapper, 2026-11-18) + дуга подъёма t∈0..1 и масштаб.
        // shortcut: одно положение на все предметы; если другие инструменты встанут криво — смещение по классу предмета
        void pose_hand(float t)
        {
            if (!g_hand_comp || !g_component) return;
            Vec offset{0, 0, 10};
            if (t > 0.001f)
            {
                // Дуга в мировых осях куклы (вверх 40 см, вперёд 20 см — четверть эллипса), в кость переводим обратным поворотом: осей кисти мы не знаем
                static auto* math = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetMathLibrary"));
                float a = t * 1.5707963f, yaw = (g_yaw + g_mesh_yaw) * 3.14159265f / 180.f;
                Vec world{std::cos(yaw) * 20 * (1 - std::cos(a)), std::sin(yaw) * 20 * (1 - std::cos(a)), 40 * std::sin(a)};
                UFunctionCall sock(g_component, STR("GetSocketTransform"));
                auto* p_sock = sock ? sock.param<FName>(STR("InSocketName")) : nullptr;
                auto* p_space = sock ? sock.param<uint8_t>(STR("TransformSpace")) : nullptr;
                auto* p_tf = sock ? sock.param<Transform>(STR("ReturnValue")) : nullptr;
                UFunctionCall inv(math, STR("InverseTransformDirection"));
                auto* p_t = inv ? inv.param<Transform>(STR("T")) : nullptr;
                auto* p_dir = inv ? inv.param<Vec>(STR("Direction")) : nullptr;
                auto* p_out = inv ? inv.param<Vec>(STR("ReturnValue")) : nullptr;
                if (p_sock && p_space && p_tf && p_t && p_dir && p_out)
                {
                    *p_sock = FName(STR("hand_r"), FNAME_Add);
                    *p_space = 0; // RTS_World
                    sock.call();
                    *p_t = *p_tf;
                    *p_dir = world;
                    inv.call();
                    offset = {offset.X + p_out->X, offset.Y + p_out->Y, offset.Z + p_out->Z};
                }
            }
            UFunctionCall rel(g_hand_comp, STR("K2_SetRelativeLocationAndRotation"));
            auto* p_rloc = rel ? rel.param<Vec>(STR("NewLocation")) : nullptr;
            auto* p_rrot = rel ? rel.param<Rot>(STR("NewRotation")) : nullptr;
            auto* p_tp = rel ? rel.param<bool>(STR("bTeleport")) : nullptr;
            if (p_rloc && p_rrot && p_tp)
            {
                *p_rloc = offset;
                *p_rrot = {0, -175, 0};
                *p_tp = true;
                rel.call();
            }
            // Кончик Scrapper — ось +X меша (замер через мост 2026-11-19): ставим мировой поворот так, чтобы он смотрел вперёд по взгляду куклы,
            // не зная осей кости. shortcut: ось +X подходит инструментам Scrapper; у других предметов смотреть, куда встанет
            UFunctionCall world_rot(g_hand_comp, STR("K2_SetWorldRotation"));
            auto* p_wrot = world_rot ? world_rot.param<Rot>(STR("NewRotation")) : nullptr;
            auto* p_wtp = world_rot ? world_rot.param<bool>(STR("bTeleport")) : nullptr;
            if (p_wrot && p_wtp)
            {
                *p_wrot = {0, g_yaw + g_mesh_yaw, 0}; // куда смотрит меш куклы, а не её актёр
                *p_wtp = true;
                world_rot.call();
            }
            UFunctionCall scale(g_hand_comp, STR("SetRelativeScale3D"));
            if (auto* p = scale ? scale.param<Vec>(STR("NewScale3D")) : nullptr)
            {
                *p = {g_hand_scale, g_hand_scale, g_hand_scale};
                scale.call();
            }
        }

        // Кукла — манекен ALS (скелет UE4: правая кисть — кость hand_r). Прикрепляем меш предмета к ней: он ходит вместе с рукой.
        // Кости нет (другой меш) — false, предмет ставим смещением (place_hand)
        bool attach_to_hand(UObject* item_component)
        {
            if (!g_component) return false;
            FName bone(STR("hand_r"), FNAME_Add);
            UFunctionCall exists(g_component, STR("DoesSocketExist"));
            auto* p_name = exists ? exists.param<FName>(STR("InSocketName")) : nullptr;
            auto* p_exists = exists ? exists.param<bool>(STR("ReturnValue")) : nullptr;
            if (!p_name || !p_exists) return false;
            *p_name = bone;
            exists.call();
            if (!*p_exists) return false;
            UFunctionCall attach(item_component, STR("K2_AttachToComponent"));
            auto* p_parent = attach ? attach.param<UObject*>(STR("Parent")) : nullptr;
            auto* p_socket = attach ? attach.param<FName>(STR("SocketName")) : nullptr;
            auto* p_loc = attach ? attach.param<uint8_t>(STR("LocationRule")) : nullptr;
            auto* p_rot = attach ? attach.param<uint8_t>(STR("RotationRule")) : nullptr;
            auto* p_scale = attach ? attach.param<uint8_t>(STR("ScaleRule")) : nullptr;
            auto* p_ret = attach ? attach.param<bool>(STR("ReturnValue")) : nullptr;
            if (!p_parent || !p_socket || !p_loc || !p_rot || !p_scale || !p_ret) return false;
            *p_parent = g_component;
            *p_socket = bone;
            *p_loc = *p_rot = 2; // EAttachmentRule::SnapToTarget
            *p_scale = 1;        // KeepWorld: масштаб предмета 1, не масштаб кости
            attach.call();
            if (!*p_ret) return false;
            g_hand_comp = item_component;
            pose_hand(0);
            return true;
        }

        // Предмет в руке куклы: StaticMeshActor с мешем предмета собеседника, у кисти куклы (или у груди)
        void rebuild_hand(UObject* character, const Vec& at)
        {
            destroy_hand();
            if (g_hand_wanted.empty()) return;
            auto paths = split(g_hand_wanted, ';');
            UObject* mesh = Items::find_or_load(paths[0]);
            if (!mesh)
            {
                Output::send<LogLevel::Warning>(STR("[Presence] меш предмета собеседника не найден: {}\n"), Items::wide(paths[0]));
                return;
            }
            UObject* component = nullptr;
            UObject* actor = spawn_mesh_actor(character, at, mesh, component);
            if (!actor) return;
            g_hand = actor;
            Cached::track(g_hand, g_hand_weak);
            g_hand_hidden = false;
            g_hand_attached = attach_to_hand(component);

            // Части (hand_fx): прикреплены к корпусу, положение задаёт apply_parts
            for (size_t i = 1; g_hand_fx && i < paths.size() && g_parts.size() < MaxParts; ++i)
            {
                UObject* part_mesh = Items::find_or_load(paths[i]);
                UObject* part_comp = nullptr;
                UObject* part = part_mesh ? spawn_mesh_actor(character, at, part_mesh, part_comp) : nullptr;
                if (!part) continue;
                UFunctionCall attach(part_comp, STR("K2_AttachToComponent"));
                auto* p_parent = attach ? attach.param<UObject*>(STR("Parent")) : nullptr;
                auto* p_loc = attach ? attach.param<uint8_t>(STR("LocationRule")) : nullptr;
                auto* p_rot = attach ? attach.param<uint8_t>(STR("RotationRule")) : nullptr;
                auto* p_scale = attach ? attach.param<uint8_t>(STR("ScaleRule")) : nullptr;
                if (!p_parent || !p_loc || !p_rot || !p_scale)
                {
                    destroy_actor(part);
                    continue;
                }
                *p_parent = component;
                *p_loc = *p_rot = *p_scale = 0; // KeepRelative: положение относительно корпуса придёт в «parts»
                attach.call();
                g_parts.push_back(part);
                g_parts_weak.emplace_back(part);
                g_part_comps.push_back(part_comp);
            }
            apply_parts();
            Output::send<LogLevel::Verbose>(STR("[Presence] предмет в руке куклы: {} ({}), частей {}\n"), mesh->GetName(),
                                            g_hand_attached ? STR("на кости hand_r") : STR("кости hand_r нет — у груди"), g_parts.size());
        }

        // Запасной путь, если у меша куклы нет кости hand_r. shortcut: одно смещение на все предметы (спереди-справа у груди)
        void place_hand(const Vec& p, float yaw)
        {
            constexpr float Forward = 45, Side = 18, Up = 15, ToRad = 3.14159265f / 180.f;
            float c = std::cos(yaw * ToRad), s = std::sin(yaw * ToRad);
            UFunctionCall fn(g_hand, STR("K2_SetActorLocationAndRotation"));
            auto* p_loc = fn ? fn.param<Vec>(STR("NewLocation")) : nullptr;
            auto* p_rot = fn ? fn.param<Rot>(STR("NewRotation")) : nullptr;
            auto* p_teleport = fn ? fn.param<bool>(STR("bTeleport")) : nullptr;
            if (!p_loc || !p_rot || !p_teleport) return;
            *p_loc = {p.X + c * Forward - s * Side, p.Y + s * Forward + c * Side, p.Z + Up};
            *p_rot = {0, yaw, 0};
            *p_teleport = true;
            fn.call();
        }

        void destroy_marionette()
        {
            UFunctionCall fn(g_marionette, STR("K2_DestroyActor"));
            if (fn) fn.call();
            g_marionette = g_component = nullptr;
            destroy_hand();
            g_hand_dirty = true; // новая кукла — предмет создадим заново
        }
    } // namespace

    void on_pose(const Coop::PoseBody& pose)
    {
        if (!g_have_target) Output::send<LogLevel::Verbose>(STR("[Presence] первая поза собеседника: ({:.0f}, {:.0f}, {:.0f})\n"), pose.X, pose.Y, pose.Z);
        g_target = pose;
        g_target_ms = now_ms();
        g_have_target = true;
    }

    void on_hand(const std::string& tag, const std::string& mesh_path)
    {
        if (tag == "parts")
        {
            g_parts_wanted = mesh_path;
            g_parts_dirty = true;
            return;
        }
        if (tag == "use")
        {
            g_use_wanted = mesh_path == "1";
            return;
        }
        if (tag != "hand" || mesh_path == g_hand_wanted) return;
        g_hand_wanted = mesh_path;
        g_parts_wanted.clear(); // части нового предмета придут следом
        g_hand_dirty = true;
    }

    void on_map_loaded() { g_map_loaded = true; }

    void set_seated(bool seated) { g_seated = seated; }
    bool seated() { return g_seated; }
    const Coop::PoseBody* mate() { return g_have_target && Net::connected() && now_ms() - g_target_ms <= StaleMs ? &g_target : nullptr; }

    void tick(float delta_seconds, bool hand_fx, float hand_scale)
    {
        if (g_map_loaded.exchange(false))
        {
            g_marionette = g_component = g_hand = g_own_item = g_own_base = nullptr; // акторы уничтожены вместе со старым миром
            g_parts.clear();
            g_parts_weak.clear();
            g_part_comps.clear();
            g_own_parts.clear();
            g_hand_dirty = true;
            g_hand_sent = "\x01"; // собеседник мог потерять нашу руку вместе со своим миром
            g_use_sent = -1;
            g_hand_comp = nullptr;
        }
        if (hand_scale != g_hand_scale)
        {
            g_hand_scale = hand_scale;
            pose_hand(g_use_t);
        }
        if (hand_fx != g_hand_fx)
        {
            g_hand_fx = hand_fx; // coop.ini поменяли на ходу — пересобрать предмет у куклы с частями или без
            g_hand_dirty = true;
        }
        if (Cached::drop_if_dead(g_marionette, g_marionette_weak)) g_component = nullptr; // компонент принадлежит актёру
        if (Cached::drop_if_dead(g_hand, g_hand_weak)) g_hand_dirty = true;
        if (!Net::connected())
        {
            g_hand_sent = "\x01"; // после переподключения шлём руку заново
            g_use_sent = -1;
            return;
        }
        UObject* character = own_character();
        if (!character) return; // меню, загрузка

        uint64_t now = now_ms();
        if (now - g_last_send_ms >= Net::adaptive_interval_ms(Coop::PoseIntervalMs))
        {
            g_last_send_ms = now;
            send_own_pose(character);
        }
        if (now - g_hand_poll_ms >= 100)
        {
            g_hand_poll_ms = now;
            UObject* item = own_hand_item(character);
            if (item != g_own_item || (item && !g_own_item_weak.Get()) || g_hand_sent == "\x01")
            {
                std::string paths = describe_own_item(item);
                if (paths != g_hand_sent)
                {
                    if (Net::send_part("hand", paths, Coop::HandItem))
                    {
                        g_hand_sent = paths;
                        g_parts_sent.clear(); // положение частей нового предмета — следом
                        Output::send<LogLevel::Verbose>(STR("[Presence] в руке: {} (частей {})\n"),
                                                        paths.empty() ? std::wstring(STR("пусто")) : Items::wide(paths.substr(0, paths.find(';'))), g_own_parts.size());
                    }
                    else g_own_item = nullptr; // не ушло — повторим на следующем опросе
                }
            }
        }
        // Части и флаг «использует» — 24 Гц, пока предмет работает (и чуть после), плюс один снимок на новый предмет; в покое — тишина
        if (now - g_parts_poll_ms >= PartsIntervalMs)
        {
            g_parts_poll_ms = now;
            auto* desire = g_own_item && g_own_item_weak.Get() ? property<bool>(g_own_item, STR("Desire Tool Primary Use")) : nullptr;
            bool use = desire && *desire;
            if (use) g_stream_until_ms = now + StreamAfterUseMs;
            if (int(use) != g_use_sent && Net::send_part("use", use ? "1" : "0", Coop::HandItem)) g_use_sent = use;
            if (!g_own_parts.empty() && g_own_item_weak.Get() && (now < g_stream_until_ms || g_parts_sent.empty()))
            {
                std::string text = own_parts_text();
                if (!text.empty() && text != g_parts_sent && Net::send_part("parts", text, Coop::HandItem)) g_parts_sent = text;
            }
        }

        if (!g_have_target) return;
        if (now - g_target_ms > StaleMs)
        {
            if (g_marionette) destroy_marionette();
            g_have_target = false;
            Output::send<LogLevel::Verbose>(STR("[Presence] поз нет {} с — марионетка убрана\n"), StaleMs / 1000);
            return;
        }

        Vec target{g_target.X, g_target.Y, g_target.Z};
        if (!g_marionette)
        {
            g_marionette = spawn_marionette(character, target);
            Cached::track(g_marionette, g_marionette_weak);
            if (!g_marionette)
            {
                if (!g_spawn_error_logged) Output::send<LogLevel::Error>(STR("[Presence] марионетку создать не удалось\n"));
                g_spawn_error_logged = true;
                return;
            }
            g_pos = g_prev_pos = target;
            g_yaw = g_target.Yaw;
            g_hidden = false;
        }
        set_hidden((g_target.Flags & Coop::PoseInCar) != 0);

        // Интерполяция к последней позе (спека 3.2.1: клиент сглаживает, Host прав)
        float dx = target.X - g_pos.X, dy = target.Y - g_pos.Y, dz = target.Z - g_pos.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > SnapDistance)
        {
            g_pos = g_prev_pos = target;
            g_yaw = g_target.Yaw;
        }
        else
        {
            float a = 1 - std::exp(-delta_seconds * SmoothRate);
            g_pos = {g_pos.X + dx * a, g_pos.Y + dy * a, g_pos.Z + dz * a};
            float dyaw = std::fmod(g_target.Yaw - g_yaw + 540.0f, 360.0f) - 180.0f; // кратчайший поворот
            g_yaw += dyaw * a;
        }
        move_marionette(g_pos, g_yaw);
        update_animation(delta_seconds);

        if (g_hand_dirty)
        {
            g_hand_dirty = false;
            rebuild_hand(character, g_pos);
        }
        if (g_hand)
        {
            bool hidden = (g_target.Flags & Coop::PoseInCar) != 0;
            if (hidden != g_hand_hidden)
            {
                g_hand_hidden = hidden;
                hide_actor(g_hand, hidden);
                g_parts_dirty = true; // части прячем и показываем вместе с корпусом
            }
            if (g_parts_dirty)
            {
                g_parts_dirty = false;
                apply_parts();
            }
            if (!g_hand_attached) place_hand(g_pos, g_yaw);
            else
            {
                // Использование: предмет идёт вверх по дуге, доля остатка за шаг постоянна (геометрический ряд); в машине — без подъёма
                float goal = g_use_wanted && !g_hand_hidden ? 1.f : 0.f;
                g_use_t += (goal - g_use_t) * (1 - std::exp(-delta_seconds * 9));
                if (g_use_t < 0.002f) g_use_t = 0;
                pose_hand(g_use_t); // каждый кадр: кость идёт за анимацией, поворот держим по взгляду куклы
            }
        }
    }
} // namespace Presence
