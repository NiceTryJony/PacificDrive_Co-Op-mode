#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <enet/enet.h>

#include "Net.h"
#include "Protocol.h"

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <format>

namespace Net
{
    namespace
    {
        Config g_config;
        ENetHost* g_host = nullptr;
        ENetPeer* g_peer = nullptr; // у Host и Client ровно один собеседник
        bool g_connected = false;
        uint32_t g_sequence = 0;
        uint64_t g_last_ping_ms = 0, g_last_stats_ms = 0, g_last_connect_ms = 0;
        // Паузы между вызовами tick (кадры игры): сеть обслуживается из игрового потока, поэтому rtt ENet не меньше кадра.
        // Окно статистики — 10 с (лог «[Net] связь»)
        uint64_t g_last_tick_ms = 0, g_gap_sum_ms = 0, g_gap_max_ms = 0;
        uint32_t g_gap_count = 0;
        uint32_t g_pings_received = 0;
        uint32_t g_tx_id = 0; // id последней отправки файла
        // Приём файла по кускам: у каждого вида файла своё состояние, чтобы зона и сейв пассажира могли идти одновременно
        struct Rx
        {
            std::vector<uint8_t> data;
            uint32_t id = 0, received = 0, logged_quarter = 0;
            uint64_t started_ms = 0;
        };
        Rx g_rx_zone, g_rx_passenger;

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        void send(uint8_t channel, const std::vector<uint8_t>& data)
        {
            if (!g_peer || !g_connected) return;
            enet_peer_send(g_peer, channel, enet_packet_create(data.data(), data.size(), channel == Coop::ChannelUnreliable ? 0 : ENET_PACKET_FLAG_RELIABLE));
        }

        void connect_to_host(const Events& events)
        {
            ENetAddress address{};
            enet_address_set_host(&address, g_config.host_ip.c_str());
            address.port = g_config.port;
            g_peer = enet_host_connect(g_host, &address, Coop::NumChannels, 0);
            g_last_connect_ms = now_ms();
            events.log(std::format(L"[Net] подключение к {}:{}...", std::wstring(g_config.host_ip.begin(), g_config.host_ip.end()), g_config.port));
        }

        // true — файл принят целиком и CRC сошёлся (байты в rx.data, забирает вызывающий)
        bool receive_chunk(Rx& rx, const uint8_t* data, size_t size, const Events& events, const wchar_t* what)
        {
            auto* chunk = Coop::body_of<Coop::ZoneChunkBody>(data, size);
            if (!chunk || !chunk->Total) return false;
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
            // Все куски сразу в очередь ENet: дальше он сам дозирует отправку по окну и переотправляет потерянное
            for (body.Offset = 0; body.Offset < body.Total; body.Offset += Coop::ChunkSize)
            {
                uint32_t length = (std::min)(Coop::ChunkSize, body.Total - body.Offset);
                send(Coop::ChannelBulk, Coop::make_packet(id, ++g_sequence, body, file.data() + body.Offset, length));
            }
            enet_host_flush(g_host);
            events.log(std::format(L"[Net] {} поставлен в отправку: {} байт ({} кусков), CRC {:08X}", what, body.Total, (body.Total + Coop::ChunkSize - 1) / Coop::ChunkSize, body.Crc32));
        }

        void on_receive(const ENetEvent& e, const Events& events)
        {
            const uint8_t* data = e.packet->data;
            size_t size = e.packet->dataLength;
            if (size < sizeof(Coop::Header)) return;
            switch (data[0])
            {
            case Coop::Ping:
                if (g_config.role == Role::Host)
                {
                    send(e.channelID, std::vector<uint8_t>(data, data + size)); // эхо
                }
                else if (auto* ping = Coop::body_of<Coop::PingBody>(data, size); ping && ++g_pings_received % 10 == 0)
                {
                    events.log(std::format(L"[Net] эхо: rtt={} мс (канал {})", now_ms() - ping->ClientTimeMs, e.channelID));
                }
                break;
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
                if (auto* car = Coop::body_of<Coop::CarStateBody>(data, size); car && events.on_car_state) events.on_car_state(*car);
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
            case Coop::HostPose:
            case Coop::PassengerPose:
                if (auto* pose = Coop::body_of<Coop::PoseBody>(data, size); pose && events.on_pose) events.on_pose(*pose);
                break;
            default:
                events.log(std::format(L"[Net] неизвестный пакет {}", data[0]));
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
            else if (key == "exit_side_cm") config.exit_side_cm = static_cast<float>(std::atof(value.c_str()));
            else if (key == "status_overlay") config.status_overlay = value != "0";
            else if (key == "client_parts") config.client_parts = value != "0";
        }
        fclose(f);
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
            if (g_host) connect_to_host(events);
        }
        return g_host != nullptr;
    }

    void stop()
    {
        if (g_peer && g_connected) enet_peer_disconnect_now(g_peer, 0);
        if (g_host) enet_host_destroy(g_host);
        if (g_config.role != Role::Off) enet_deinitialize();
        g_host = nullptr;
        g_peer = nullptr;
        g_connected = false;
    }

