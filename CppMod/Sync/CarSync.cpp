#include "CarSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <optional>
#include <cstring>
#include <string>
#include <vector>

// То же объявление, что в Windows.h — без тяжёлого заголовка рядом с Unreal
extern "C" __declspec(dllimport) struct HINSTANCE__* __stdcall GetModuleHandleW(const wchar_t* name);

using namespace RC;
using namespace RC::Unreal;

// Машина IWCarPhysics: одно rigid body `Chassis` (RECON.md). У Client физику Chassis выключаем — машину ведут пакеты Host
// (спека 3.2.1: клиент не симулирует критичное состояние). Интерполяция между снимками с задержкой — плавно при джиттере.
namespace CarSync
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
        struct Snapshot
        {
            uint64_t ReceivedMs;
            Coop::CarStateBody State;
        };

        constexpr uint64_t InterpDelayMs = 100;     // показываем прошлое на 100 мс: всегда есть два снимка для интерполяции
        constexpr uint64_t MaxExtrapolateMs = 250;  // пакеты пропали — едем по скорости не дольше этого
        constexpr uint64_t StaleMs = 5000;          // Host молчит — возвращаем машине физику
        constexpr size_t MaxSnapshots = 32;

        std::atomic<bool> g_map_loaded{false};
        std::deque<Snapshot> g_snapshots;
        UObject* g_car = nullptr;
        bool g_kinematic = false, g_missing_logged = false;
        uint64_t g_last_send_ms = 0;
        Coop::CarStateBody g_applied{}; // куда кукла поставлена последним apply()
        std::optional<float> g_ground_offset; // высота центра машины над землёй до захвата — чтобы вернуть её на землю
        constexpr uint32_t HitLocationOffset = 0x0C;  // FHitResult::Location (CXXHeaderDump)
        constexpr float TraceUp = 300, TraceDown = 20000, ReleaseMargin = 20; // см

        // Колёса куклы: физика выключена, нативный IWCarPhysics их не крутит. Пишем живые float-поля UWheelComponent
        // (RECON.md 6.16): угол вращения в радианах и руль. Байты кода, где эти поля читаются, сверяем — другая версия игры = не трогаем
        constexpr uint32_t WheelAngularPosition = 0x660, WheelSteer = 0x4E8;
        constexpr float WheelRadiusCm = 40, WheelBaseCm = 298, MaxSteerDeg = 35;
        struct CodeSig
        {
            uint32_t Rva;
            uint8_t Bytes[8];
            size_t Size;
        };
        constexpr CodeSig WheelSigs[] = {{0xa8a1e5, {0x8b, 0x81, 0x60, 0x06, 0x00, 0x00}, 6},
                                         {0xa8a205, {0x8b, 0x81, 0x5c, 0x06, 0x00, 0x00}, 6},
                                         {0xa7febd, {0xf3, 0x0f, 0x10, 0x87, 0xe8, 0x04, 0x00, 0x00}, 8}};
        struct Wheel
        {
            UObject* Object;
            bool Front;
        };
        std::vector<Wheel> g_wheels;
        bool g_wheels_ok = false, g_wheels_checked = false;
        float g_wheel_pos = 0, g_steer_deg = 0;
        uint64_t g_wheel_ms = 0;
        float g_prev_yaw = 0;

        bool wheel_code_matches()
        {
            auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            for (const auto& sig : WheelSigs)
                if (std::memcmp(base + sig.Rva, sig.Bytes, sig.Size)) return false;
            return true;
        }

        void find_wheels(UObject* car)
        {
            g_wheels.clear();
            std::vector<UObject*> all;
            UObjectGlobals::FindAllOf(STR("WheelComponent"), all);
            for (auto* w : all)
                if (w && w->GetOuterPrivate() == car) g_wheels.push_back({w, w->GetName().find(STR("Front")) != std::wstring::npos});
            Output::send<LogLevel::Verbose>(STR("[CarSync] колёс куклы найдено: {}\n"), g_wheels.size());
        }

        // Скорость вдоль кузова и курсовая скорость → вращение колёс и руль (руль по схеме «велосипеда»)
        void animate_wheels(UObject* car, const Coop::CarStateBody& s, float yaw_deg, uint64_t now)
        {
            if (!g_wheels_checked)
            {
                g_wheels_checked = true;
                g_wheels_ok = wheel_code_matches();
                if (!g_wheels_ok) Output::send<LogLevel::Error>(STR("[CarSync] байты кода колёс не совпали — другая версия игры, колёса куклы не крутим\n"));
            }
            if (!g_wheels_ok) return;
            if (g_wheels.empty()) find_wheels(car); // список сбрасывается при загрузке карты, как и g_car
            float dt = g_wheel_ms ? float(now - g_wheel_ms) / 1000.0f : 0.0f;
            g_wheel_ms = now;
            if (dt <= 0 || dt > 0.5f) { g_prev_yaw = yaw_deg; return; }
            constexpr float Rad = 3.14159265f / 180.0f;
            float forward = s.VX * std::cos(yaw_deg * Rad) + s.VY * std::sin(yaw_deg * Rad); // см/с, минус = задним ходом
            float yaw_rate = (std::fmod(yaw_deg - g_prev_yaw + 540.0f, 360.0f) - 180.0f) / dt * Rad;          // рад/с
            g_prev_yaw = yaw_deg;
            g_wheel_pos = std::fmod(g_wheel_pos + forward / WheelRadiusCm * dt, 2 * 3.14159265f);
            float target = std::fabs(forward) > 30 ? std::atan(WheelBaseCm * yaw_rate / forward) / Rad : 0.0f;
            target = (std::max)(-MaxSteerDeg, (std::min)(MaxSteerDeg, target));
            g_steer_deg += (target - g_steer_deg) * (std::min)(1.0f, dt * 8.0f); // сглаживание: курсовая скорость шумит
            for (const auto& w : g_wheels)
            {
                auto* base = reinterpret_cast<uint8_t*>(w.Object);
                *reinterpret_cast<float*>(base + WheelAngularPosition) = g_wheel_pos;
                if (w.Front) *reinterpret_cast<float*>(base + WheelSteer) = g_steer_deg;
            }
        }

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        UObject* find_car() { return Cached::car(); } // BP_PlayerCarNew_C : ANewVehiclePawn

        void set_simulate_physics(UObject* car, bool simulate)
        {
            auto** chassis = property<UObject*>(car, STR("Chassis"));
            UFunctionCall fn(chassis ? *chassis : nullptr, STR("SetSimulatePhysics"));
            if (auto* p = fn ? fn.param<bool>(STR("bSimulate")) : nullptr)
            {
                *p = simulate;
                fn.call();
            }
        }

        Vec actor_location(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {};
            fn.call();
            return *ret;
        }

        // Земля под точкой: луч сверху вниз, сама машина игнорируется (bIgnoreSelf + она же WorldContext)
        std::optional<float> ground_z(UObject* car, const Vec& at)
        {
            auto* lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
            UFunctionCall fn(lib, STR("LineTraceSingle"));
            auto* p_ctx = fn ? fn.param<UObject*>(STR("WorldContextObject")) : nullptr;
            auto* p_start = fn ? fn.param<Vec>(STR("Start")) : nullptr;
            auto* p_end = fn ? fn.param<Vec>(STR("End")) : nullptr;
            auto* p_self = fn ? fn.param<bool>(STR("bIgnoreSelf")) : nullptr;
            auto* p_hit = fn ? fn.param<uint8_t>(STR("OutHit")) : nullptr;
            auto* p_ret = fn ? fn.param<bool>(STR("ReturnValue")) : nullptr;
            if (!p_ctx || !p_start || !p_end || !p_self || !p_hit || !p_ret) return std::nullopt;
            *p_ctx = car;
            *p_start = {at.X, at.Y, at.Z + TraceUp};
            *p_end = {at.X, at.Y, at.Z - TraceDown};
            *p_self = true; // TraceChannel = 0 (Visibility) — нули буфера
            fn.call();
            if (!*p_ret) return std::nullopt;
            return reinterpret_cast<const Vec*>(p_hit + HitLocationOffset)->Z;
        }

        void zero_velocity(UObject* car)
        {
            auto** chassis = property<UObject*>(car, STR("Chassis"));
            for (const TCHAR* function : {STR("SetPhysicsLinearVelocity"), STR("SetPhysicsAngularVelocityInDegrees")})
            {
                UFunctionCall fn(chassis ? *chassis : nullptr, function);
                if (fn) fn.call(); // NewVel = 0, bAddToCurrent = false, BoneName = None — нули буфера
            }
        }

        void send_state(UObject* car)
        {
            UFunctionCall get_loc(car, STR("K2_GetActorLocation"));
            UFunctionCall get_rot(car, STR("K2_GetActorRotation"));
            UFunctionCall get_vel(car, STR("GetVelocity"));
            auto* loc = get_loc ? get_loc.param<Vec>(STR("ReturnValue")) : nullptr;
            auto* rot = get_rot ? get_rot.param<Rot>(STR("ReturnValue")) : nullptr;
            auto* vel = get_vel ? get_vel.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!loc || !rot || !vel) return;
            get_loc.call();
            get_rot.call();
            get_vel.call();
            Net::send_car_state({loc->X, loc->Y, loc->Z, rot->Pitch, rot->Yaw, rot->Roll, vel->X, vel->Y, vel->Z});
        }

        float lerp_angle(float a, float b, float t)
        {
            float d = std::fmod(b - a + 540.0f, 360.0f) - 180.0f; // кратчайший путь
            return a + d * t;
        }

        void apply(UObject* car, const Vec& p, const Rot& r)
        {
            UFunctionCall fn(car, STR("K2_SetActorLocationAndRotation"));
            auto* p_loc = fn ? fn.param<Vec>(STR("NewLocation")) : nullptr;
            auto* p_rot = fn ? fn.param<Rot>(STR("NewRotation")) : nullptr;
            auto* p_teleport = fn ? fn.param<bool>(STR("bTeleport")) : nullptr;
            if (!p_loc || !p_rot || !p_teleport) return;
            *p_loc = p;
            *p_rot = r;
            *p_teleport = true;
            fn.call();
        }

        void release_car(const wchar_t* reason)
        {
            if (g_car && g_kinematic)
            {
                // Последняя поза Host могла быть в земле/на склоне: включить физику там = машину рвёт и ломает (тест 2026-10-26).
                // Ставим ровно на землю на прежнюю высоту и с нулевой скоростью, потом физика
                Vec at{g_applied.X, g_applied.Y, g_applied.Z};
                if (auto ground = ground_z(g_car, at); ground && g_ground_offset)
                    apply(g_car, {at.X, at.Y, *ground + *g_ground_offset + ReleaseMargin}, {0, g_applied.Yaw, 0});
                set_simulate_physics(g_car, true);
                zero_velocity(g_car);
            }
            if (g_kinematic) Output::send<LogLevel::Verbose>(STR("[CarSync] машина снова своя ({})\n"), reason);
            g_kinematic = false;
            g_snapshots.clear();
        }

        void client_tick()
        {
            if (g_snapshots.empty()) return;
            uint64_t now = now_ms();
            if (now - g_snapshots.back().ReceivedMs > StaleMs)
            {
                release_car(L"Host молчит 5 с");
                return;
            }
            if (!g_car) g_car = find_car();
            if (!g_car)
            {
                if (!g_missing_logged) Output::send<LogLevel::Warning>(STR("[CarSync] машины в мире нет — CarState игнорируется\n"));
                g_missing_logged = true;
                return;
            }
            if (!g_kinematic)
            {
                Vec car_at = actor_location(g_car);
                auto ground = ground_z(g_car, car_at);
                g_ground_offset = ground ? std::optional<float>(car_at.Z - *ground) : std::nullopt;
                UObject* character = Cached::character();
                Output::send<LogLevel::Verbose>(STR("[CarSync] до захвата: машина z={:.0f}, земля z={}, персонаж z={:.0f}\n"), car_at.Z,
                                                ground ? std::to_wstring(static_cast<int>(*ground)) : std::wstring(L"?"), character ? actor_location(character).Z : 0.0f);
                set_simulate_physics(g_car, false);
                g_kinematic = true;
                Output::send<LogLevel::Verbose>(STR("[CarSync] машина под управлением Host (физика выключена): {}\n"), g_car->GetFullName());
            }

            uint64_t render = now - InterpDelayMs;
            const Snapshot* a = &g_snapshots.front();
            const Snapshot* b = nullptr;
            for (const auto& s : g_snapshots)
            {
                if (s.ReceivedMs <= render) a = &s;
                else
                {
                    b = &s;
                    break;
                }
            }
            const auto& sa = a->State;
            Vec p{sa.X, sa.Y, sa.Z};
            Rot r{sa.Pitch, sa.Yaw, sa.Roll};
            if (b && b->ReceivedMs > a->ReceivedMs && render >= a->ReceivedMs)
            {
                float t = float(render - a->ReceivedMs) / float(b->ReceivedMs - a->ReceivedMs);
                const auto& sb = b->State;
                p = {sa.X + (sb.X - sa.X) * t, sa.Y + (sb.Y - sa.Y) * t, sa.Z + (sb.Z - sa.Z) * t};
                r = {lerp_angle(sa.Pitch, sb.Pitch, t), lerp_angle(sa.Yaw, sb.Yaw, t), lerp_angle(sa.Roll, sb.Roll, t)};
            }
            else if (!b && render > a->ReceivedMs)
            {
                // Новых снимков нет (потеря/джиттер) — недолго продолжаем по скорости
                float dt = float((std::min)(render - a->ReceivedMs, MaxExtrapolateMs)) / 1000.0f;
                p = {sa.X + sa.VX * dt, sa.Y + sa.VY * dt, sa.Z + sa.VZ * dt};
            }
            apply(g_car, p, r);
            animate_wheels(g_car, sa, r.Yaw, now);
            g_applied = {p.X, p.Y, p.Z, r.Pitch, r.Yaw, r.Roll, 0, 0, 0};
        }
    } // namespace

    void on_car_state(const Coop::CarStateBody& state)
    {
        g_snapshots.push_back({now_ms(), state});
        while (g_snapshots.size() > MaxSnapshots) g_snapshots.pop_front();
    }

    void on_map_loaded() { g_map_loaded = true; }

    UObjectPtr puppet_car() { return g_kinematic ? g_car : nullptr; }

    bool puppet_pose(Coop::CarStateBody& out)
    {
        if (!g_kinematic) return false;
        out = g_applied;
        return true;
    }

    void tick(float, bool is_host)
    {
        if (g_map_loaded.exchange(false))
        {
            g_car = nullptr; // старая машина ушла вместе с миром; новую найдём заново
            g_kinematic = false;
            g_missing_logged = false;
            g_snapshots.clear();
            g_ground_offset.reset();
            g_wheels.clear();
            g_wheel_ms = 0;
        }
        if (!Net::connected())
        {
            if (!is_host && g_kinematic) release_car(L"нет связи");
            return;
        }
        if (!is_host)
        {
            client_tick();
            return;
        }
        uint64_t now = now_ms();
        if (now - g_last_send_ms < Net::adaptive_interval_ms(Coop::CarStateIntervalMs)) return;
        g_last_send_ms = now;
        if (!g_car) g_car = find_car();
        if (g_car) send_state(g_car);
    }
} // namespace CarSync
