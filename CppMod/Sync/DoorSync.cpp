#include "DoorSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <atomic>
#include <chrono>
#include <optional>

using namespace RC;
using namespace RC::Unreal;

// Опрос 10 Гц: двери меняются руками игрока, событие OnDoorChanged делегатом ловить дороже, чем шесть вызовов GetDoorState.
// Host главный: запрос Client-а он применяет у себя и рассылает итог (DoorState). Свои изменения каждый шлёт сразу.
namespace DoorSync
{
    namespace
    {
        constexpr uint64_t PollMs = 100;
        constexpr uint64_t GraceMs = 3000; // после применения чужого изменения дверь едет/качается — расхождение не считаем действием игрока

        std::atomic<bool> g_map_loaded{false};
        uint8_t g_known = 0;           // состояние, о котором собеседник уже знает; расхождение с игрой = действие игрока
        bool g_baseline = false;       // g_known снят с игры после загрузки карты
        bool g_send_full = false;      // Host: отправить полное состояние (подключился клиент)
        uint64_t g_last_poll_ms = 0;
        uint64_t g_grace_until_ms[Coop::DoorCount]{}; // 0 — окна нет
        uint8_t g_pending_mask = 0, g_pending_changed = 0; // Client: получено от Host, машины ещё нет (загрузка зоны)

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        UObject* car_manager() { return UObjectGlobals::FindFirstOf(STR("BP_CarManager_C")); }

        // Биты открытых дверей из игры; nullopt — менеджера машины нет (гараж, меню, загрузка)
        std::optional<uint8_t> read_doors()
        {
            auto* manager = car_manager();
            if (!manager) return std::nullopt;
            uint8_t mask = 0;
            for (uint8_t door = 0; door < Coop::DoorCount; ++door)
            {
                UFunctionCall fn(manager, STR("GetDoorState"));
                auto* p_door = fn ? fn.param<uint8_t>(STR("Door")) : nullptr;
                auto* p_open = fn ? fn.param<bool>(STR("Open")) : nullptr;
                if (!p_door || !p_open) return std::nullopt;
                *p_door = door;
                fn.call();
                if (*p_open) mask |= static_cast<uint8_t>(1u << door);
            }
            return mask;
        }

        // Привести двери из changed к состоянию mask (только те, что в игре сейчас иначе). false — машины нет, применить не вышло
        bool apply_to_game(uint8_t mask, uint8_t changed)
        {
            auto local = read_doors();
            auto* manager = car_manager();
            if (!local || !manager) return false;
            uint64_t now = now_ms();
            for (uint8_t door = 0; door < Coop::DoorCount; ++door)
            {
                auto bit = static_cast<uint8_t>(1u << door);
                if (!(changed & bit) || ((mask ^ *local) & bit) == 0) continue;
                UFunctionCall fn(manager, STR("AttemptOpenOrShutDoor"));
                auto* p_door = fn ? fn.param<uint8_t>(STR("Door")) : nullptr;
                auto* p_open = fn ? fn.param<bool>(STR("Open")) : nullptr;
                if (!p_door || !p_open)
                {
                    Output::send<LogLevel::Error>(STR("[DoorSync] нет AttemptOpenOrShutDoor/параметров — дверь {} не тронута\n"), door);
                    continue;
                }
                *p_door = door;
                *p_open = (mask & bit) != 0;
                fn.call();
                g_grace_until_ms[door] = now + GraceMs;
                Output::send<LogLevel::Verbose>(STR("[DoorSync] дверь {} → {}\n"), door, (mask & bit) ? STR("открыть") : STR("закрыть"));
            }
            return true;
        }

        void remember(uint8_t mask, uint8_t changed)
        {
            g_known = static_cast<uint8_t>((g_known & ~changed) | (mask & changed));
        }
    } // namespace

    void on_map_loaded()
    {
        g_map_loaded = true;
    }

    int known_mask() { return g_baseline ? g_known : -1; }

