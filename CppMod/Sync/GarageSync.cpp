#include "GarageSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <chrono>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Host опрашивает двери гаража дважды в секунду и шлёт изменившиеся (при подключении и новой карте — все). Client приводит
// ближайшую к присланной позиции дверь к нужному состоянию: сначала штатные OpenDoor/CloseDoor (с анимацией), если за ~3 с
// не вышло — ForceDoorOpen/ForceDoorClosed. Client помнит последнее слово Host-а (g_target) и после загрузки карты сверяет с ним
// свои двери (Host заново не пришлёт); позже расхождение — действие игрока: запрос Host-у, решает Host.
namespace GarageSync
{
    namespace
    {
        constexpr uint64_t PollMs = 500;
        constexpr uint64_t ApplyWindowMs = 8000; // после загрузки карты расхождение с Host-ом — не действие игрока
        constexpr int ForceAfterPolls = 6;       // после стольких опросов без результата — принудительно
        constexpr int GiveUpAfterPolls = 12;

        struct Vec
        {
            float X, Y, Z;
        };
        using Key = std::tuple<int32_t, int32_t, int32_t>;

        bool g_map_loaded = false, g_send_full = false, g_logged = false;
        uint64_t g_last_poll_ms = 0, g_apply_until_ms = 0;
        std::map<Key, uint8_t> g_sent;     // Host: что Client уже знает
        std::map<Key, uint8_t> g_target;   // Client: последнее слово Host-а (ключ — позиция двери у Host)
        std::map<Key, uint8_t> g_wanted;   // привести дверь к этому: Client — к слову Host-а, Host — к запросу Client-а
        std::map<Key, int> g_attempts;
        std::map<Key, uint8_t> g_seen;     // Client: расхождение прошлого опроса (дребезг анимации)
        std::map<Key, uint8_t> g_reported; // Client: что уже отправлено Host-у
        std::map<Key, float> g_last_position; // положение двери на прошлом опросе: изменилось — дверь едет, не форсируем

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        Key location_of(UObject* door)
        {
            UFunctionCall fn(door, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {INT32_MIN, 0, 0};
            fn.call();
            return {static_cast<int32_t>(std::lround(ret->X)), static_cast<int32_t>(std::lround(ret->Y)), static_cast<int32_t>(std::lround(ret->Z))};
        }

        struct Door
        {
            UObject* actor;
            Key key;
            bool open;
            float position; // CurrentDoorPosition: 0 — закрыта, 1 — открыта; пока меняется, дверь едет (IsOpen переключается только в конце анимации)
            bool moving = false;
            bool heading_open = false; // куда идёт дверь: по движению, а не по IsOpen, который запаздывает на всю анимацию
        };

        std::vector<Door> read_doors()
        {
            std::vector<UObject*> actors;
            UObjectGlobals::FindAllOf(STR("BP_GarageDoor_C"), actors);
            std::vector<Door> doors;
            for (auto* actor : actors)
            {
                auto* open = property<bool>(actor, STR("IsOpen"));
                Key key = location_of(actor);
                if (!open || std::get<0>(key) == INT32_MIN) continue;
                auto* position = property<float>(actor, STR("CurrentDoorPosition"));
                doors.push_back({actor, key, *open, position ? *position : 0.0f});
            }
            return doors;
        }

        int64_t dist2(const Key& a, const Key& b)
        {
            int64_t dx = std::get<0>(a) - std::get<0>(b), dy = std::get<1>(a) - std::get<1>(b), dz = std::get<2>(a) - std::get<2>(b);
            return dx * dx + dy * dy + dz * dz;
        }

        // Ближайшая к позиции дверь не дальше GarageDoorMatchCm или nullptr
        const Door* nearest(const std::vector<Door>& doors, const Key& key)
        {
            const Door* best = nullptr;
            int64_t best_d2 = static_cast<int64_t>(Coop::GarageDoorMatchCm) * Coop::GarageDoorMatchCm;
            for (auto& d : doors)
                if (auto d2 = dist2(d.key, key); d2 <= best_d2)
                {
                    best = &d;
                    best_d2 = d2;
                }
            return best;
        }

        void call_door(UObject* door, const TCHAR* function)
        {
            UFunctionCall fn(door, function);
            if (fn) fn.call();
            else Output::send<LogLevel::Error>(STR("[GarageSync] нет функции {}\n"), function);
        }

        Coop::GarageDoorBody body_of(const Key& key, bool open)
        {
            return {std::get<0>(key), std::get<1>(key), std::get<2>(key), static_cast<uint8_t>(open ? 1 : 0)};
        }
    } // namespace

    void on_garage_door(Coop::PacketId id, const Coop::GarageDoorBody& body, bool is_host)
    {
        Key key{body.X, body.Y, body.Z};
        if (is_host != (id == Coop::GarageDoorRequest)) return; // Host принимает только запросы, Client — только слово Host-а
        g_wanted[key] = body.Open ? 1 : 0;
        g_attempts.erase(key);
        if (!is_host)
        {
            g_target[key] = body.Open ? 1 : 0;
            g_seen.erase(key);
            g_reported.erase(key);
        }
        Output::send<LogLevel::Verbose>(STR("[GarageSync] {}: дверь ({}, {}, {}) → {}\n"), is_host ? STR("Client просит") : STR("получено"), body.X, body.Y, body.Z, body.Open ? STR("открыта") : STR("закрыта"));
    }

    void on_connected(bool is_host)
    {
        if (is_host) g_send_full = true;
    }

    void on_map_loaded()
    {
        g_map_loaded = true;
    }

