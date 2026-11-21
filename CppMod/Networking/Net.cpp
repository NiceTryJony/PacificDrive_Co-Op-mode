#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <enet/enet.h>

#include "Net.h"
#include "Protocol.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <thread>

// Сеть живёт в своём потоке (ENet трогает только он): загрузка уровня блокирует игровой поток, но не связь.
// Игровой поток кладёт пакеты в очереди по классам приоритета и забирает принятые в tick(); UObject сетевой поток не трогает.
namespace Net
{
    namespace
    {
        constexpr uint32_t BulkInFlightBytes = 128 * 1024; // сколько файла одновременно «в воздухе»: остальное ждёт, события идут вперёд
        constexpr uint32_t MaxFileBytes = 64 * 1024 * 1024; // сейв зоны ~10 МБ; больше — это не сейв
        constexpr uint64_t StatsIntervalMs = 10'000;
        // Игровой поток связь больше не держит. Закрытие/вылет игры напарника иначе не видно: при 15/45 с HUD писал «left» через ~25 с
        constexpr uint32_t TimeoutMinMs = 5'000, TimeoutMaxMs = 10'000;

        Config g_config;
        ENetHost* g_host = nullptr;
        ENetPeer* g_peer = nullptr; // только сетевой поток; у Host и Client ровно один собеседник
        std::thread g_thread;
        std::atomic<bool> g_running{false}, g_connected{false}, g_version_mismatch{false};
        std::atomic<int> g_rtt{-1};
        std::atomic<int> g_attempts{0}; // попытки подключения Client с последнего успеха (карточка входа в HUD)
        std::atomic<double> g_loss{0};
        std::atomic<uint32_t> g_sequence{0};
        uint32_t g_tx_id = 0; // id последней отправки файла (игровой поток)
        uint32_t g_pings_received = 0; // сетевой поток

        // Статистика по классам: сколько ушло, сколько заменено более новым пакетом с тем же ключом
        struct ClassStats
        {
            std::atomic<uint64_t> packets{0}, bytes{0}, merged{0};
        } g_stats[Coop::PrioCount];

        // Приём файла по кускам: у каждого вида файла своё состояние, чтобы зона и сейв пассажира могли идти одновременно
        struct Rx
        {
            std::vector<uint8_t> data;
            uint32_t id = 0, received = 0, logged_quarter = 0;
            uint64_t started_ms = 0;
        };
        Rx g_rx_zone, g_rx_passenger;

        // Принятое сетевым потоком → игровому
        enum class InKind { Packet, Connected, Disconnected, Log };
        struct In
        {
            InKind kind;
            std::vector<uint8_t> data;
            std::wstring text;
        };
        std::map<uint8_t, Handler> g_handlers; // игровой поток
        std::mutex g_in_mutex;
        std::vector<In> g_in;

        // Исходящее: очередь на класс. key != 0 — состояние: новый пакет с тем же ключом заменяет ещё не ушедший старый
        struct Out
        {
            uint64_t key = 0;
            std::vector<uint8_t> data;
        };
        std::mutex g_out_mutex;
        std::deque<Out> g_out[Coop::PrioCount];

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        void push_in(In&& in)
        {
            std::lock_guard lock(g_in_mutex);
            g_in.push_back(std::move(in));
        }

        void log_async(std::wstring text) { push_in({InKind::Log, {}, std::move(text)}); }

        uint64_t hash_tag(const std::string& s)
        {
            uint64_t h = 1469598103934665603ull;
            for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
            return h;
        }

        // Состояния «последнее значение побеждает» (полное состояние в каждом пакете)
        uint64_t default_key(Coop::PacketId id)
        {
            switch (id)
            {
            case Coop::CarState: case Coop::HostPose: case Coop::PassengerPose: case Coop::DriveInput: case Coop::CarSystems: case Coop::QuirkSet: case Coop::RadioTrack: case Coop::ZoneCensus:
                return id;
            default:
                return 0;
            }
        }

        // Состояние Host → Client, которое при (пере)подключении нужно отдать заново целиком; хранится последний пакет на (вид, ключ).
        // Погода не входит: событие "wev" устаревает, её модуль досылает сам
        std::map<std::pair<uint8_t, uint64_t>, std::vector<uint8_t>> g_state; // игровой поток

