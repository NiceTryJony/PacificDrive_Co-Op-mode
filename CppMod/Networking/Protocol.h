// Протокол Host ↔ Client (спека 3.2.3, ID — docs/SYNC_MAP.md). Общий для мода и tools/CoopProbe.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Coop
{
    constexpr uint16_t DefaultPort = 47777;
    constexpr size_t NumChannels = 3;
    constexpr uint8_t ChannelReliable = 0;   // события (посадка, лут)
    constexpr uint8_t ChannelUnreliable = 1; // частые состояния (CarState и т.п.)
    constexpr uint8_t ChannelBulk = 2;       // файлы: свой reliable-канал, чтобы 3 МБ не задерживали события канала 0
    constexpr uint32_t ChunkSize = 64 * 1024; // файл режется на куски: прогресс в логе, основа для докачки
    // Загрузка уровня на клиенте блокирует игровой поток ~10 с, а ENet обслуживается из тика — дефолтные 5–30 с мало
    constexpr uint32_t PeerTimeoutMinMs = 60'000, PeerTimeoutMaxMs = 120'000;

    enum PacketId : uint8_t
    {
        CarState = 1,       // Host→Client: машина (CarStateBody), спека 3.2.2
        PassengerAttach = 3, // Host→Client: ответ на RequestSeat (SeatBody)
        PassengerDetach = 4, // Host→Client: ответ на RequestExitVehicle (SeatBody)
        DoorState = 14,      // Host→Client: двери/капот/багажник машины изменились (DoorBody), docs/SYNC_MAP.md
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
        PassengerPose = 25, // Client→Host: поза пассажира (PoseBody) — вместо ввода, docs/DECISIONS.md
        PassengerSave = 26,        // Client→Host: сейв кооп-слота с инвентарём пассажира; Host→Client: ответ на запрос (куски, как ZoneSave)
        PassengerSaveRequest = 28, // Client→Host: пришли мой сейв (Have=1); Host→Client: у меня его нет (Have=0)
        QuirkSet = 34,     // Host→Client: все причуды машины Host (QuirkSetBody), RECON.md 6.30
        QuirkCure = 35,    // Client→Host: пассажир вылечил причуду (QuirkBody) — Host лечит у себя
        AnchorTaken = 36,  // оба направления: якорь взят из основания в этой точке (AnchorBody) — снять у себя
        WorldItem = 39,    // оба направления: игрок положил предмет на землю (WorldItemBody + путь архетипа + значения свойств)
        RadioTrack = 38,   // Host→Client: текущий трек радио (PartBody: вид file|stock + имя файла/Title), RECON.md 6.36
        QuirkKnow = 37,    // Client→Host: пассажир выяснил о причуде (QuirkKnowBody) — Host добавляет к своему знанию
        Ping = 40,    // Client→Host, Host отвечает тем же пакетом (эхо)
    };

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
    };

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
    };
    constexpr uint8_t IgnitionMax = 2;
    constexpr uint8_t PercentStep = 1; // изменение процента меньше шага не рассылаем

    // Нужно ли слать: зажигание и свет — сразу при любом изменении, проценты — когда ушли на шаг и больше
    inline bool systems_changed(const SystemsBody& sent, const SystemsBody& now)
    {
        auto apart = [](uint8_t a, uint8_t b) { return (a > b ? a - b : b - a) >= PercentStep; };
        return sent.Ignition != now.Ignition || sent.Lights != now.Lights || sent.Radio != now.Radio || apart(sent.Fuel, now.Fuel) || apart(sent.Battery, now.Battery);
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
        QuirkBehaviorPart = 8, QuirkBehaviorType = 16;  // QuirkBehavior.PartKnown / TypeKnown
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
