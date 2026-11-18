#include "HudFeed.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunction.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/PassengerSync.h"
#include "Sync/Presence.h"
#include "Sync/StatusFile.h"

#include <cmath>
#include <format>
#include <fstream>
#include <string>

using namespace RC;
using namespace RC::Unreal;

namespace HudFeed
{
    namespace
    {
        constexpr uint64_t IntervalMs = 50; // как поза (20 Гц): при 200 мс метка над напарником отставала на бегу на 2–3 м
        constexpr float NearCarCm = 1200;
        struct Vec // FVector / FRotator (Pitch, Yaw, Roll) в UE 4.27 — три float
        {
            float X, Y, Z;
        };

        uint64_t g_last_ms = 0, g_left_at_ms = 0; // g_left_at_ms: когда пропала связь, 0 — на связи или ещё не подключались
        int g_link_rev = 0, g_cfg_rev = 0;
        bool g_connected = false, g_force = false;
        std::string g_last;
        std::fstream g_out;

        bool actor_location(UObject* actor, Vec& out)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* r = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!r) return false;
            fn.call();
            out = *r;
            return true;
        }

        // Камера: откуда смотрим и куда (рыскание). Нет персонажа/камеры — меню или загрузка
        bool camera(Vec& from, float& yaw)
        {
            auto* ch = Cached::character();
            auto** ctrl = ch ? property<UObject*>(ch, STR("Controller")) : nullptr;
            auto** cam = ctrl && *ctrl ? property<UObject*>(*ctrl, STR("PlayerCameraManager")) : nullptr;
            if (!cam || !*cam) return false;
            UFunctionCall gl(*cam, STR("GetCameraLocation")), gr(*cam, STR("GetCameraRotation"));
            auto *loc = gl ? gl.param<Vec>(STR("ReturnValue")) : nullptr, *rot = gr ? gr.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!loc || !rot) return false;
            gl.call(), gr.call();
            from = *loc;
            yaw = rot->Y;
            return true;
        }

        const char* seat_name(int seat) { return seat == 0 ? "left" : seat == 1 ? "right" : seat == Coop::SeatDriver ? "driver" : "-"; }

        std::string mate_fields(bool host)
        {
            const Coop::PoseBody* m = Presence::mate();
            Vec cam{};
            float yaw = 0;
            if (!m || !camera(cam, yaw)) return "mate=0\n";
            constexpr float Deg = 180.f / 3.14159265f;
            float dx = m->X - cam.X, dy = m->Y - cam.Y;
            float dist_m = std::sqrt(dx * dx + dy * dy + (m->Z - cam.Z) * (m->Z - cam.Z)) / 100.f;
            // Угол на напарника от взгляда камеры по горизонтали, −180…180; плюс — вправо (в UE ось Y смотрит вправо)
            int bearing = static_cast<int>(std::lround(std::remainder(std::atan2(dy, dx) * Deg - yaw, 360.f)));
            bool in_car = (m->Flags & Coop::PoseInCar) != 0;
            // Host знает, куда посадил пассажира; у Client напарник — водитель
            int seat = !in_car ? -1 : host ? PassengerSync::host_passenger_seat() : Coop::SeatDriver;
            Vec car{};
            bool near_car = false;
            if (auto* c = Cached::car(); c && actor_location(c, car))
                near_car = std::hypot(m->X - car.X, m->Y - car.Y, m->Z - car.Z) < NearCarCm;
            // сам в машине: посажен модом (Client) или игрой (PercentInCar, Host) — метка над напарником, когда оба в машине, не нужна
            auto* percent = Cached::character() ? property<float>(Cached::character(), STR("PercentInCar")) : nullptr;
            bool me_in_car = Presence::seated() || (percent && *percent > 0.5f);
            // mate_x/y/z — центр капсулы, см: HUD ставит метку над головой (проекция на экран — в Lua каждый кадр)
            return std::format("mate=1\nmate_dist_m={:.0f}\nmate_bearing={}\nmate_in_car={}\nmate_seat={}\nmate_near_car={}\nmate_x={:.0f}\nmate_y={:.0f}\nmate_z={:.0f}\nme_in_car={}\n",
                               dist_m, bearing, in_car ? 1 : 0, seat_name(seat), near_car ? 1 : 0, m->X, m->Y, m->Z, me_in_car ? 1 : 0);
        }
    } // namespace

    void config_changed()
    {
        ++g_cfg_rev;
        g_force = true;
    }

    void tick(const std::filesystem::path& file, bool host)
    {
        uint64_t now = Cached::now_ms();
        // Подключение/отключение — каждый кадр (одно чтение флага), чтобы баннер в HUD появился без ожидания интервала
        if (bool c = Net::connected(); c != g_connected)
        {
            g_connected = c;
            ++g_link_rev;
            g_left_at_ms = c ? 0 : now;
            g_force = true;
        }
        if (!g_force && now - g_last_ms < IntervalMs) return;
        g_last_ms = now;
        g_force = false;

        long long left_s = g_left_at_ms ? static_cast<long long>((now - g_left_at_ms) / 1000) : -1;
        std::string text = std::format("link_rev={}\ncfg_rev={}\nleft_s={}\n", g_link_rev, g_cfg_rev, left_s) + (g_connected ? mate_fields(host) : "mate=0\n");
        if (text == g_last) return; // запись только при изменении
        g_last = text;
        StatusFile::write_fixed(g_out, file, text, 384); // с координатами ~230 байт
    }
} // namespace HudFeed