        bool replayable(Coop::PacketId id)
        {
            return id == Coop::CarSystems || id == Coop::QuirkSet || id == Coop::RadioTrack || id == Coop::CarPart;
        }

        void enqueue(Coop::PacketId id, std::vector<uint8_t>&& data, uint64_t key = 0)
        {
            if (!key) key = default_key(id);
            if (g_config.role == Role::Host && replayable(id)) g_state[{id, key}] = data; // и пока собеседника нет: он получит при подключении
            if (!g_connected) return;
            auto cls = Coop::priority_of(id);
            std::lock_guard lock(g_out_mutex);
            auto& queue = g_out[cls];
            if (key)
                for (auto& item : queue)
                    if (item.key == key)
                    {
                        item.data = std::move(data);
                        g_stats[cls].merged++;
                        return;
                    }
            queue.push_back({key, std::move(data)});
        }

        template <typename Body>
        void send_body(Coop::PacketId id, const Body& body)
        {
            enqueue(id, Coop::make_packet(id, ++g_sequence, body));
        }

        void clear_out()
        {
            std::lock_guard lock(g_out_mutex);
            for (auto& q : g_out) q.clear();
        }

        // true — файл принят целиком и CRC сошёлся (байты в rx.data, забирает вызывающий)
        bool receive_chunk(Rx& rx, const uint8_t* data, size_t size, const Events& events, const wchar_t* what)
        {
            auto* chunk = Coop::body_of<Coop::ZoneChunkBody>(data, size);
            if (!chunk || !chunk->Total) return false;
            if (chunk->Total > MaxFileBytes) // размер из пакета: мусор не должен выделять гигабайты на игровом потоке
            {
                events.log(std::format(L"[Net] {}: размер {} байт больше допустимого — отброшен", what, chunk->Total));
                rx.id = 0;
                return false;
            }
            const uint8_t* bytes = data + sizeof(Coop::Header) + sizeof(Coop::ZoneChunkBody);
            size_t length = size - sizeof(Coop::Header) - sizeof(Coop::ZoneChunkBody);
            if (chunk->TransferId != rx.id || chunk->Offset == 0)
            {
                rx.id = chunk->TransferId;
                rx.data.assign(chunk->Total, 0);
                rx.received = 0;
                rx.logged_quarter = 0;
                rx.started_ms = now_ms();
            }
            // Канал reliable и упорядоченный: куски приходят по порядку, дыр быть не может
            if (chunk->Offset != rx.received || rx.received + length > rx.data.size())
            {
                events.log(std::format(L"[Net] {}: кусок не по порядку (смещение {}, ждали {}) — приём сброшен", what, chunk->Offset, rx.received));
                rx.id = 0;
                return false;
            }
            std::memcpy(rx.data.data() + rx.received, bytes, length);
            rx.received += static_cast<uint32_t>(length);
            if (uint32_t quarter = static_cast<uint32_t>(rx.received * 4 / rx.data.size()); quarter > rx.logged_quarter && rx.received < rx.data.size())
            {
                rx.logged_quarter = quarter;
                events.log(std::format(L"[Net] {}: приём {}%", what, quarter * 25));
            }
            if (rx.received < rx.data.size()) return false;
            rx.id = 0;
            if (Coop::crc32(rx.data.data(), rx.data.size()) != chunk->Crc32)
            {
                events.log(std::format(L"[Net] {} ПОВРЕЖДЁН: CRC не совпал — отброшен", what));
                return false;
            }
            events.log(std::format(L"[Net] получен {}: {} байт за {} мс, CRC {:08X}", what, rx.data.size(), now_ms() - rx.started_ms, chunk->Crc32));
            return true;
        }