    void on_connected(bool is_host)
    {
        if (is_host) g_send_full = true;
    }

    void on_door(Coop::PacketId id, const Coop::DoorBody& body, bool is_host)
    {
        uint8_t changed = body.Changed & Coop::DoorAll;
        Output::send<LogLevel::Verbose>(STR("[DoorSync] получено {}: открыты {:06b}, изменились {:06b}\n"), id == Coop::DoorState ? STR("DoorState") : STR("DoorRequest"),
                                        body.Mask & Coop::DoorAll, changed);
        if (id == Coop::DoorRequest && is_host)
        {
            if (!g_baseline)
            {
                Output::send<LogLevel::Warning>(STR("[DoorSync] запрос двери до снятия исходного состояния — отклонён\n"));
                return;
            }
            uint8_t next = g_known;
            auto reply = Coop::apply_door_request(next, body);
            if (!reply.Changed) return; // у Host уже так — собеседник знает
            if (!apply_to_game(reply.Mask, reply.Changed))
            {
                Output::send<LogLevel::Warning>(STR("[DoorSync] запрос двери: машины нет — отклонён\n"));
                return;
            }
            g_known = next;
            Net::send_door(Coop::DoorState, reply); // итог — обоим: Client увидит своё же изменение подтверждённым
        }
        else if (id == Coop::DoorState && !is_host)
        {
            g_pending_mask = static_cast<uint8_t>((g_pending_mask & ~changed) | (body.Mask & changed));
            g_pending_changed |= changed; // применится в tick, когда есть машина и снят baseline
        }
    }

    void tick(bool is_host)
    {
        if (g_map_loaded.exchange(false)) g_baseline = false;
        if (!Net::connected()) return;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs) return;
        g_last_poll_ms = now;

        auto local = read_doors();
        if (!local)
        {
            g_baseline = false;
            return;
        }
        if (!g_baseline)
        {
            g_known = *local;
            g_baseline = true;
            Output::send<LogLevel::Verbose>(STR("[DoorSync] исходное состояние дверей: {:06b}\n"), g_known);
        }
        if (g_pending_changed && apply_to_game(g_pending_mask, g_pending_changed))
        {
            remember(g_pending_mask, g_pending_changed);
            g_pending_changed = 0;
        }
        if (g_send_full && is_host)
        {
            g_send_full = false;
            Net::send_door(Coop::DoorState, {*local, Coop::DoorAll});
            g_known = *local;
            Output::send<LogLevel::Verbose>(STR("[DoorSync] клиенту отправлено полное состояние: {:06b}\n"), *local);
        }

        uint8_t changed = 0;
        for (uint8_t door = 0; door < Coop::DoorCount; ++door)
        {
            auto bit = static_cast<uint8_t>(1u << door);
            bool differs = ((*local ^ g_known) & bit) != 0;
            if (g_grace_until_ms[door])
            {
                if (!differs) g_grace_until_ms[door] = 0; // дошла до нужного положения — окно закрыто
                else if (now < g_grace_until_ms[door]) continue;
                else
                {
                    // не выполнила: считаем реальностью, не действием игрока (эхо назад не шлём)
                    g_grace_until_ms[door] = 0;
                    g_known = static_cast<uint8_t>((g_known & ~bit) | (*local & bit));
                    Output::send<LogLevel::Warning>(STR("[DoorSync] дверь {} не приняла команду за {} мс — оставляю как есть\n"), door, GraceMs);
                    continue;
                }
            }
            if (differs) changed |= bit;
        }
        if (!changed) return;
        remember(*local, changed);
        Net::send_door(is_host ? Coop::DoorState : Coop::DoorRequest, {*local, changed});
        Output::send<LogLevel::Verbose>(STR("[DoorSync] дверь изменена игроком: открыты {:06b}, изменились {:06b} → {}\n"), *local, changed,
                                        is_host ? STR("DoorState") : STR("DoorRequest"));
    }
} // namespace DoorSync
