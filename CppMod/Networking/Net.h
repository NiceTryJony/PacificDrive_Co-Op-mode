// Сеть мода поверх ENet (docs/design/DECISIONS.md «Транспорт: ENet»). Без заголовков UE4SS: winsock из ENet с ними конфликтует.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
        bool camera_follow = true; // камера сидящего пассажира поворачивается вместе с машиной
        // Высадка (Ctrl+E): на сколько см в сторону от оси машины (с той стороны, где сидели); 0 — штатная точка игры (ставила на крышу)
        float exit_side_cm = 230;
        // Надпись «Co-op: …» поверх игры (отдельное окно Windows); 0 — выключить, если рядом оверлеи (RTSS, Afterburner…) и игра падает
        bool status_overlay = true;
        // Детали машины, которые пассажир ставит/снимает на своей копии, уходят Host-у (CarPartRequest). 0 — запрет:
        // Client откатывает свою правку к состоянию Host, Host запросы игнорирует
        bool client_parts = true;
        // Тест досылки: доля (%) unreliable-пакетов (поза, машина), которые сеть «теряет» при отправке; 0 — выключено (coop.ini: sim_loss)
        float sim_loss_percent = 0;
        // Тест на одном ПК: HeapGuard ведёт себя как у Client, даже если игра — Host (coop.ini: heap_test=1)
        bool heap_test = false;
        // Client, сидя в машине, шлёт Host-у свой газ и руль (DriveInput) — Host ведёт машину по ним. По умолчанию выключено (coop.ini: client_drive=1)
        bool client_drive = false;
        // Предмет в руке собеседника: показывать его работу (насадки, пламя, плазма, вращение), а не только корпус (coop.ini: hand_fx=0 — только модель)
        bool hand_fx = false; // по умолчанию выкл: огоньки и насадки по частям вставали криво (2026-11-19), показываем модель
        // Размер предмета в руке куклы (1 = как в игре; coop.ini: hand_scale)
        float hand_scale = 0.5f;
        // Разведка 4б (DriveProbe): подмена оси газа/руля у машины Host; оба 0 — выключено (coop.ini: drive_probe_throttle, drive_probe_steer)
        float drive_probe_throttle = 0, drive_probe_steer = 0;
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
        std::function<void(const Coop::QuirkSetBody&)> on_quirk_set;  // Client: причуды машины Host
        std::function<void(const Coop::QuirkBody&)> on_quirk_cure;    // Host: пассажир вылечил причуду
        std::function<void(const Coop::QuirkKnowBody&)> on_quirk_know; // Host: пассажир выяснил о причуде
        std::function<void(const Coop::WorldItemBody&, const std::string&, const std::vector<Coop::TrunkValue>&)> on_world_item; // собеседник положил предмет на землю
        std::function<void(const std::string&, const std::string&)> on_radio; // Client: текущий трек радио Host (вид, имя)
        std::function<void(const std::string&, const std::string&)> on_weather; // Client: погода Host-а — тег "wev" + путь класса WEV_* / "lvl" + уровни
        std::function<void(const std::string&, const std::string&)> on_gate; // Client: раскрытые отрезки маршрута у Host (тег "legs")
        std::function<void(const std::string&, const std::string&)> on_hand; // оба: предмет в руке собеседника (тег "hand", путь меша)
        std::function<void(Coop::PacketId, const Coop::AnchorBody&)> on_anchor; // AnchorTaken — якорь взял собеседник, PortalActive — активировал портал
        std::function<void(const std::string&, const std::string&)> on_census; // оба: срез зоны собеседника (карта, «Класс=число …»)
        std::function<void(const std::string&, const std::string&)> on_hazard; // Client: аномалия Host-а от якоря (позиция и поворот текстом, путь класса)
    };

    // Реестр: модуль сам подписывается на свои пакеты (игровой поток, из tick). body — после заголовка. Пакет без подписчика и без поля в Events — «неизвестный».
    // Новые синхронизации — только так: ни Events, ни switch в Net.cpp править не нужно. Класс приоритета и ключ слияния — Coop::priority_of / key
    using Handler = std::function<void(const uint8_t* body, size_t size)>;
    void on(Coop::PacketId id, Handler handler);
    // key != 0: пока предыдущий пакет с этим ключом не ушёл, он заменяется этим (состояние «последнее побеждает»)
    void send(Coop::PacketId id, const std::vector<uint8_t>& payload, uint64_t key = 0);

    // coop.ini: строки key=value — role=host|client|off, host_ip=..., port=..., seat_x/seat_y/seat_z=...
    Config load_config(const std::filesystem::path& path);
    bool start(const Config& config, const Events& events);
    void stop();
    // shortcut: ENet обслуживается из игрового тика (не свой поток) — хватает при длинных таймаутах; поток — если начнёт рваться связь
    void tick(const Events& events);
    uint32_t adaptive_interval_ms(uint32_t base_ms); // base_ms, а при плохой связи (rtt/потери) вдвое-полтора больше
    bool connected();
    int zone_progress();    // Client: сколько процентов сейва зоны уже принято; -1 — приёма сейчас нет
    int rtt_ms();           // -1 — нет связи
    bool version_mismatch(); // Client: Host отклонил — версии мода не совпадают
    int connect_attempts();  // Client: сколько попыток подключения с последнего успешного соединения
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
    void send_quirk_set(const Coop::QuirkSetBody& body);            // reliable, Host
    void send_quirk_cure(const Coop::QuirkBody& body);              // reliable, Client
    void send_quirk_know(const Coop::QuirkKnowBody& body);          // reliable, Client
    bool send_world_item(const Coop::WorldItemBody& body, const std::string& path, const std::vector<Coop::TrunkValue>& values); // reliable, оба
    void send_anchor(const Coop::AnchorBody& body, Coop::PacketId id = Coop::AnchorTaken); // reliable, оба; PortalActive — тем же телом
    void send_garage_door(Coop::PacketId id, const Coop::GarageDoorBody& body); // reliable: GarageDoor (Host) / GarageDoorRequest (Client)
    // reliable; CarPart — Host → Client, CarPartRequest — Client → Host; false — не влезло в пакет
    bool send_part(const std::string& tag, const std::string& path, Coop::PacketId id = Coop::CarPart);
    bool send_trunk(Coop::PacketId id, uint16_t count, const std::string& path, const std::vector<Coop::TrunkValue>& values = {}); // reliable; false — не влезло в пакет
} // namespace Net