        void send_file(Coop::PacketId id, const std::vector<uint8_t>& file, const Events& events, const wchar_t* what)
        {
            if (!g_connected)
            {
                events.log(std::format(L"[Net] {} не отправлен: нет собеседника", what));
                return;
            }
            if (file.empty()) return;
            Coop::ZoneChunkBody body{++g_tx_id, 0, static_cast<uint32_t>(file.size()), Coop::crc32(file.data(), file.size())};
            // Куски в очередь класса Bulk: сетевой поток отдаёт их ENet по мере освобождения окна, события идут вперёд
            for (body.Offset = 0; body.Offset < body.Total; body.Offset += Coop::ChunkSize)
            {
                uint32_t length = (std::min)(Coop::ChunkSize, body.Total - body.Offset);
                enqueue(id, Coop::make_packet(id, ++g_sequence, body, file.data() + body.Offset, length));
            }
            events.log(std::format(L"[Net] {} поставлен в отправку: {} байт ({} кусков), CRC {:08X}", what, body.Total, (body.Total + Coop::ChunkSize - 1) / Coop::ChunkSize, body.Crc32));
        }

        void on_receive(const uint8_t* data, size_t size, const Events& events)
        {
            if (size < sizeof(Coop::Header)) return;
            switch (data[0])
            {
            case Coop::ZoneSave:
                if (g_config.role == Role::Client && receive_chunk(g_rx_zone, data, size, events, L"сейв зоны")) events.on_zone_save(std::move(g_rx_zone.data));
                break;
            case Coop::PassengerSave:
                if (events.on_passenger_save && receive_chunk(g_rx_passenger, data, size, events, L"сейв пассажира")) events.on_passenger_save(std::move(g_rx_passenger.data));
                break;
            case Coop::PassengerSaveRequest:
                if (auto* request = Coop::body_of<Coop::SaveRequestBody>(data, size); request && events.on_passenger_request) events.on_passenger_request(*request);
                break;
            case Coop::CarState:
                if (auto* car = Coop::body_of<Coop::CarStateWire>(data, size); car && events.on_car_state) events.on_car_state(Coop::from_wire(*car));
                break;
            case Coop::RequestSeat:
            case Coop::RequestExitVehicle:
            case Coop::PassengerAttach:
            case Coop::PassengerDetach:
                if (auto* seat = Coop::body_of<Coop::SeatBody>(data, size); seat && events.on_seat) events.on_seat(static_cast<Coop::PacketId>(data[0]), *seat);
                break;
            case Coop::DoorState:
            case Coop::DoorRequest:
                if (auto* door = Coop::body_of<Coop::DoorBody>(data, size); door && events.on_door) events.on_door(static_cast<Coop::PacketId>(data[0]), *door);
                break;
            case Coop::CarSystems:
                if (auto* systems = Coop::body_of<Coop::SystemsBody>(data, size); systems && events.on_systems) events.on_systems(*systems);
                break;
            case Coop::CarPart:
                if (std::string tag, path; events.on_part && Coop::parse_part(data, size, tag, path)) events.on_part(tag, path);
                break;
            case Coop::WeatherEvent:
                if (std::string tag, path; events.on_weather && Coop::parse_part(data, size, tag, path)) events.on_weather(tag, path);
                break;
            case Coop::HandItem:
                if (std::string tag, path; events.on_hand && Coop::parse_part(data, size, tag, path)) events.on_hand(tag, path);
                break;
            case Coop::GateState:
                if (std::string tag, path; events.on_gate && Coop::parse_part(data, size, tag, path)) events.on_gate(tag, path);
                break;
            case Coop::ZoneCensus:
                if (std::string tag, path; events.on_census && Coop::parse_part(data, size, tag, path)) events.on_census(tag, path);
                break;
            case Coop::HazardSpawn:
                if (std::string tag, path; events.on_hazard && Coop::parse_part(data, size, tag, path)) events.on_hazard(tag, path);
                break;
            case Coop::GarageDoor:
            case Coop::GarageDoorRequest:
                if (auto* door = Coop::body_of<Coop::GarageDoorBody>(data, size); door && events.on_garage_door) events.on_garage_door(static_cast<Coop::PacketId>(data[0]), *door);
                break;
            case Coop::CarPartRequest:
                if (std::string tag, path; events.on_part_request && Coop::parse_part(data, size, tag, path)) events.on_part_request(tag, path);
                break;
            case Coop::TrunkItem:
            case Coop::TrunkDenied:
            case Coop::TrunkTake:
            case Coop::TrunkPut:
                if (uint16_t count; events.on_trunk)
                {
                    std::string path;
                    std::vector<Coop::TrunkValue> values;
                    if (Coop::parse_trunk(data, size, count, path, values)) events.on_trunk(static_cast<Coop::PacketId>(data[0]), count, path, values);
                }
                break;
            case Coop::LootPicked:
            case Coop::LootDenied:
            case Coop::InventoryAction:
                if (auto* loot = Coop::body_of<Coop::LootBody>(data, size); loot && events.on_loot) events.on_loot(static_cast<Coop::PacketId>(data[0]), *loot);
                break;
            case Coop::QuirkSet:
                if (auto* b = Coop::body_of<Coop::QuirkSetBody>(data, size); b && events.on_quirk_set) events.on_quirk_set(*b);
                break;
            case Coop::QuirkCure:
                if (auto* b = Coop::body_of<Coop::QuirkBody>(data, size); b && events.on_quirk_cure) events.on_quirk_cure(*b);
                break;
            case Coop::QuirkKnow:
                if (auto* b = Coop::body_of<Coop::QuirkKnowBody>(data, size); b && events.on_quirk_know) events.on_quirk_know(*b);
                break;
            case Coop::WorldItem:
                if (Coop::WorldItemBody body; events.on_world_item)
                {
                    std::string path;
                    std::vector<Coop::TrunkValue> values;
                    if (Coop::parse_world_item(data, size, body, path, values)) events.on_world_item(body, path, values);
                }
                break;
            case Coop::RadioTrack:
                if (std::string kind, name; events.on_radio && Coop::parse_part(data, size, kind, name)) events.on_radio(kind, name);
                break;
            case Coop::AnchorTaken:
            case Coop::PortalActive:
            case Coop::GeneratorBroken:
            case Coop::Mark:
                if (auto* b = Coop::body_of<Coop::AnchorBody>(data, size); b && events.on_anchor) events.on_anchor(static_cast<Coop::PacketId>(data[0]), *b);
                break;
            case Coop::HostPose:
            case Coop::PassengerPose:
                if (auto* pose = Coop::body_of<Coop::PoseBody>(data, size); pose && events.on_pose) events.on_pose(*pose);
                break;
            default:
                if (auto it = g_handlers.find(data[0]); it != g_handlers.end()) it->second(data + sizeof(Coop::Header), size - sizeof(Coop::Header));
                else events.log(std::format(L"[Net] неизвестный пакет {}", data[0]));
            }
        }


