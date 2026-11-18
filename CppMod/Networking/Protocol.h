// Протокол Host ↔ Client (спека 3.2.3, ID — docs/sync/SYNC_MAP.md). Используется модом (сетевой поток и игровой).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Coop
{
    constexpr uint16_t DefaultPort = 47777;
    // Версия протокола уходит в ENet connect(data): Host отклоняет несовпавшую. Менять при любом изменении формата пакетов
    constexpr uint32_t ProtocolVersion = 6;
    // Классы приоритета = каналы ENet (у каждого свой порядок доставки: застрявший пакет не держит остальные)
    enum Priority : uint8_t
    {
        PrioCritical = 0,   // посадка/высадка, запрос сейва, ping — всегда первыми, reliable
        PrioRealtime = 1,   // поза, машина — unreliable, в очереди только последнее значение
        PrioEvent = 2,      // двери, детали, багажник, лут, якорь — reliable
        PrioBackground = 3, // погода, радио, срез зоны, метки — reliable, после событий
        PrioBulk = 4,       // сейвы: получают то, что осталось, окно в полёте ограничено
        PrioCount = 5
    };
    constexpr size_t NumChannels = PrioCount;
    constexpr uint32_t ChunkSize = 64 * 1024; // файл режется на куски: прогресс в логе, основа для докачки

    enum PacketId : uint8_t
    {
        CarState = 1,       // Host→Client: машина (CarStateBody), спека 3.2.2
        PassengerAttach = 3, // Host→Client: ответ на RequestSeat (SeatBody)
        PassengerDetach = 4, // Host→Client: ответ на RequestExitVehicle (SeatBody)
        DoorState = 14,      // Host→Client: двери/капот/багажник машины изменились (DoorBody), docs/sync/SYNC_MAP.md
        CarSystems = 15,     // Host→Client: зажигание, свет, топливо, аккумулятор (SystemsBody)
        CarPart = 16,        // Host→Client: что стоит в слоте машины (PartBody + тег и путь архетипа)
        TrunkItem = 17,      // Host→Client: сколько предметов одного типа в багажнике (TrunkItemBody + путь архетипа)
        TrunkDenied = 18,    // Host→Client: из «взял N» столько у Host уже не было — убрать у пассажира (TrunkItemBody)
        GarageDoor = 32,     // Host→Client: дверь/ворота гаража открыта или закрыта (GarageDoorBody), опознаётся по позиции
        GarageDoorRequest = 33, // Client→Host: пассажир открыл/закрыл дверь гаража (GarageDoorBody) — решает Host
        CarPartRequest = 31, // Client→Host: пассажир поставил/снял деталь в слоте машины (PartBody + тег и путь, как CarPart) — решает Host
        TrunkTake = 29,     // Client→Host: пассажир взял из багажника N предметов типа (TrunkItemBody)
        TrunkPut = 30,       // Client→Host: пассажир положил в багажник N предметов типа (TrunkItemBody)
        LootPicked = 6,      // Host→Client: предмет из мира забрали — убрать у себя (LootBody), спека 3.2.2
        LootDenied = 13,     // Host→Client: этот предмет Host уже забрал — убрать из своего инвентаря (LootBody)
        ZoneSave = 9,       // Host→Client: кусок файла сейва зоны (ZoneChunkBody + данные)
        HostPose = 12,      // Host→Client: поза водителя (PoseBody)
        RequestSeat = 21,        // Client→Host: хочу сесть (SeatBody)
        RequestExitVehicle = 22, // Client→Host: хочу выйти (SeatBody)
        InventoryAction = 23,    // Client→Host: я подобрал предмет из мира (LootBody), спека 3.2.2
        DoorRequest = 27,        // Client→Host: хочу открыть/закрыть дверь (DoorBody) — решает Host
        PassengerPose = 25, // Client→Host: поза пассажира (PoseBody) — вместо ввода, docs/design/DECISIONS.md
        PassengerSave = 26,        // Client→Host: сейв кооп-слота с инвентарём пассажира; Host→Client: ответ на запрос (куски, как ZoneSave)
        PassengerSaveRequest = 28, // Client→Host: пришли мой сейв (Have=1); Host→Client: у меня его нет (Have=0)
        QuirkSet = 34,     // Host→Client: все причуды машины Host (QuirkSetBody), RECON.md 6.30
        QuirkCure = 35,    // Client→Host: пассажир вылечил причуду (QuirkBody) — Host лечит у себя
        AnchorTaken = 36,  // оба направления: якорь взят из основания в этой точке (AnchorBody) — снять у себя
        WorldItem = 39,    // оба направления: игрок положил предмет на землю (WorldItemBody + путь архетипа + значения свойств)
        RadioTrack = 38,   // Host→Client: текущий трек радио (PartBody: вид file|stock + имя файла/Title), RECON.md 6.36
        QuirkKnow = 37,    // Client→Host: пассажир выяснил о причуде (QuirkKnowBody) — Host добавляет к своему знанию
        Ping = 40,    // Client→Host, Host отвечает тем же пакетом (эхо)
        PortalActive = 41, // оба направления: портал выхода в этой точке активирован (AnchorBody) — активировать у себя
        WeatherEvent = 42, // Host→Client (PartBody): тег "wev" + путь класса WEV_* (пусто — нет события) или тег "lvl" + 6 уровней погоды текстом
        Mark = 44,         // оба: игрок нажал Ctrl+F7 «здесь что-то не так» (AnchorBody — где он стоит) — метка в лог собеседника
        ZoneCensus = 45,   // оба: срез зоны (PartBody: тег — имя карты, значение — «Класс=число …») для сверки [ZoneDiff]
        GateState = 46,    // оба (Host — всё раскрытое; Client — раскрытое у него выше, чем у Host): раскрытые отрезки маршрута (PartBody: тег "legs", значение «индекс RouteShapeLeg:статус» через запятую) — от статуса 2 открываются ворота гаража
        HazardSpawn = 43,  // Host→Client: аномалия от взятия якоря (PartBody: тег "x y z pitch yaw roll" + путь класса) — создать там же
        DriveInput = 47,   // Client→Host: ввод Client-водителя (DriveInputBody) — Host подставляет в ось газа/руля; нули при обрыве и высадке
        // Пиявки (LeechSync, design/LEECH_PLAN.md): Host ведёт, у Client марионетки
        LeechSpawn = 50, // Host→Client: u16 номер, float x y z, int16 pitch yaw roll (pack_angle), путь архетипа — создать марионетку;
                         // Client→Host (номер 0): Client выбросил пиявку из рук — Host создаёт настоящую и ведёт её
        LeechState = 51, // Host→Client, unreliable: u8 число, на каждую u16 номер, u8 флаги (LeechOnChassis), float x y z, int16 pitch yaw roll
        LeechGone = 52,  // Host→Client: u16 номер — пиявки у Host больше нет
        LeechHello = 53, // Client→Host: карта у меня готова, пришли всех пиявок заново (без тела)
        GeneratorBroken = 49, // оба: плазменный генератор в этой точке сломан (AnchorBody) — сломать у себя (поломка при появлении — случай, при сборе — всегда)
        LeechAction = 55, // Client→Host: u16 номер пиявки Host, u8 действие (LeechKnockOff / LeechKill) — Client сделал это с марионеткой, Host повторяет у своей
        HandItem = 54,    // оба: что у отправителя в руке (PartBody: тег "hand", значение — путь меша BaseMesh предмета; пусто — рука пуста) — у куклы показываем меш
        DriveAction = 48,  // Client→Host: действие Client-водителя в салоне (DriveActionBody) — гудок, дворники, свет, зажигание, коробка, способность; Host вызывает у себя
    };

    inline Priority priority_of(PacketId id)
    {
        switch (id)
        {
        case PassengerAttach: case PassengerDetach: case RequestSeat: case RequestExitVehicle: case PassengerSaveRequest: case Ping:
            return PrioCritical;
        case CarState: case HostPose: case PassengerPose: case DriveInput: case LeechState:
            return PrioRealtime;
        case WeatherEvent: case RadioTrack: case ZoneCensus: case Mark:
            return PrioBackground;
        case ZoneSave: case PassengerSave:
            return PrioBulk;
        default:
            return PrioEvent;
        }
    }

#pragma pack(push, 1)
    struct Header
    {
        uint8_t Id;
        uint32_t Sequence;
    };

    struct PingBody
    {
        uint64_t ClientTimeMs; // время отправки по часам клиента — RTT считает клиент
    };

    struct PoseBody
    {
        float X, Y, Z; // центр капсулы персонажа, мир (см)
        float Yaw;     // градусы
        uint8_t Flags; // PoseInCar
    };
    constexpr uint8_t PoseInCar = 1;
    constexpr uint32_t PoseIntervalMs = 50; // 20 Гц, спека 3.2.2

    struct CarStateBody
    {
        float X, Y, Z;          // позиция актора машины, мир (см)
        float Pitch, Yaw, Roll; // градусы
        float VX, VY, VZ;       // линейная скорость, см/с — для экстраполяции при потере пакетов
    };
    constexpr uint32_t CarStateIntervalMs = 33; // ~30 Гц, спека 3.2.2 (20–30 Гц)

    // На проводе углы — int16 (360°/65536 ≈ 0,0055°), скорость — int16 по 2 см/с; позиция float (точность важнее). 36 → 24 байта
    struct CarStateWire
    {
        float X, Y, Z;
        int16_t Pitch, Yaw, Roll;
        int16_t VX, VY, VZ;
    };

    inline int16_t pack_angle(float degrees)
    {
        float d = std::fmod(degrees, 360.0f);
        return static_cast<int16_t>(static_cast<uint16_t>(static_cast<int32_t>(std::lround(d * (65536.0f / 360.0f)))));
    }
    inline float unpack_angle(int16_t v) { return v * (360.0f / 65536.0f); }
    inline int16_t pack_speed(float v) { return static_cast<int16_t>(std::clamp(std::lround(v / 2), -32767l, 32767l)); }
    inline float unpack_speed(int16_t v) { return v * 2.0f; }

    inline CarStateWire to_wire(const CarStateBody& s)
    {
        return {s.X, s.Y, s.Z, pack_angle(s.Pitch), pack_angle(s.Yaw), pack_angle(s.Roll), pack_speed(s.VX), pack_speed(s.VY), pack_speed(s.VZ)};
    }
    inline CarStateBody from_wire(const CarStateWire& w)
    {
        return {w.X, w.Y, w.Z, unpack_angle(w.Pitch), unpack_angle(w.Yaw), unpack_angle(w.Roll), unpack_speed(w.VX), unpack_speed(w.VY), unpack_speed(w.VZ)};
    }

    // Ввод Client-водителя. Оси −127…127 = −1…1 (газ: минус = назад, как AccelerateBrake). Дальше — по плану: коробка/ручник запросом отдельно
    struct DriveInputBody
    {
        int8_t Steer;    // SteerLeftRight
        int8_t Throttle; // AccelerateBrake
        uint8_t Flags;   // 0 — зарезервировано; нули = «не за рулём»
    };
    constexpr uint32_t DriveInputIntervalMs = 33; // ~30 Гц, как событие газа в игре

    // Действие в салоне. Value — новое состояние (bool / зажигание 0..2 / коробка 0..1 / «нажата» для способности), Arg — слот способности
    enum DriveActionKind : uint8_t
    {
        DriveHorn = 1,    // ToggleCarHorn(Active)
        DriveWipers = 2,  // BP_CarManager_C.SetWipersOnOffVariable
        DriveLights = 3,  // Set Headlights On
        DriveIgnition = 4, // BP_CarManager_C.SetCarIgnitionState
        DriveGear = 5,    // ToggleGearBoxState
        DriveAbility = 6, // SetAbilityPressed(Slot, Pressed)
    };
    struct DriveActionBody
    {
        uint8_t Kind;
        uint8_t Value;
        uint8_t Arg;
    };

    struct SeatBody
    {
        uint8_t Seat;   // 0 — задний левый, 1 — задний правый, SeatAny — любой свободный
        uint8_t Result; // SeatResult (в запросах не используется)
    };
    constexpr uint8_t SeatAny = 255;
    enum SeatResult : uint8_t
    {
        SeatOk = 0,
        SeatBlockedByModule = 1, // в слоте заднего ряда стоит модуль (спека 3.3.3)
        SeatNoCar = 2,
        SeatDoorClosed = 3,      // задняя дверь с этой стороны закрыта (откройте её или снимите)
        SeatDriverBusy = 4,      // место 2 (водитель): персонаж Host сидит в машине
    };
    constexpr uint8_t SeatDriver = 2; // Client за рулём (client_drive): мод сажает его на точку водителя, как на заднее сиденье

    // Двери машины: 6 штук, EOpenableDoors 0..5 (BP_CarManager_C.DoorStates, RECON.md «6.2»); бит i = дверь i открыта.
    // DoorState: Changed — какие двери только что изменились (Client применяет только их), Mask — состояние всех шести.
    // DoorRequest: Changed — какие двери Client хочет изменить, Mask — желаемое состояние этих дверей
    struct DoorBody
    {
        uint8_t Mask;
        uint8_t Changed;
    };
    constexpr uint8_t DoorCount = 6, DoorAll = 0x3F;

    // Host применяет запрос Client-а к своему состоянию: возвращает DoorState для рассылки (Changed = реально изменившиеся
    // двери; 0 — запрос ничего не поменял, рассылать не надо)
    inline DoorBody apply_door_request(uint8_t& state, const DoorBody& request)
    {
        uint8_t changed = static_cast<uint8_t>((state ^ request.Mask) & request.Changed & DoorAll);
        state = static_cast<uint8_t>((state & ~changed) | (request.Mask & changed));
        return {state, changed};
    }

    // Client→Host Have=1: «пришли мне мой сейв пассажира»; Host→Client Have=0: «сохранённого у меня нет» (Client перестаёт ждать)
    struct SaveRequestBody
    {
        uint8_t Have;
    };

    // Системы машины Host → Client, всегда полным состоянием (5 байт). Ignition — CarIgnitionState 0..2 (RECON.md «6.9»),
    // Fuel/Battery — проценты 0..100 (топливо и заряд у игры суммы по предметам-деталям, записать их на кукле нечем),
    // Radio — 0/1, CarState_RadioOnOff (RECON.md «6.16»)
    struct SystemsBody
    {
        uint8_t Ignition;
        uint8_t Lights;
        uint8_t Fuel;
        uint8_t Battery;
        uint8_t Radio;
        uint8_t Wipers; // 0/1, BP_CarManager_C.WiperBladesOnOff (протокол 5)
    };
    constexpr uint8_t IgnitionMax = 2;
    constexpr uint8_t LeechKnockOff = 1, LeechKill = 2, LeechTake = 3; // LeechAction: сбита (KnockOff) / убита (здоровье 0) / подобрана Client-ом
    constexpr uint8_t LeechOnChassis = 1; // LeechState: позиция и поворот — относительно Chassis машины (пиявка сцеплена)
    constexpr uint8_t PercentStep = 1; // изменение процента меньше шага не рассылаем

    // Нужно ли слать: зажигание и свет — сразу при любом изменении, проценты — когда ушли на шаг и больше
    inline bool systems_changed(const SystemsBody& sent, const SystemsBody& now)
    {
        auto apart = [](uint8_t a, uint8_t b) { return (a > b ? a - b : b - a) >= PercentStep; };
        return sent.Ignition != now.Ignition || sent.Lights != now.Lights || sent.Radio != now.Radio || sent.Wipers != now.Wipers || apart(sent.Fuel, now.Fuel) || apart(sent.Battery, now.Battery);
    }

    // Причуда = пара классов «условие → поведение» (SE_Quirk_MixAndMatch_C.QuirkCondition/QuirkBehavior): у Host и Client одинаковые,
    // все причуды — один класс, поэтому ключ — FNV-1a строки "QC_…|QB_…"
    struct QuirkBody
    {
        uint32_t Key;
    };
    constexpr int MaxQuirks = 16;
    struct QuirkSetBody
    {
        uint8_t Count;
        uint32_t Keys[MaxQuirks];
        uint8_t Known[MaxQuirks]; // что о причуде выяснено (биты QuirkKnown*), у причуд с одним ключом — объединение
    };
    // Знание о причуде только растёт — синхронизируем объединением (ИЛИ): что нашёл любой из двоих, видят оба
    constexpr uint8_t QuirkDiagnosed = 1,        // SE_Quirk_MixAndMatch_C.Diagnosed
        QuirkConditionPart = 2, QuirkConditionType = 4, // QuirkCondition.PartKnown / TypeKnown (BP_BaseQuirkElement_C)
        QuirkBehaviorPart = 8, QuirkBehaviorType = 16,  // QuirkBehavior.PartKnown / TypeKnown
        // «решение найдено»: SE_Quirk_MixAndMatch_C.IsQuirkKnown(категория).FullyKnown, категории 0..2 — биты 32, 64, 128.
        // Флаги выше панель решения не читает (тест 2026-11-10: Diagnosed дошёл, решения у второго игрока не видно)
        QuirkFullyKnown0 = 32;
    constexpr int QuirkCategories = 3; // EQuirkDetectiveRevealCategory
    struct QuirkKnowBody
    {
        uint32_t Key;
        uint8_t Known;
    };

    // Основание якоря (BP_Spawn_Anchor_Base_01_C) неподвижно и пришло в сейве Host — опознаётся по позиции, см
    struct AnchorBody
    {
        int32_t X, Y, Z;
    };
    constexpr int32_t AnchorMatchCm = 300;

    // Дверь гаража (BP_GarageDoor_C): имена актёров у Host и Client могут разниться, позиция — нет (дверь неподвижна), см
    struct GarageDoorBody
    {
        int32_t X, Y, Z;
        uint8_t Open;
    };
    constexpr int32_t GarageDoorMatchCm = 100; // дверь Client — ближайшая к присланной не дальше этого

    // Предмет мира опознаётся по типу и позиции при первом появлении: имена объектов у Host и Client разные
    // (Client грузит сейв — RECON.md), а позиции совпадают до сантиметров
    struct LootBody
    {
        uint32_t Archetype; // FNV-1a имени архетипа
        float X, Y, Z;
    };
    constexpr float LootMatchRadius = 50; // см

    struct ZoneChunkBody
    {
        uint32_t TransferId; // новый id = новый файл, начатый приём сбрасывается
        uint32_t Offset;
        uint32_t Total;      // размер всего файла
        uint32_t Crc32;      // CRC всего файла
        // дальше байты куска
    };
    // Деталь машины Host → Client, один слот на пакет. Хвост: тег слота (Car.Equip.Body.DoorsRL) и значение слота:
    // путь архетипа (/Game/.../IA_X.IA_X), за ним через '|' пути классов эффектов состояния (SE_LeakyFuel…); PathLength 0 — слот пуст.
    // Только ASCII: теги и пути ассетов игры латиницей
    struct PartBody
    {
        uint8_t TagLength;
        uint16_t PathLength;
    };

    // Багажник, одна «стопка» на пакет: архетип + значения свойств (прочность, топливо, заряд — ItemProps::Specs).
    // TrunkItem: всего таких предметов у Host (по всем ячейкам), 0 — больше нет; TrunkTake/TrunkPut/TrunkDenied: сколько
    // взяли/положили/отказано. Не по ячейкам: игра сдвигает ячейки, когда одна пустеет (тест 2026-10-30 14:51).
    // Предметы одного архетипа с разными значениями — разные стопки
    struct TrunkItemBody
    {
        uint16_t Count;
        uint8_t PathLength;
        uint8_t ValueCount;
        // дальше путь архетипа (ASCII), затем ValueCount × TrunkValue
    };
    struct TrunkValue
    {
        uint8_t Spec;  // индекс в ItemProps::Specs
        int32_t Bits;  // int32 как есть; float — его биты
        bool operator==(const TrunkValue&) const = default;
    };
    constexpr size_t MaxTrunkValues = 8;

    // Предмет, который игрок положил на землю (выбросил, снял деталь, достал из багажника): создаётся у собеседника в той же точке.
    // Transform — FTransform как есть (3 × 4 float: поворот-кватернион, позиция, масштаб), дальше путь архетипа и ValueCount × TrunkValue
    struct WorldItemBody
    {
        float Transform[12];
        uint8_t CreateContext;
        uint8_t PathLength;
        uint8_t ValueCount;
    };
#pragma pack(pop)

    // CRC-32 (IEEE, как zlib.crc32) — проверка целостности файла после передачи
    inline uint32_t crc32(const uint8_t* data, size_t size)
    {
        static const auto table = [] {
            std::vector<uint32_t> t(256);
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    template <typename Body>
    std::vector<uint8_t> make_packet(PacketId id, uint32_t sequence, const Body& body, const uint8_t* tail = nullptr, size_t tail_size = 0)
    {
        std::vector<uint8_t> out(sizeof(Header) + sizeof(Body) + tail_size);
        Header h{id, sequence};
        std::memcpy(out.data(), &h, sizeof(h));
        std::memcpy(out.data() + sizeof(h), &body, sizeof(body));
        if (tail_size) std::memcpy(out.data() + sizeof(h) + sizeof(body), tail, tail_size);
        return out;
    }

    // Кодек полезной нагрузки: запись/чтение с проверкой границ (новые пакеты вместо ручной упаковки структур с хвостом)
    class Writer
    {
    public:
        Writer& u8(uint8_t v) { return raw(&v, 1); }
        Writer& u16(uint16_t v) { return raw(&v, 2); }
        Writer& u32(uint32_t v) { return raw(&v, 4); }
        Writer& f32(float v) { return raw(&v, 4); }
        Writer& str(const std::string& v) // длина u16 + байты; длиннее 65535 обрезается
        {
            uint16_t n = static_cast<uint16_t>((std::min<size_t>)(v.size(), 65535));
            u16(n);
            return raw(v.data(), n);
        }
        const std::vector<uint8_t>& bytes() const { return m_buf; }

    private:
        Writer& raw(const void* p, size_t n)
        {
            auto* b = static_cast<const uint8_t*>(p);
            m_buf.insert(m_buf.end(), b, b + n);
            return *this;
        }
        std::vector<uint8_t> m_buf;
    };

    class Reader
    {
    public:
        Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
        uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
        uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
        uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
        float f32() { float v = 0; raw(&v, 4); return v; }
        std::string str()
        {
            uint16_t n = u16();
            if (!m_ok || m_size - m_pos < n) { m_ok = false; return {}; }
            std::string v(reinterpret_cast<const char*>(m_data + m_pos), n);
            m_pos += n;
            return v;
        }
        bool ok() const { return m_ok; } // false — пакет короче, чем читали; прочитанное недействительно

    private:
        void raw(void* p, size_t n)
        {
            if (!m_ok || m_size - m_pos < n) { m_ok = false; return; }
            std::memcpy(p, m_data + m_pos, n);
            m_pos += n;
        }
        const uint8_t* m_data;
        size_t m_size, m_pos = 0;
        bool m_ok = true;
    };

    // Тело пакета нужного типа или nullptr, если пакет короче
    template <typename Body>
    const Body* body_of(const uint8_t* data, size_t size)
    {
        return size >= sizeof(Header) + sizeof(Body) ? reinterpret_cast<const Body*>(data + sizeof(Header)) : nullptr;
    }

    // Пустой вектор — тег длиннее 255 или значение длиннее 65535 (не влезают в PartBody)
    inline std::vector<uint8_t> make_part_packet(uint32_t sequence, const std::string& tag, const std::string& path, PacketId id = CarPart)
    {
        if (tag.empty() || tag.size() > 255 || path.size() > 65535) return {};
        std::string tail = tag + path;
        return make_packet(id, sequence, PartBody{static_cast<uint8_t>(tag.size()), static_cast<uint16_t>(path.size())},
                           reinterpret_cast<const uint8_t*>(tail.data()), tail.size());
    }

    // Пустой вектор — путь пустой или длиннее 255, значений больше MaxTrunkValues
    inline std::vector<uint8_t> make_trunk_packet(PacketId id, uint32_t sequence, uint16_t count, const std::string& path, const std::vector<TrunkValue>& values = {})
    {
        if (path.empty() || path.size() > 255 || values.size() > MaxTrunkValues) return {};
        std::vector<uint8_t> tail(path.begin(), path.end());
        tail.resize(path.size() + values.size() * sizeof(TrunkValue));
        if (!values.empty()) std::memcpy(tail.data() + path.size(), values.data(), values.size() * sizeof(TrunkValue));
        return make_packet(id, sequence, TrunkItemBody{count, static_cast<uint8_t>(path.size()), static_cast<uint8_t>(values.size())}, tail.data(), tail.size());
    }

    inline std::vector<uint8_t> make_world_item_packet(uint32_t sequence, const WorldItemBody& body, const std::string& path, const std::vector<TrunkValue>& values)
    {
        if (path.empty() || path.size() > 255 || values.size() > MaxTrunkValues) return {};
        std::vector<uint8_t> tail(path.begin(), path.end());
        tail.resize(path.size() + values.size() * sizeof(TrunkValue));
        if (!values.empty()) std::memcpy(tail.data() + path.size(), values.data(), values.size() * sizeof(TrunkValue));
        WorldItemBody b = body;
        b.PathLength = static_cast<uint8_t>(path.size());
        b.ValueCount = static_cast<uint8_t>(values.size());
        return make_packet(WorldItem, sequence, b, tail.data(), tail.size());
    }

    // false — мусор: обрезан, длины не сходятся, без архетипа
    inline bool parse_world_item(const uint8_t* data, size_t size, WorldItemBody& body, std::string& path, std::vector<TrunkValue>& values)
    {
        auto* b = body_of<WorldItemBody>(data, size);
        size_t offset = sizeof(Header) + sizeof(WorldItemBody);
        if (!b || !b->PathLength || b->ValueCount > MaxTrunkValues || size != offset + b->PathLength + b->ValueCount * sizeof(TrunkValue)) return false;
        body = *b;
        path.assign(reinterpret_cast<const char*>(data + offset), b->PathLength);
        values.assign(b->ValueCount, TrunkValue{});
        if (b->ValueCount) std::memcpy(values.data(), data + offset + b->PathLength, b->ValueCount * sizeof(TrunkValue));
        return true;
    }

    // false — мусор: обрезан, длины не сходятся, без архетипа, слишком много значений
    inline bool parse_trunk(const uint8_t* data, size_t size, uint16_t& count, std::string& path, std::vector<TrunkValue>& values)
    {
        auto* b = body_of<TrunkItemBody>(data, size);
        size_t offset = sizeof(Header) + sizeof(TrunkItemBody);
        if (!b || !b->PathLength || b->ValueCount > MaxTrunkValues || size != offset + b->PathLength + b->ValueCount * sizeof(TrunkValue)) return false;
        count = b->Count;
        path.assign(reinterpret_cast<const char*>(data + offset), b->PathLength);
        values.assign(b->ValueCount, TrunkValue{});
        if (b->ValueCount) std::memcpy(values.data(), data + offset + b->PathLength, b->ValueCount * sizeof(TrunkValue));
        return true;
    }

    // false — пакет обрезан или длины не сходятся (мусор из сети)
    inline bool parse_part(const uint8_t* data, size_t size, std::string& tag, std::string& path)
    {
        auto* body = body_of<PartBody>(data, size);
        size_t offset = sizeof(Header) + sizeof(PartBody);
        if (!body || !body->TagLength || size != offset + body->TagLength + body->PathLength) return false;
        tag.assign(reinterpret_cast<const char*>(data + offset), body->TagLength);
        path.assign(reinterpret_cast<const char*>(data + offset + body->TagLength), body->PathLength);
        return true;
    }
} // namespace Coop
