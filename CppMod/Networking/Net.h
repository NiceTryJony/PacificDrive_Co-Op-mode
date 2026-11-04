// Сеть мода поверх ENet (docs/DECISIONS.md «Транспорт: ENet»). Без заголовков UE4SS: winsock из ENet с ними конфликтует.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "Protocol.h"

namespace Net
{
    enum class Role
    {
        Off,
        Host,
        Client
    };

    struct Config
    {
        Role role = Role::Off;
        std::string host_ip = "127.0.0.1";
        uint16_t port = 0;
        // Заднее сиденье относительно точки водителя (TempSocket_Seat), см, оси машины: x вперёд, y вправо, z вверх.
        // В coop.ini, чтобы подгонять без пересборки
        float seat_x = -95, seat_y = 0, seat_z = 40;
        // То же для правого заднего места: отсчёт от точки водителя, отражённой через ось машины (y → −y)
        float seat_right_x = -95, seat_right_y = 0, seat_right_z = 40;
        // Пока пассажир сидит, подсвечиваются (нажимаются) только двери и карта; 0 — отключить, если что-то нужное оказалось недоступным
        bool restrict_interactions = true;
        // Высадка (Ctrl+E): на сколько см в сторону от оси машины (с той стороны, где сидели); 0 — штатная точка игры (ставила на крышу)
        float exit_side_cm = 230;
        // Надпись «Co-op: …» поверх игры (отдельное окно Windows); 0 — выключить, если рядом оверлеи (RTSS, Afterburner…) и игра падает
        bool status_overlay = true;
        // Детали машины, которые пассажир ставит/снимает на своей копии, уходят Host-у (CarPartRequest). 0 — запрет:
        // Client откатывает свою правку к состоянию Host, Host запросы игнорирует
        bool client_parts = true;
    };

    struct Events
    {
        std::function<void(const std::wstring&)> log;
        std::function<void()> on_connected;
        std::function<void(std::vector<uint8_t>&&)> on_zone_save; // Client: файл сейва зоны, CRC уже проверен
        std::function<void(const Coop::PoseBody&)> on_pose;      // поза собеседника (Host ← пассажир, Client ← водитель)
        std::function<void(const Coop::CarStateBody&)> on_car_state; // Client: машина Host
        std::function<void(Coop::PacketId, const Coop::SeatBody&)> on_seat; // посадка/высадка: запросы (Host) и ответы (Client)
        std::function<void(Coop::PacketId, const Coop::DoorBody&)> on_door; // двери: DoorState (Client), DoorRequest (Host)
        std::function<void(std::vector<uint8_t>&&)> on_passenger_save;            // файл сейва пассажира, CRC проверен (Host — от Client, Client — ответ Host-а)
        std::function<void(const Coop::SaveRequestBody&)> on_passenger_request;  // Host: Client просит свой сейв; Client: у Host-а его нет
        std::function<void(const Coop::SystemsBody&)> on_systems; // Client: зажигание/свет/топливо/аккумулятор Host
        std::function<void(const std::string&, const std::string&)> on_part; // Client: слот машины Host (тег, путь архетипа; пустой — слот пуст)
        std::function<void(Coop::PacketId, const Coop::GarageDoorBody&)> on_garage_door; // GarageDoor (Client: дверь Host), GarageDoorRequest (Host: Client сменил)
        std::function<void(const std::string&, const std::string&)> on_part_request; // Host: пассажир сменил деталь в слоте (тег, путь; пустой — снял)
        // багажник: TrunkItem/TrunkDenied (Client), TrunkTake/TrunkPut (Host) — сколько предметов архетипа
        std::function<void(Coop::PacketId, uint16_t, const std::string&, const std::vector<Coop::TrunkValue>&)> on_trunk;
        std::function<void(Coop::PacketId, const Coop::LootBody&)> on_loot; // подбор лута: LootPicked/LootDenied (Client), InventoryAction (Host)
    };

    // coop.ini: строки key=value — role=host|client|off, host_ip=..., port=..., seat_x/seat_y/seat_z=...
    Config load_config(const std::filesystem::path& path);
    bool start(const Config& config, const Events& events);
    void stop();
    // shortcut: ENet обслуживается из игрового тика (не свой поток) — хватает при длинных таймаутах; поток — если начнёт рваться связь
    void tick(const Events& events);
    bool connected();
    int zone_progress();    // Client: сколько процентов сейва зоны уже принято; -1 — приёма сейчас нет
    int rtt_ms();           // -1 — нет связи
    double loss_percent();  // для окна статуса
    void send_zone_save(const std::vector<uint8_t>& file, const Events& events);
    void send_passenger_save(const std::vector<uint8_t>& file, const Events& events); // тем же способом, что зона
    void send_passenger_request(const Coop::SaveRequestBody& body);                   // reliable
    // Своя поза собеседнику: Host шлёт HostPose, Client — PassengerPose (unreliable, старые отбрасываются)
    void send_pose(const Coop::PoseBody& pose);
    // Host: состояние машины (unreliable)
    void send_car_state(const Coop::CarStateBody& state);
    // Посадка/высадка (reliable): RequestSeat/RequestExitVehicle от Client, PassengerAttach/PassengerDetach от Host
    void send_seat(Coop::PacketId id, const Coop::SeatBody& body);
    void send_loot(Coop::PacketId id, const Coop::LootBody& body); // reliable
    void send_door(Coop::PacketId id, const Coop::DoorBody& body); // reliable: DoorState (Host) / DoorRequest (Client)
    void send_systems(const Coop::SystemsBody& body);               // reliable, Host
    void send_garage_door(Coop::PacketId id, const Coop::GarageDoorBody& body); // reliable: GarageDoor (Host) / GarageDoorRequest (Client)
    // reliable; CarPart — Host → Client, CarPartRequest — Client → Host; false — не влезло в пакет
    bool send_part(const std::string& tag, const std::string& path, Coop::PacketId id = Coop::CarPart);
    bool send_trunk(Coop::PacketId id, uint16_t count, const std::string& path, const std::vector<Coop::TrunkValue>& values = {}); // reliable; false — не влезло в пакет
} // namespace Net
