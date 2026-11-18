#include "GateSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace GateSync
{
    namespace
    {
        constexpr uint64_t PollMs = 500, SettleMs = 3000, ResendMs = 10'000;
        constexpr int Explicit = 2; // ELegRevealState: NotRevealed 0, Implicit 1, Explicit 2 (PenDriverPro_enums.hpp:274)

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        uint64_t g_loaded_ms = 0, g_last_poll_ms = 0, g_sent_ms = 0;
        std::string g_sent;              // что отправлено последним (Host — всё раскрытое, Client — раскрытое выше, чем у Host)
        std::map<int, int> g_wanted;     // индекс RouteShapeLeg → статус у напарника, ещё не применённый
        std::map<int, int> g_host;       // Client: последнее полное состояние от Host
        bool g_host_seen = false;        // Client: Host уже прислал состояние после загрузки
        bool g_is_host = true;           // роль из tick (вызывается каждый кадр, раньше любых пакетов)
        bool g_warned = false;

        // Текущий маршрут: ProgressionManager.GetWorldState (так его берёт директор двери)
        UObject* world_state()
        {
            static Cached::First pm{STR("BP_ProgressionManager_C")};
            UFunctionCall fn(pm.get(), STR("GetWorldState"));
            auto* ret = fn ? fn.param<UObject*>(STR("ReturnValue")) : nullptr;
            if (!ret) return nullptr;
            fn.call();
            return *ret;
        }

        // Индекс из имени отрезка формы «RouteShapeLeg_7»: имена объектов отрезков у Host и Client разные, индекс формы — один
        int shape_index(UObject* leg)
        {
            auto** shape = property<UObject*>(leg, STR("ShapeLeg"));
            if (!shape || !*shape) return -1;
            auto name = (*shape)->GetName();
            auto pos = name.rfind(STR('_'));
            if (pos == name.npos) return -1;
            try { return std::stoi(name.substr(pos + 1)); } catch (...) { return -1; }
        }

        struct Leg
        {
            UObject* object;
            int index;
            uint8_t* reveal;
        };

        std::vector<Leg> legs_of(UObject* route)
        {
            std::vector<UObject*> all;
            UObjectGlobals::FindAllOf(STR("BaseLegState"), all);
            std::vector<Leg> legs;
            for (auto* leg : all)
            {
                if (leg->GetOuterPrivate() != route) continue; // отрезки других маршрутов (их несколько в памяти)
                auto* reveal = property<uint8_t>(leg, STR("RevealState"));
                int index = shape_index(leg);
                if (reveal && index >= 0) legs.push_back({leg, index, reveal});
            }
            return legs;
        }

        std::string encode(const std::vector<Leg>& legs)
        {
            std::map<int, int> revealed;
            for (auto& l : legs)
                if (*l.reveal > 0) revealed[l.index] = *l.reveal;
            std::string out;
            for (auto [index, state] : revealed) out += (out.empty() ? "" : ",") + std::to_string(index) + ":" + std::to_string(state);
            return out;
        }

        bool reveal(UObject* leg, int state)
        {
            UFunctionCall fn(leg, STR("RevealLeg"));
            auto* p_mode = fn ? fn.param<uint8_t>(STR("RevealMode")) : nullptr;
            auto* p_suppress = fn ? fn.param<bool>(STR("bSuppressImplicitReveals")) : nullptr;
            if (!p_mode || !p_suppress)
            {
                if (!g_warned) Output::send<LogLevel::Warning>(STR("[Gate] RevealLeg: функция {}, параметры {}\n"), fn ? STR("есть") : STR("НЕТ"), p_mode ? STR("есть") : STR("НЕТ"));
                g_warned = true;
                return false;
            }
            *p_mode = static_cast<uint8_t>(state);
            *p_suppress = true; // соседей Host присылает сам, со своими статусами
            fn.call();
            return true;
        }
    } // namespace

    void on_map_loaded()
    {
        g_loaded_ms = now_ms();
        g_sent.clear();
        g_wanted.clear(); // отрезки пересоздаются при загрузке — Host пришлёт заново
        g_host.clear();
        g_host_seen = false;
    }

    void on_gate(const std::string& tag, const std::string& value)
    {
        if (tag != "legs") return;
        std::map<int, int> wanted;
        size_t pos = 0;
        while (pos < value.size())
        {
            size_t end = value.find(',', pos);
            if (end == value.npos) end = value.size();
            auto item = value.substr(pos, end - pos);
            pos = end + 1;
            auto colon = item.find(':');
            if (colon == item.npos) continue;
            int index = -1, state = 0;
            try { index = std::stoi(item.substr(0, colon)); state = std::stoi(item.substr(colon + 1)); } catch (...) { continue; }
            if (index >= 0 && state > 0 && state <= Explicit) wanted[index] = state;
        }
        if (wanted != g_wanted) Output::send<LogLevel::Verbose>(STR("[Gate] получено от напарника: отрезков {} ({})\n"), wanted.size(), std::wstring(value.begin(), value.end())); // ASCII
        if (!g_is_host)
        {
            g_host = wanted; // у Client пакет — полное состояние Host
            g_host_seen = true;
        }
        g_wanted = std::move(wanted);
    }

    void tick(bool is_host)
    {
        g_is_host = is_host;
        uint64_t now = now_ms();
        if (!Net::connected() || now - g_last_poll_ms < PollMs || now - g_loaded_ms < SettleMs) return;
        g_last_poll_ms = now;
        UObject* route = world_state();
        if (!route) return; // меню или загрузка
        auto legs = legs_of(route);
        if (legs.empty()) return;

        // Оба: поднимаем статус до присланного напарником (ниже не опускаем — игра сама статус не снижает).
        // Место на карте выбирает любой: подтверждение раскрывает отрезок только у выбравшего (натив → CPE_ScanSettings.SetLegScanned → RevealLeg(2, true)).
        for (auto& l : legs)
        {
            auto it = g_wanted.find(l.index);
            if (it == g_wanted.end() || *l.reveal >= it->second) continue;
            int before = *l.reveal, want = it->second;
            g_wanted.erase(it); // одна попытка на пакет: не вышло — напарник повторит через ResendMs
            if (!reveal(l.object, want)) return;
            Output::send<LogLevel::Verbose>(STR("[Gate] отрезок RouteShapeLeg_{}: статус {} → {} (как у напарника), стало {}\n"), l.index, before, want, static_cast<int>(*l.reveal));
        }

        std::string state;
        if (is_host)
            state = encode(legs);
        else
        {
            // Client шлёт только то, что у него раскрыто выше, чем у Host (выбрал место сам); Host применит и разошлёт всем
            if (!g_host_seen) return; // до первого пакета Host не знаем его состояния — не шлём весь свой сейв обратно
            std::vector<Leg> above;
            for (auto& l : legs)
            {
                auto it = g_host.find(l.index);
                if (*l.reveal > (it == g_host.end() ? 0 : it->second)) above.push_back(l);
            }
            state = encode(above);
            if (state.empty()) { g_sent.clear(); return; }
        }
        if (state == g_sent && now - g_sent_ms < ResendMs) return;
        if (state != g_sent)
            Output::send<LogLevel::Verbose>(STR("[Gate] {}: {} → отправляю\n"), is_host ? STR("раскрытые отрезки маршрута") : STR("раскрыто у Client выше, чем у Host"),
                                            state.empty() ? std::wstring(STR("нет")) : std::wstring(state.begin(), state.end())); // ASCII
        Net::send_part("legs", state, Coop::GateState);
        g_sent = state;
        g_sent_ms = now;
    }
} // namespace GateSync
