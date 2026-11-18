#include "SystemsSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/DriveProbe.h"

#include <algorithm>
#include <chrono>
#include <optional>

using namespace RC;
using namespace RC::Unreal;

// Host опрашивает машину 5 раз в секунду и шлёт полное состояние при изменении. Проценты не чаще раза в секунду:
// топливо течёт непрерывно, а зажигание и свет уходят сразу.
namespace SystemsSync
{
    namespace
    {
        constexpr uint64_t PollMs = 200;
        constexpr uint64_t PercentMinIntervalMs = 1000;
        constexpr int MaxApplyAttempts = 10; // Client: сколько опросов ждать, пока игра примет Set…

        bool g_map_loaded = false;
        bool g_send_full = false;
        std::optional<Coop::SystemsBody> g_sent;   // Host: что собеседник уже знает
        std::optional<Coop::SystemsBody> g_wanted; // Client: получено от Host, ещё не применено
        std::optional<Coop::SystemsBody> g_last;   // для окна статуса
        int g_attempts = 0;
        uint64_t g_last_poll_ms = 0, g_last_percent_ms = 0;

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        UObject* car_manager() { static Cached::First f{STR("BP_CarManager_C")}; return f.get(); }

        uint8_t percent(float current, float maximum)
        {
            if (maximum <= 0.0f) return 0;
            return static_cast<uint8_t>(std::clamp(current * 100.0f / maximum + 0.5f, 0.0f, 100.0f));
        }

        // nullopt — менеджера машины нет (гараж, меню, загрузка) или игра не приняла вызов
        std::optional<Coop::SystemsBody> read_systems()
        {
            auto* manager = car_manager();
            if (!manager) return std::nullopt;
            Coop::SystemsBody out{};

            UFunctionCall ignition(manager, STR("GetCarIgnitionState"));
            auto* p_ignition = ignition ? ignition.param<uint8_t>(STR("CarIgnitionState")) : nullptr;
            UFunctionCall lights(manager, STR("GetCarLightsOn"));
            auto* p_lights = lights ? lights.param<bool>(STR("AreCarLightsOn")) : nullptr;
            UFunctionCall fuel(manager, STR("GetAggregateFuel"));
            auto* p_fuel = fuel ? fuel.param<int32_t>(STR("AggregateFuelCurrent")) : nullptr;
            auto* p_fuel_max = fuel ? fuel.param<int32_t>(STR("AggregateFuelMax")) : nullptr;
            UFunctionCall battery(manager, STR("GetAggregateBattery"));
            auto* p_battery = battery ? battery.param<float>(STR("AggregateBatteryCurrent")) : nullptr;
            auto* p_battery_max = battery ? battery.param<float>(STR("AggregateBatteryMax")) : nullptr;
            UFunctionCall radio(manager, STR("GetCarRadioOnOffState"));
            auto* p_radio = radio ? radio.param<bool>(STR("CarState_Radio")) : nullptr;
            if (!p_ignition || !p_lights || !p_fuel || !p_fuel_max || !p_battery || !p_battery_max) return std::nullopt;

            ignition.call();
            lights.call();
            if (p_radio) radio.call(); // радио необязательно: нет функции — считаем выключенным, остальное синхронизируется
            fuel.call();
            battery.call();
            if (*p_fuel_max <= 0 || *p_battery_max <= 0) return std::nullopt; // карта грузится: предметов машины ещё нет — иначе Client увидит 0 %
            out.Ignition = std::min<uint8_t>(*p_ignition, Coop::IgnitionMax);
            out.Lights = *p_lights ? 1 : 0;
            out.Radio = p_radio && *p_radio ? 1 : 0;
            auto* wipers = property<bool>(manager, STR("WiperBladesOnOff"));
            out.Wipers = wipers && *wipers ? 1 : 0;
            out.Fuel = percent(static_cast<float>(*p_fuel), static_cast<float>(*p_fuel_max));
            out.Battery = percent(*p_battery, *p_battery_max);
            return out;
        }