        // ---- сетевой поток ----

        void connect_to_host()
        {
            ENetAddress address{};
            enet_address_set_host(&address, g_config.host_ip.c_str());
            address.port = g_config.port;
            g_peer = enet_host_connect(g_host, &address, Coop::NumChannels, Coop::ProtocolVersion);
            ++g_attempts;
            log_async(std::format(L"[Net] подключение к {}:{}...", std::wstring(g_config.host_ip.begin(), g_config.host_ip.end()), g_config.port));
        }

        void raw_send(uint8_t cls, const std::vector<uint8_t>& data)
        {
            bool reliable = cls != Coop::PrioRealtime;
            // sim_loss в coop.ini: проверка потерь без плохой сети (только для unreliable)
            if (!reliable && g_config.sim_loss_percent > 0 && std::rand() % 10000 < static_cast<int>(g_config.sim_loss_percent * 100)) return;
            enet_peer_send(g_peer, cls, enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0));
            g_stats[cls].packets++;
            g_stats[cls].bytes += data.size();
        }

        // Строго по приоритету; Bulk — только пока в воздухе немного и ENet ничего не ждёт отправки
        void flush_out()
        {
            if (!g_peer || !g_connected) return;
            for (uint8_t cls = 0; cls < Coop::PrioBulk; ++cls)
            {
                std::deque<Out> batch;
                {
                    std::lock_guard lock(g_out_mutex);
                    batch.swap(g_out[cls]);
                }
                for (auto& item : batch) raw_send(cls, item.data);
            }
            for (int burst = 0; burst < 4; ++burst)
            {
                if (g_peer->reliableDataInTransit >= BulkInFlightBytes || !enet_list_empty(&g_peer->outgoingCommands)) break;
                Out item;
                {
                    std::lock_guard lock(g_out_mutex);
                    auto& queue = g_out[Coop::PrioBulk];
                    if (queue.empty()) break;
                    item = std::move(queue.front());
                    queue.pop_front();
                }
                raw_send(Coop::PrioBulk, item.data);
            }
        }