    void tick(const Events& events)
    {
        if (!g_host) return;
        ENetEvent e;
        while (enet_host_service(g_host, &e, 0) > 0)
        {
            switch (e.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                // shortcut: новое подключение всегда вытесняет старое (кто угодно в LAN может выбить напарника), когда понадобится защита — проверять IP
                if (g_peer && g_peer != e.peer) enet_peer_disconnect_now(g_peer, 0);
                g_peer = e.peer;
                g_connected = true;
                enet_peer_timeout(g_peer, 0, Coop::PeerTimeoutMinMs, Coop::PeerTimeoutMaxMs);
                events.log(L"[Net] соединение установлено");
                events.on_connected();
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (e.peer != g_peer) break; // вытесненное старое соединение
                events.log(L"[Net] соединение разорвано");
                g_peer = nullptr;
                g_connected = false;
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                on_receive(e, events);
                enet_packet_destroy(e.packet);
                break;
            default:
                break;
            }
        }

        uint64_t now = now_ms();
        if (g_last_tick_ms)
        {
            uint64_t gap = now - g_last_tick_ms;
            g_gap_sum_ms += gap;
            g_gap_max_ms = (std::max)(g_gap_max_ms, gap);
            ++g_gap_count;
        }
        g_last_tick_ms = now;
        if (g_connected && now - g_last_stats_ms >= 10'000)
        {
            g_last_stats_ms = now;
            events.log(std::format(L"[Net] связь: rtt={} мс, потери={:.1f}%, кадр игры: в среднем {} мс, максимум {} мс", g_peer->roundTripTime,
                                   g_peer->packetLoss * 100.0 / static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE), g_gap_count ? g_gap_sum_ms / g_gap_count : 0, g_gap_max_ms));
            g_gap_sum_ms = g_gap_max_ms = 0;
            g_gap_count = 0;
        }
        if (g_config.role == Role::Client)
        {
            if (!g_connected && now - g_last_connect_ms > 5000)
            {
                if (g_peer) enet_peer_reset(g_peer);
                connect_to_host(events);
            }
            if (g_connected && now - g_last_ping_ms >= 1000)
            {
                g_last_ping_ms = now;
                Coop::PingBody ping{now};
                send(Coop::ChannelReliable, Coop::make_packet(Coop::Ping, ++g_sequence, ping));
                send(Coop::ChannelUnreliable, Coop::make_packet(Coop::Ping, ++g_sequence, ping));
            }
        }
    }

    bool connected() { return g_connected; }

    int zone_progress()
    {
        bool receiving = g_rx_zone.id && !g_rx_zone.data.empty() && g_rx_zone.received < g_rx_zone.data.size();
        return receiving ? static_cast<int>(g_rx_zone.received * 100ull / g_rx_zone.data.size()) : -1;
    }

    int rtt_ms() { return g_peer && g_connected ? static_cast<int>(g_peer->roundTripTime) : -1; }

    double loss_percent()
    {
        return g_peer && g_connected ? g_peer->packetLoss * 100.0 / static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE) : 0.0;
    }

    void send_loot(Coop::PacketId id, const Coop::LootBody& body)
    {
        send(Coop::ChannelReliable, Coop::make_packet(id, ++g_sequence, body));
    }

    void send_door(Coop::PacketId id, const Coop::DoorBody& body)
    {
        send(Coop::ChannelReliable, Coop::make_packet(id, ++g_sequence, body));
    }

    void send_garage_door(Coop::PacketId id, const Coop::GarageDoorBody& body)
    {
        send(Coop::ChannelReliable, Coop::make_packet(id, ++g_sequence, body));
    }

    void send_systems(const Coop::SystemsBody& body)
    {
        send(Coop::ChannelReliable, Coop::make_packet(Coop::CarSystems, ++g_sequence, body));
    }

    bool send_part(const std::string& tag, const std::string& path, Coop::PacketId id)
    {
        auto packet = Coop::make_part_packet(g_sequence + 1, tag, path, id);
        if (packet.empty()) return false;
        ++g_sequence;
        send(Coop::ChannelReliable, packet);
        return true;
    }

    bool send_trunk(Coop::PacketId id, uint16_t count, const std::string& path, const std::vector<Coop::TrunkValue>& values)
    {
        auto packet = Coop::make_trunk_packet(id, g_sequence + 1, count, path, values);
        if (packet.empty()) return false;
        ++g_sequence;
        send(Coop::ChannelReliable, packet);
        return true;
    }

    void send_seat(Coop::PacketId id, const Coop::SeatBody& body)
    {
        send(Coop::ChannelReliable, Coop::make_packet(id, ++g_sequence, body));
    }

    void send_car_state(const Coop::CarStateBody& state)
    {
        send(Coop::ChannelUnreliable, Coop::make_packet(Coop::CarState, ++g_sequence, state));
    }

    void send_pose(const Coop::PoseBody& pose)
    {
        send(Coop::ChannelUnreliable, Coop::make_packet(g_config.role == Role::Host ? Coop::HostPose : Coop::PassengerPose, ++g_sequence, pose));
    }

    void send_zone_save(const std::vector<uint8_t>& file, const Events& events)
    {
        send_file(Coop::ZoneSave, file, events, L"сейв зоны");
    }

    void send_passenger_save(const std::vector<uint8_t>& file, const Events& events)
    {
        send_file(Coop::PassengerSave, file, events, L"сейв пассажира");
    }

    void send_passenger_request(const Coop::SaveRequestBody& body)
    {
        send(Coop::ChannelReliable, Coop::make_packet(Coop::PassengerSaveRequest, ++g_sequence, body));
    }
} // namespace Net