        // Client: привести зажигание и свет куклы к присланным. true — уже так
        bool apply_to_game(const Coop::SystemsBody& want, const Coop::SystemsBody& local)
        {
            auto* manager = car_manager();
            if (!manager) return false;
            struct Suppress // эти вызовы — не действия Client-водителя, Host про них не спрашиваем (DriveAction)
            {
                Suppress() { DriveProbe::suppress_actions(true); }
                ~Suppress() { DriveProbe::suppress_actions(false); }
            } suppress;
            if (local.Ignition != want.Ignition)
            {
                UFunctionCall fn(manager, STR("SetCarIgnitionState"));
                if (auto* p = fn ? fn.param<uint8_t>(STR("NewState")) : nullptr)
                {
                    *p = want.Ignition;
                    fn.call();
                }
                else Output::send<LogLevel::Error>(STR("[SystemsSync] нет SetCarIgnitionState/NewState\n"));
            }
            if (local.Lights != want.Lights)
            {
                UFunctionCall fn(manager, STR("SetCarLightsOn"));
                if (auto* p = fn ? fn.param<bool>(STR("Set Car Lights On")) : nullptr)
                {
                    *p = want.Lights != 0;
                    fn.call();
                }
                else Output::send<LogLevel::Error>(STR("[SystemsSync] нет SetCarLightsOn/параметра\n"));
            }
            if (local.Radio != want.Radio)
            {
                UFunctionCall fn(manager, STR("SetCarRadioOnOffState"));
                auto* p = fn ? fn.param<bool>(STR("CarState_Radio")) : nullptr;
                auto* skip = fn ? fn.param<bool>(STR("SkipQuirkConditions")) : nullptr;
                if (p && skip)
                {
                    *p = want.Radio != 0;
                    *skip = true; // у куклы не запускаем реплики и условия «причуд» — их ведёт Host
                    fn.call();
                }
                else Output::send<LogLevel::Error>(STR("[SystemsSync] нет SetCarRadioOnOffState/параметров\n"));
            }
            if (local.Wipers != want.Wipers)
            {
                // Переменная менеджера щётки не двигает (проба 2026-11-18): их запускает ToggleWiperBlades машины, он же переключает переменную
                UFunctionCall fn(Cached::car(), STR("ToggleWiperBlades"));
                if (fn) fn.call();
                else Output::send<LogLevel::Error>(STR("[SystemsSync] нет ToggleWiperBlades у машины\n"));
            }
            return local.Ignition == want.Ignition && local.Lights == want.Lights && local.Radio == want.Radio && local.Wipers == want.Wipers;
        }
    } // namespace

    void on_map_loaded()
    {
        g_map_loaded = true;
    }

    void on_connected(bool is_host)
    {
        if (is_host) g_send_full = true;
    }

    const Coop::SystemsBody* last() { return g_last ? &*g_last : nullptr; }

    void on_systems(const Coop::SystemsBody& body)
    {
        Output::send<LogLevel::Verbose>(STR("[SystemsSync] получено: зажигание {}, свет {}, радио {}, дворники {}, топливо {}%, аккумулятор {}%\n"), body.Ignition, body.Lights, body.Radio,
                                        body.Wipers, body.Fuel, body.Battery);
        g_wanted = body;
        g_last = body;
        g_wanted->Ignition = std::min<uint8_t>(g_wanted->Ignition, Coop::IgnitionMax); // мусор из сети в игру не передаём
        g_attempts = 0;
    }

    void tick(bool is_host)
    {
        if (g_map_loaded)
        {
            g_map_loaded = false;
            g_sent.reset(); // новая карта — собеседнику слать заново
            g_send_full = is_host;
        }
        if (!Net::connected()) return;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs) return;
        g_last_poll_ms = now;

        auto local = read_systems();
        if (!local) return;

        if (!is_host)
        {
            if (g_wanted && (apply_to_game(*g_wanted, *local) || ++g_attempts >= MaxApplyAttempts))
            {
                if (g_attempts >= MaxApplyAttempts) Output::send<LogLevel::Warning>(STR("[SystemsSync] игра не приняла зажигание/свет/радио/дворники за {} опросов — оставляю как есть\n"), MaxApplyAttempts);
                g_wanted.reset();
            }
            return;
        }

        bool changed = !g_sent || Coop::systems_changed(*g_sent, *local);
        if (!changed && !g_send_full) return;
        // Проценты одни (зажигание и свет те же) — не чаще раза в секунду; полное состояние и смена зажигания/света — сразу
        bool urgent = g_send_full || !g_sent || g_sent->Ignition != local->Ignition || g_sent->Lights != local->Lights || g_sent->Radio != local->Radio || g_sent->Wipers != local->Wipers;
        if (!urgent && now - g_last_percent_ms < PercentMinIntervalMs) return;
        g_send_full = false;
        g_last_percent_ms = now;
        g_sent = local;
        g_last = local;
        Net::send_systems(*local);
        Output::send<LogLevel::Verbose>(STR("[SystemsSync] отправлено: зажигание {}, свет {}, радио {}, дворники {}, топливо {}%, аккумулятор {}%\n"), local->Ignition, local->Lights,
                                        local->Radio, local->Wipers, local->Fuel, local->Battery);
    }
} // namespace SystemsSync