        void on_net_event(const ENetEvent& e)
        {
            switch (e.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                if (g_config.role == Role::Host && e.data != Coop::ProtocolVersion)
                {
                    log_async(std::format(L"[Net] собеседник с другой версией протокола ({} вместо {}) — отклонён", e.data, Coop::ProtocolVersion));
                    enet_peer_disconnect(e.peer, 1);
                    break;
                }
                // shortcut: новое подключение всегда вытесняет старое (кто угодно в LAN может выбить напарника), когда понадобится защита — проверять IP
                if (g_peer && g_peer != e.peer) enet_peer_disconnect_now(g_peer, 0);
                g_peer = e.peer;
                clear_out(); // до connected: модули досылают полное состояние в on_connected, старое устарело
                enet_peer_timeout(g_peer, 0, TimeoutMinMs, TimeoutMaxMs);
                g_connected = true;
                g_attempts = 0;
                push_in({InKind::Connected, {}, L"[Net] соединение установлено"});
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (e.peer != g_peer) break; // вытесненное старое соединение
                g_peer = nullptr;
                g_connected = false;
                g_rtt = -1;
                clear_out();
                if (e.data == 1)
                {
                    g_version_mismatch = true;
                    push_in({InKind::Disconnected, {}, L"[Net] Host отклонил соединение: версии мода не совпадают — обновите мод на обоих ПК"});
                }
                else
                    push_in({InKind::Disconnected, {}, L"[Net] соединение разорвано"});
                break;
            case ENET_EVENT_TYPE_RECEIVE:
            {
                const uint8_t* data = e.packet->data;
                size_t size = e.packet->dataLength;
                if (size >= sizeof(Coop::Header))
                {
                    if (data[0] == Coop::Ping)
                    {
                        if (g_config.role == Role::Host)
                            enet_peer_send(e.peer, e.channelID, enet_packet_create(data, size, e.channelID == Coop::PrioRealtime ? 0 : ENET_PACKET_FLAG_RELIABLE)); // эхо
                        else if (auto* ping = Coop::body_of<Coop::PingBody>(data, size); ping && ++g_pings_received % 10 == 0)
                            log_async(std::format(L"[Net] эхо: rtt={} мс (канал {})", now_ms() - ping->ClientTimeMs, e.channelID));
                    }
                    else
                        push_in({InKind::Packet, std::vector<uint8_t>(data, data + size), {}});
                }
                enet_packet_destroy(e.packet);
                break;
            }
            default:
                break;
            }
        }

        void send_ping()
        {
            Coop::PingBody ping{now_ms()};
            auto reliable = Coop::make_packet(Coop::Ping, ++g_sequence, ping);
            auto unreliable = Coop::make_packet(Coop::Ping, ++g_sequence, ping);
            enet_peer_send(g_peer, Coop::PrioCritical, enet_packet_create(reliable.data(), reliable.size(), ENET_PACKET_FLAG_RELIABLE));
            enet_peer_send(g_peer, Coop::PrioRealtime, enet_packet_create(unreliable.data(), unreliable.size(), 0));
        }

