#include "Presence.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Марионетка — SkeletalMeshActor с мешем нашего же персонажа (меш гарантированно загружен), без коллизии.
// Анимация — циклы стойка/ходьба/бег того же скелета (уже в памяти: ими пользуется наш персонаж), выбор по скорости.
// Не настоящий Character: GameMode/PlayerController одиночной игры второй pawn не ждут (docs/DECISIONS.md).
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

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        UObject* own_character() { return UObjectGlobals::FindFirstOf(STR("BaseCharacter")); }

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

        UObject* spawn_marionette(UObject* character, const Vec& at)
        {
            auto* statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
            auto* actor_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, STR("/Script/Engine.SkeletalMeshActor"));
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

            // Без коллизии: марионетка не должна толкать машину и игрока (физику ведёт Host)
            UFunctionCall no_collision(actor, STR("SetActorEnableCollision"));
            if (auto* p = no_collision ? no_collision.param<bool>(STR("bNewActorEnableCollision")) : nullptr)
            {
                *p = false;
                no_collision.call();
            }

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

        void destroy_marionette()
        {
            UFunctionCall fn(g_marionette, STR("K2_DestroyActor"));
            if (fn) fn.call();
            g_marionette = g_component = nullptr;
        }
    } // namespace

    void on_pose(const Coop::PoseBody& pose)
    {
        if (!g_have_target) Output::send<LogLevel::Verbose>(STR("[Presence] первая поза собеседника: ({:.0f}, {:.0f}, {:.0f})\n"), pose.X, pose.Y, pose.Z);
        g_target = pose;
        g_target_ms = now_ms();
        g_have_target = true;
    }

    void on_map_loaded() { g_map_loaded = true; }

    void set_seated(bool seated) { g_seated = seated; }

    void tick(float delta_seconds)
    {
        if (g_map_loaded.exchange(false)) g_marionette = g_component = nullptr; // актор уничтожен вместе со старым миром
        if (!Net::connected()) return;
        UObject* character = own_character();
        if (!character) return; // меню, загрузка

        uint64_t now = now_ms();
        if (now - g_last_send_ms >= Coop::PoseIntervalMs)
        {
            g_last_send_ms = now;
            send_own_pose(character);
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
    }
} // namespace Presence
