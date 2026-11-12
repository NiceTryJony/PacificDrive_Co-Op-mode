#include "GateSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <chrono>

using namespace RC;
using namespace RC::Unreal;

namespace GateSync
{
    namespace
    {
        constexpr uint64_t PollMs = 250, SettleMs = 3000, ResendMs = 10'000;
        constexpr uint64_t HoldMs = 2000; // Client сам только что сменил ворота — повтор от Host-а, отправленный раньше, его не откатывает
        constexpr const TCHAR* SwitchPath = STR("/Game/Systems/GlobalStateSwitches/GSS_Garage_ExitGateOpened.GSS_Garage_ExitGateOpened");
        constexpr const TCHAR* StateName = STR("GarageFrontDoor"); // единственное состояние переключателя (States)
        constexpr uint8_t PositionOn = 1, PositionOff = 2;         // ESwitchPosition (PenDriverUtils_enums.hpp)

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        uint64_t g_loaded_ms = 0, g_last_poll_ms = 0, g_sent_ms = 0, g_changed_ms = 0;
        int g_seen = -1; // значение у нас на прошлом опросе (считано или применено по пакету); -1 — ещё не знаем
        int g_peer = -1; // что прислал собеседник и мы ещё не применили; -1 — нечего применять
        bool g_is_host = false, g_warned = false;

        UObject* game_instance()
        {
            static Cached::First f{STR("DrivingGameInstance")};
            return f.get();
        }

        UObject* gate_switch() { return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, SwitchPath); }

        int read(UObject* gi, UObject* sw)
        {
            UFunctionCall fn(gi, STR("GetGlobalSwitch"));
            auto* p_sw = fn ? fn.param<UObject*>(STR("SWITCH")) : nullptr;
            auto* p_ret = fn ? fn.param<bool>(STR("ReturnValue")) : nullptr;
            if (!p_sw || !p_ret)
            {
                if (!g_warned) Output::send<LogLevel::Warning>(STR("[Gate] GetGlobalSwitch: функция {}, параметры {}\n"), fn ? STR("есть") : STR("НЕТ"), p_ret ? STR("есть") : STR("НЕТ"));
                g_warned = true;
                return -1;
            }
            *p_sw = sw;
            fn.call();
            return *p_ret ? 1 : 0;
        }

        bool apply(UObject* gi, UObject* sw, bool on)
        {
            UFunctionCall fn(gi, STR("SetGlobalSwitch"));
            auto* p_sw = fn ? fn.param<UObject*>(STR("SWITCH")) : nullptr;
            auto* p_state = fn ? fn.param<FName>(STR("State")) : nullptr;
            auto* p_pos = fn ? fn.param<uint8_t>(STR("Position")) : nullptr;
            if (!p_sw || !p_state || !p_pos) return false;
            *p_sw = sw;
            *p_state = FName(StateName, FNAME_Add);
            *p_pos = on ? PositionOn : PositionOff;
            fn.call();
            return true;
        }
    } // namespace

    void on_map_loaded()
    {
        g_loaded_ms = now_ms();
        g_seen = -1;
        g_peer = -1;
    }

    void on_gate(const std::string& tag, const std::string& value)
    {
        if (tag != "gate" || (value != "0" && value != "1")) return;
        if (!g_is_host && now_ms() - g_changed_ms < HoldMs) return; // свежее собственное изменение важнее повтора Host-а
        g_peer = value == "1" ? 1 : 0;
    }

    void tick(bool is_host)
    {
        g_is_host = is_host;
        uint64_t now = now_ms();
        if (!Net::connected() || now - g_last_poll_ms < PollMs || now - g_loaded_ms < SettleMs) return;
        g_last_poll_ms = now;
        UObject* gi = game_instance();
        UObject* sw = gate_switch();
        if (!gi || !sw) return; // меню, загрузка или ассет не загружен
        int cur = read(gi, sw);
        if (cur < 0) return;
        if (g_peer >= 0)
        {
            int want = g_peer;
            g_peer = -1;
            if (want != cur && apply(gi, sw, want == 1))
            {
                Output::send<LogLevel::Verbose>(STR("[Gate] ворота выезда у собеседника: {} → ставлю\n"), want ? STR("открыты") : STR("закрыты"));
                g_seen = want; // применённое — не наше изменение, обратно не шлём
                return;
            }
        }
        bool changed = cur != g_seen;
        if (changed) g_changed_ms = now;
        // Client первое значение после загрузки не шлёт: оно «закрыто» по умолчанию и закрыло бы ворота у Host-а
        bool send = (changed && (is_host || g_seen >= 0)) || (is_host && now - g_sent_ms >= ResendMs);
        if (send)
        {
            if (changed) Output::send<LogLevel::Verbose>(STR("[Gate] ворота выезда у меня: {} → отправляю\n"), cur ? STR("открыты") : STR("закрыты"));
            Net::send_part("gate", cur ? "1" : "0", Coop::GateState);
            g_sent_ms = now;
        }
        g_seen = cur;
    }
} // namespace GateSync