        void thread_main()
        {
            uint64_t last_ping = 0, last_stats = 0, last_connect = now_ms();
            while (g_running)
            {
                flush_out();
                ENetEvent e;
                int r = enet_host_service(g_host, &e, 1); // ждёт сокет до 1 мс: исходящее уходит с задержкой не больше
                while (r > 0)
                {
                    on_net_event(e);
                    r = enet_host_service(g_host, &e, 0);
                }
                uint64_t now = now_ms();
                if (g_connected && g_peer)
                {
                    g_rtt = static_cast<int>(g_peer->roundTripTime);
                    g_loss = g_peer->packetLoss * 100.0 / static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE);
                    if (now - last_stats >= StatsIntervalMs)
                    {
                        last_stats = now;
                        std::wstring line = std::format(L"[Net] связь: rtt={} мс, потери={:.1f}%; за {} с по классам пакетов/байт/заменено:", g_rtt.load(), g_loss.load(), StatsIntervalMs / 1000);
                        static const wchar_t* names[Coop::PrioCount] = {L"крит", L"реалт", L"событ", L"фон", L"bulk"};
                        for (int c = 0; c < Coop::PrioCount; ++c)
                            line += std::format(L" {}={}/{}/{}", names[c], g_stats[c].packets.exchange(0), g_stats[c].bytes.exchange(0), g_stats[c].merged.exchange(0));
                        log_async(std::move(line));
                    }
                }
                if (g_config.role == Role::Client)
                {
                    if (!g_connected && !g_version_mismatch && now - last_connect > 5000)
                    {
                        last_connect = now;
                        if (g_peer) enet_peer_reset(g_peer);
                        connect_to_host();
                    }
                    if (g_connected && g_peer && now - last_ping >= 1000)
                    {
                        last_ping = now;
                        send_ping();
                    }
                }
            }
        }
    } // namespace

    Config load_config(const std::filesystem::path& path)
    {
        Config config;
        config.port = Coop::DefaultPort;
        FILE* f = _wfopen(path.c_str(), L"r");
        if (!f) return config;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            std::string s(line);
            auto eq = s.find('=');
            if (eq == std::string::npos || s[0] == '#' || s[0] == ';') continue;
            auto trim = [](std::string v) {
                v.erase(0, v.find_first_not_of(" \t"));
                v.erase(v.find_last_not_of(" \t\r\n") + 1);
                return v;
            };
            std::string key = trim(s.substr(0, eq)), value = trim(s.substr(eq + 1));
            if (key == "role") config.role = value == "host" ? Role::Host : value == "client" ? Role::Client : Role::Off;
            else if (key == "host_ip") config.host_ip = value;
            else if (key == "port") config.port = static_cast<uint16_t>(std::atoi(value.c_str()));
            else if (key == "seat_x") config.seat_x = static_cast<float>(std::atof(value.c_str()));
            else if (key == "seat_y") config.seat_y = static_cast<float>(std::atof(value.c_str()));
            else if (key == "seat_z") config.seat_z = static_cast<float>(std::atof(value.c_str()));
            else if (key == "seat_right_x") config.seat_right_x = static_cast<float>(std::atof(value.c_str()));
            else if (key == "seat_right_y") config.seat_right_y = static_cast<float>(std::atof(value.c_str()));
            else if (key == "seat_right_z") config.seat_right_z = static_cast<float>(std::atof(value.c_str()));
            else if (key == "restrict_interactions") config.restrict_interactions = value != "0";
            else if (key == "camera_follow") config.camera_follow = value != "0";
            else if (key == "exit_side_cm") config.exit_side_cm = static_cast<float>(std::atof(value.c_str()));
            else if (key == "status_overlay") config.status_overlay = value != "0";
            else if (key == "client_parts") config.client_parts = value != "0";
            else if (key == "heap_test") config.heap_test = value != "0";
            else if (key == "client_drive") config.client_drive = value != "0";
            else if (key == "hand_fx") config.hand_fx = value != "0";
            else if (key == "hand_scale") config.hand_scale = std::clamp(static_cast<float>(std::atof(value.c_str())), 0.1f, 2.0f);
            else if (key == "drive_probe_throttle") config.drive_probe_throttle = static_cast<float>(std::atof(value.c_str()));
            else if (key == "drive_probe_steer") config.drive_probe_steer = static_cast<float>(std::atof(value.c_str()));
            else if (key == "sim_loss") config.sim_loss_percent = static_cast<float>(std::atof(value.c_str()));
        }
        fclose(f);
        // Мусор в coop.ini (nan, значение вне разумного диапазона, порт 0) не должен ломать игру: берём значение по умолчанию.
        // Файл правят руками и лаунчер, а читается он и при живой перезагрузке настроек
        const Config def;
        auto fix = [](float& v, float fallback, float lo, float hi) {
            if (!std::isfinite(v) || v < lo || v > hi) v = fallback;
        };
        fix(config.seat_x, def.seat_x, -1000, 1000), fix(config.seat_y, def.seat_y, -1000, 1000), fix(config.seat_z, def.seat_z, -1000, 1000);
        fix(config.seat_right_x, def.seat_right_x, -1000, 1000), fix(config.seat_right_y, def.seat_right_y, -1000, 1000), fix(config.seat_right_z, def.seat_right_z, -1000, 1000);
        fix(config.exit_side_cm, def.exit_side_cm, 0, 2000);
        fix(config.sim_loss_percent, def.sim_loss_percent, 0, 100);
        fix(config.drive_probe_throttle, def.drive_probe_throttle, -1, 1), fix(config.drive_probe_steer, def.drive_probe_steer, -1, 1);
        if (config.port == 0) config.port = Coop::DefaultPort;
        if (config.host_ip.empty()) config.host_ip = def.host_ip;
        return config;
    }

    bool start(const Config& config, const Events& events)
    {
        g_config = config;
        if (config.role == Role::Off) return false;
        if (enet_initialize() != 0)
        {
            events.log(L"[Net] enet_initialize: ОШИБКА");
            return false;
        }
        if (config.role == Role::Host)
        {
            ENetAddress address{ENET_HOST_ANY, config.port};
            // 2 места: пока старое соединение «висит» до таймаута, новое должно вытеснить его, а не ждать 1–2 минуты
            g_host = enet_host_create(&address, 2, Coop::NumChannels, 0, 0);
            events.log(g_host ? std::format(L"[Net] Host: слушаю UDP {}", config.port) : std::format(L"[Net] Host: порт {} занят/недоступен", config.port));
        }
        else
        {
            g_host = enet_host_create(nullptr, 1, Coop::NumChannels, 0, 0);
            if (g_host) connect_to_host();
        }
        if (!g_host) return false;
        // Сжатие датаграмм range coder-ом ENet (оба конца обязаны включить — защищает ProtocolVersion). Сейвы в куски уже LZ4 (GAME), их ENet
        // отправит как есть, если сжатие не помогло; выигрыш — на мелких пакетах с путями и текстом
        enet_host_compress_with_range_coder(g_host);
        g_running = true;
        g_thread = std::thread(thread_main);
        return true;
    }

    void stop()
    {
        g_running = false;
        if (g_thread.joinable()) g_thread.join();
        if (g_peer && g_connected) enet_peer_disconnect_now(g_peer, 0);
        if (g_host) enet_host_destroy(g_host);
        if (g_config.role != Role::Off) enet_deinitialize();
        g_host = nullptr;
        g_peer = nullptr;
        g_connected = false;
    }

    // Игровой поток: принятое сетевым потоком разбирается здесь, обработчики модулей работают с UObject как раньше
    void tick(const Events& events)
    {
        if (!g_running) return;
        std::vector<In> batch;
        {
            std::lock_guard lock(g_in_mutex);
            batch.swap(g_in);
        }
        // Загрузка карты блокирует игровой поток на ~10 с: в очереди копятся сотни устаревших состояний (поза, машина). Как и в исходящей
        // очереди, из состояний с ключом оставляем самое новое каждого вида — иначе после загрузки пик и откат объектов на старые значения
        std::bitset<256> seen;
        for (size_t i = batch.size(); i-- > 0;)
        {
            auto& in = batch[i];
            if (in.kind != InKind::Packet || in.data.empty() || !default_key(static_cast<Coop::PacketId>(in.data[0]))) continue;
            if (seen.test(in.data[0])) in.data.clear();
            else seen.set(in.data[0]);
        }
        for (auto& in : batch)
        {
            switch (in.kind)
            {
            case InKind::Packet:
                if (in.data.empty()) break; // вытеснен более новым состоянием
                on_receive(in.data.data(), in.data.size(), events);
                break;
            case InKind::Connected:
                events.log(in.text);
                events.on_connected();
                // Модули в on_connected досылают своё; общее хранилище страхует тех, у кого этого нет. Одинаковый ключ — в очереди склеится
                for (auto& [k, packet] : g_state) enqueue(static_cast<Coop::PacketId>(k.first), std::vector<uint8_t>(packet), k.second);
                break;
            default:
                events.log(in.text);
            }
        }
    }

    void on(Coop::PacketId id, Handler handler) { g_handlers[id] = std::move(handler); }

    void send(Coop::PacketId id, const std::vector<uint8_t>& payload, uint64_t key)
    {
        std::vector<uint8_t> out(sizeof(Coop::Header) + payload.size());
        Coop::Header h{id, ++g_sequence};
        std::memcpy(out.data(), &h, sizeof(h));
        if (!payload.empty()) std::memcpy(out.data() + sizeof(h), payload.data(), payload.size());
        enqueue(id, std::move(out), key);
    }

    // Связь хуже нормы — частые пакеты (поза, машина) идут реже: очередь и потери не растут. Нормальная связь — базовая частота
    uint32_t adaptive_interval_ms(uint32_t base_ms)
    {
        if (!g_connected) return base_ms;
        int rtt = g_rtt;
        double loss = g_loss;
        if (rtt > 250 || loss > 8) return base_ms * 2;
        if (rtt > 120 || loss > 3) return base_ms * 3 / 2;
        return base_ms;
    }

    bool connected() { return g_connected; }
    bool version_mismatch() { return g_version_mismatch; }
    int connect_attempts() { return g_attempts; }

    int zone_progress()
    {
        bool receiving = g_rx_zone.id && !g_rx_zone.data.empty() && g_rx_zone.received < g_rx_zone.data.size();
        return receiving ? static_cast<int>(g_rx_zone.received * 100ull / g_rx_zone.data.size()) : -1;
    }

    int rtt_ms() { return g_connected ? g_rtt.load() : -1; }

    double loss_percent() { return g_connected ? g_loss.load() : 0.0; }

    void send_loot(Coop::PacketId id, const Coop::LootBody& body) { send_body(id, body); }
    void send_door(Coop::PacketId id, const Coop::DoorBody& body) { send_body(id, body); }
    void send_garage_door(Coop::PacketId id, const Coop::GarageDoorBody& body) { send_body(id, body); }
    void send_quirk_set(const Coop::QuirkSetBody& body) { send_body(Coop::QuirkSet, body); }
    void send_quirk_cure(const Coop::QuirkBody& body) { send_body(Coop::QuirkCure, body); }
    void send_quirk_know(const Coop::QuirkKnowBody& body) { send_body(Coop::QuirkKnow, body); }
    void send_anchor(const Coop::AnchorBody& body, Coop::PacketId id) { send_body(id, body); }
    void send_systems(const Coop::SystemsBody& body) { send_body(Coop::CarSystems, body); }
    void send_seat(Coop::PacketId id, const Coop::SeatBody& body) { send_body(id, body); }
    void send_car_state(const Coop::CarStateBody& state) { send_body(Coop::CarState, Coop::to_wire(state)); }
    void send_pose(const Coop::PoseBody& pose) { send_body(g_config.role == Role::Host ? Coop::HostPose : Coop::PassengerPose, pose); }
    void send_passenger_request(const Coop::SaveRequestBody& body) { send_body(Coop::PassengerSaveRequest, body); }

    bool send_world_item(const Coop::WorldItemBody& body, const std::string& path, const std::vector<Coop::TrunkValue>& values)
    {
        auto packet = Coop::make_world_item_packet(g_sequence + 1, body, path, values);
        if (packet.empty()) return false;
        ++g_sequence;
        enqueue(Coop::WorldItem, std::move(packet));
        return true;
    }

    bool send_part(const std::string& tag, const std::string& path, Coop::PacketId id)
    {
        auto packet = Coop::make_part_packet(g_sequence + 1, tag, path, id);
        if (packet.empty()) return false;
        ++g_sequence;
        // Состояние слота/уровня погоды: пока старое не ушло, достаточно нового (ключ — вид пакета и тег); аномалии нужны все
        uint64_t key = default_key(id) || id == Coop::HazardSpawn ? 0 : (static_cast<uint64_t>(id) << 56) ^ hash_tag(tag);
        enqueue(id, std::move(packet), key);
        return true;
    }

    bool send_trunk(Coop::PacketId id, uint16_t count, const std::string& path, const std::vector<Coop::TrunkValue>& values)
    {
        auto packet = Coop::make_trunk_packet(id, g_sequence + 1, count, path, values);
        if (packet.empty()) return false;
        ++g_sequence;
        enqueue(id, std::move(packet));
        return true;
    }

    void send_zone_save(const std::vector<uint8_t>& file, const Events& events) { send_file(Coop::ZoneSave, file, events, L"сейв зоны"); }
    void send_passenger_save(const std::vector<uint8_t>& file, const Events& events) { send_file(Coop::PassengerSave, file, events, L"сейв пассажира"); }
} // namespace Net