    void tick(bool is_host)
    {
        uint64_t now = now_ms();
        if (g_map_loaded)
        {
            g_map_loaded = false;
            g_logged = false;
            g_sent.clear(); // новая карта — Client получает все двери заново
            g_send_full = is_host;
            g_apply_until_ms = now + ApplyWindowMs;
            g_seen.clear();
            g_reported.clear();
        }
        if (!Net::connected()) return;
        if (now - g_last_poll_ms < PollMs) return;
        g_last_poll_ms = now;

        auto doors = read_doors();
        if (doors.empty()) return;
        for (auto& d : doors)
        {
            auto last = g_last_position.find(d.key);
            d.moving = last != g_last_position.end() && std::fabs(last->second - d.position) > 1e-4f;
            d.heading_open = d.moving ? d.position > last->second : d.open;
            g_last_position[d.key] = d.position;
        }
        if (!g_logged)
        {
            g_logged = true;
            Output::send<LogLevel::Verbose>(STR("[GarageSync] дверей гаража на карте: {}\n"), doors.size());
            for (auto& d : doors)
                Output::send<LogLevel::Verbose>(STR("[GarageSync]   {} ({}, {}, {}) открыта={} положение={}\n"), d.actor->GetName(), std::get<0>(d.key), std::get<1>(d.key), std::get<2>(d.key), d.open, d.position);
        }

        for (auto it = g_wanted.begin(); it != g_wanted.end();)
        {
            auto* door = nearest(doors, it->first);
            bool want_open = it->second != 0;
            if (!door || door->open == want_open) // такой двери на карте нет или уже совпало
            {
                if (is_host && door) g_sent.erase(door->key); // Client ждёт ответа: итог рассылаем даже если у Host ничего не поменялось
                g_attempts.erase(it->first);
                it = g_wanted.erase(it);
                continue;
            }
            if (door->moving && g_attempts.contains(it->first)) // команда принята, дверь едет — ждём конца анимации
            {
                ++it;
                continue;
            }
            int attempt = ++g_attempts[it->first];
            if (attempt > GiveUpAfterPolls)
            {
                Output::send<LogLevel::Warning>(STR("[GarageSync] дверь ({}, {}, {}) не приняла команду за {} опросов — оставляю как есть\n"), std::get<0>(it->first), std::get<1>(it->first), std::get<2>(it->first), GiveUpAfterPolls);
                // Принимаем реальность: Host заново сообщит своё состояние, Client не будет «исправлять» Host-а запросом
                if (is_host) g_sent.erase(door->key);
                else g_target[it->first] = door->open ? 1 : 0;
                g_attempts.erase(it->first);
                it = g_wanted.erase(it);
                continue;
            }
            if (attempt == 1) call_door(door->actor, want_open ? STR("OpenDoor") : STR("CloseDoor"));
            else if (attempt == ForceAfterPolls) call_door(door->actor, want_open ? STR("ForceDoorOpen") : STR("ForceDoorClosed"));
            ++it;
        }

        if (is_host)
        {
            if (g_send_full) g_sent.clear();
            g_send_full = false;
            for (auto& d : doors)
            {
                auto it = g_sent.find(d.key);
                if (it != g_sent.end() && it->second == (d.heading_open ? 1 : 0)) continue;
                g_sent[d.key] = d.heading_open ? 1 : 0;
                Net::send_garage_door(Coop::GarageDoor, body_of(d.key, d.heading_open));
                Output::send<LogLevel::Verbose>(STR("[GarageSync] отправлено: дверь ({}, {}, {}) → {}\n"), std::get<0>(d.key), std::get<1>(d.key), std::get<2>(d.key), d.heading_open ? STR("открыта") : STR("закрыта"));
            }
            return;
        }

        // Client: сверка своих дверей со словом Host-а
        for (auto& d : doors)
        {
            const Key* tkey = nullptr;
            int64_t best_d2 = static_cast<int64_t>(Coop::GarageDoorMatchCm) * Coop::GarageDoorMatchCm;
            for (auto& [key, open] : g_target)
                if (auto d2 = dist2(d.key, key); d2 <= best_d2)
                {
                    tkey = &key;
                    best_d2 = d2;
                }
            if (!tkey || g_wanted.contains(*tkey)) continue;
            bool target_open = g_target[*tkey] != 0;
            if (d.heading_open == target_open)
            {
                g_seen.erase(*tkey);
                g_reported.erase(*tkey);
                continue;
            }
            if (now < g_apply_until_ms) // карта только загрузилась: подтягиваем к Host-у, а не жалуемся ему
            {
                g_wanted[*tkey] = target_open ? 1 : 0;
                g_attempts.erase(*tkey);
                continue;
            }
            auto seen = g_seen.find(*tkey);
            if (seen == g_seen.end() || seen->second != (d.heading_open ? 1 : 0))
            {
                g_seen[*tkey] = d.heading_open ? 1 : 0;
                continue;
            }
            // Передней дверью (выезд) управляет игра: BP_Director_GSS_GarageFrontDoor каждый тик ставит GSS_Garage_FrontDoorOpened
            // по выбранному маршруту, а он есть только у Host — закрытие у Client не действие игрока (docs/recon/reference/route-selection.md)
            if (d.actor->GetName().starts_with(STR("BP_GarageDoorFront"))) continue;
            auto reported = g_reported.find(*tkey);
            if (reported != g_reported.end() && reported->second == (d.heading_open ? 1 : 0)) continue;
            g_reported[*tkey] = d.heading_open ? 1 : 0;
            Net::send_garage_door(Coop::GarageDoorRequest, body_of(*tkey, d.heading_open));
            Output::send<LogLevel::Verbose>(STR("[GarageSync] дверь ({}, {}, {}) изменена игроком → прошу Host: {}\n"), std::get<0>(*tkey), std::get<1>(*tkey), std::get<2>(*tkey), d.heading_open ? STR("открыта") : STR("закрыта"));
        }
    }
} // namespace GarageSync
