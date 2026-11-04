// Pacific Drive Coop — C++ часть (спека, раздел 5).
// Этап 0: ZoneSave (F10, Host сохраняет зону) + ZoneLoad (F11, Client загружает её) — риск 6.2, docs/SEED_SYNC.md.
#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include "Game/ItemProps.h"
#include "Game/ObjectDump.h"
#include "Game/SaveInventory.h"
#include "Game/StatusOverlay.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/CarSync.h"
#include "Sync/DoorSync.h"
#include "Sync/PassengerSaveSync.h"
#include "Sync/StatusFile.h"
#include "Sync/SystemsSync.h"
#include "Sync/GarageSync.h"
#include "Sync/PartsSync.h"
#include "Sync/TrunkSync.h"
#include "Sync/LootSync.h"
#include "Sync/PassengerSync.h"
#include "Sync/Presence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

Net::Config g_net_config;
Net::Events g_net_events; // колбэки заполняются в on_unreal_init
std::atomic<bool> g_selftest_item_props{false}; // Ctrl+F9: записать−прочитать−вернуть значения свойств предметов рюкзака
std::atomic<bool> g_dump_object{false}; // Ctrl+F5: снимок объекта под прицелом (JSON-файл)
std::atomic<bool> g_dump_tintable{false}; // Ctrl+F4: поля IP_Tintable (краска, оклейка) у дверей в лог
std::atomic<bool> g_dump_item_props{false}; // Ctrl+F8: значения свойств предметов рюкзака в лог (читаются в игровом тике)

// ZoneSave: Host сохраняет текущую зону штатным UBaseSaveGameManager::SaveGame (как при выходе в меню: "SAVE V4: Map").
// Этап 0 — проверить, что вызов из мода пишет полную сцену; дальше этот файл уйдёт клиенту по ENet.
namespace ZoneSave
{
    constexpr uint8_t SaveTypeManual = 1; // ESaveType::MANUAL
    std::atomic<bool> g_requested{false};
    bool g_waiting = false;
    int32_t g_slot = -1;
    Hook::GlobalCallbackId g_tick_id{};
    const std::filesystem::path CoopDir = L"ue4ss/Mods/PacificDriveCoop/coop"; // рабочая папка игры — Binaries/Win64
    const std::filesystem::path ZoneFile = CoopDir / "zone.sav";

    UObject* manager() { return UObjectGlobals::FindFirstOf(STR("BaseSaveGameManager")); }

    std::filesystem::path slot_path(int32_t slot)
    {
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        return std::filesystem::path(local ? local : L"") / std::format(L"PenDriverPro/Saved/SaveGames/odsave{}.sav", slot);
    }

    void save()
    {
        auto* mgr = manager();
        auto* slot = mgr ? mgr->GetValuePtrByPropertyNameInChain<int32_t>(STR("CurrentSaveSlot")) : nullptr;
        UFunctionCall fn(mgr, STR("SaveGame"));
        auto* p_slot = fn ? fn.param<int32_t>(STR("Slot")) : nullptr;
        auto* p_type = fn ? fn.param<uint8_t>(STR("SaveGameType")) : nullptr;
        if (!slot || !p_slot || !p_type)
        {
            Output::send<LogLevel::Error>(STR("[ZoneSave] нет BaseSaveGameManager/SaveGame/параметров — сохранение НЕ вызвано\n"));
            return;
        }
        // Содержимое ящиков «при открытии» — разыграть до сейва, иначе у Client оно будет своё (docs/DECISIONS.md)
        if (g_net_config.role == Net::Role::Host) LootSync::preroll_containers();
        // Пишем в текущий слот игрока — ровно то же, что делает игра при выходе в меню
        *p_slot = g_slot = *slot;
        *p_type = SaveTypeManual;
        fn.call();
        g_waiting = true;
        Output::send<LogLevel::Verbose>(STR("[ZoneSave] SaveGame(slot={}) вызван, ждём конца асинхронной записи\n"), g_slot);
    }

    bool async_save_in_progress()
    {
        UFunctionCall fn(manager(), STR("IsAsyncSaveInProgress"));
        auto* ret = fn ? fn.param<bool>(STR("ReturnValue")) : nullptr;
        if (!ret) return false;
        fn.call();
        return *ret;
    }

    // Игровой поток (EngineTick): UFunction вызываем только отсюда, не из потока клавиш UE4SS
    void tick()
    {
        if (g_requested.exchange(false)) save();
        if (!g_waiting || async_save_in_progress()) return;
        g_waiting = false;
        std::error_code ec;
        auto path = slot_path(g_slot);
        auto size = std::filesystem::file_size(path, ec);
        Output::send<LogLevel::Verbose>(STR("[ZoneSave] готово: {} ({} байт){}\n"), path.wstring(), ec ? 0 : size, ec ? STR(" — ФАЙЛ НЕ НАЙДЕН") : STR(""));
        if (ec) return;
        // Копия в папку мода — для ручного F11 на этом же ПК и для отладки
        std::filesystem::create_directories(CoopDir, ec);
        std::filesystem::copy_file(path, ZoneFile, std::filesystem::copy_options::overwrite_existing, ec);
        Output::send<LogLevel::Verbose>(STR("[ZoneSave] копия: {}{}\n"), ZoneFile.wstring(), ec ? STR(" — ОШИБКА КОПИРОВАНИЯ") : STR(""));
        if (Net::connected())
        {
            std::ifstream f(path, std::ios::binary);
            Net::send_zone_save(std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}), g_net_events);
        }
    }
} // namespace ZoneSave

// ZoneLoad: роль клиента — загрузить зону из полученного файла (сейчас coop/zone.sav) штатным UserLoadGame.
namespace ZoneLoad
{
    constexpr uint8_t PlayModeStory = 1;    // EGamePlayMode::STORY
    constexpr int32_t MainFileNotBackup = -1; // BackupVersion < 0 → основной файл слота (дизассемблер exe+0x1038000)
    constexpr int32_t FirstSlot = 1, LastSlot = 9; // имя слота строится только для 1..9 (exe+0x106D770), 0 = служебный odsave_scratch
    const std::filesystem::path SlotFile = ZoneSave::CoopDir / "slot.txt";
    std::atomic<bool> g_requested{false};
    std::atomic<uint64_t> g_loading_since_ms{0}; // UserLoadGame вызван, карта ещё не загрузилась (надпись поверх игры); 0 — нет

    uint64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Слот из slot.txt без побочных эффектов; 0 — кооп-слот ещё не назначен
    int32_t current_slot()
    {
        int32_t slot = 0;
        if (FILE* f = _wfopen(SlotFile.c_str(), L"r"))
        {
            if (fscanf_s(f, "%d", &slot) != 1) slot = 0;
            fclose(f);
        }
        return slot >= FirstSlot && slot <= LastSlot ? slot : 0;
    }

    // Файл кооп-слота или пустой путь, если слот не назначен — для PassengerSaveSync
    std::filesystem::path current_slot_file()
    {
        int32_t slot = current_slot();
        return slot ? ZoneSave::slot_path(slot) : std::filesystem::path{};
    }

    // Кооп-слот: записанный в slot.txt, иначе первый свободный — чужой сейв не затираем никогда
    int32_t coop_slot()
    {
        if (int32_t existing = current_slot()) return existing;
        for (int32_t slot = FirstSlot; slot <= LastSlot; ++slot)
        {
            if (std::filesystem::exists(ZoneSave::slot_path(slot))) continue;
            if (FILE* f = _wfopen(SlotFile.c_str(), L"w"))
            {
                fprintf(f, "%d\n", slot);
                fclose(f);
                return slot;
            }
            return 0;
        }
        return 0;
    }

    std::vector<uint8_t> read_file(const std::filesystem::path& path)
    {
        std::ifstream f(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    std::wstring from_utf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())).wstring(); }

    // zone.sav с инвентарём из текущего файла кооп-слота; пусто — переноса нет (слота ещё нет или ошибка)
    std::vector<uint8_t> zone_with_own_inventory(int32_t slot)
    {
        auto own = read_file(ZoneSave::slot_path(slot));
        auto zone = read_file(ZoneSave::ZoneFile);
        if (own.empty()) // другой ПК или слот удалён — берём копию, которую хранит Host (PassengerSaveSync)
        {
            own = read_file(PassengerSaveSync::stored_file());
            if (!own.empty()) Output::send<LogLevel::Verbose>(STR("[ZoneLoad] своего сейва в слоте {} нет — инвентарь из копии, которую хранит Host\n"), slot);
        }
        if (own.empty())
        {
            Output::send<LogLevel::Verbose>(STR("[ZoneLoad] своего сейва в слоте {} нет — инвентарь из присланной зоны\n"), slot);
            return {};
        }
        std::string error;
        std::vector<std::string> warnings;
        auto started = std::chrono::steady_clock::now();
        auto merged = SaveInventory::transfer_files(own, zone, error, warnings);
        if (!merged)
        {
            Output::send<LogLevel::Error>(STR("[ZoneLoad] перенос своего инвентаря НЕ выполнен ({}) — грузим зону как есть\n"), from_utf8(error));
            return {};
        }
        for (auto& w : warnings) Output::send<LogLevel::Warning>(STR("[ZoneLoad] {}\n"), from_utf8(w));
        Output::send<LogLevel::Verbose>(STR("[ZoneLoad] свой инвентарь из слота {} перенесён в присланную зону ({} мс)\n"), slot,
                                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        return std::move(*merged);
    }

    void load()
    {
        int32_t slot = coop_slot();
        if (!slot)
        {
            Output::send<LogLevel::Error>(STR("[ZoneLoad] нет свободного слота 1..9 под кооп — освободите слот в игре\n"));
            return;
        }
        if (!std::filesystem::exists(ZoneSave::ZoneFile))
        {
            Output::send<LogLevel::Error>(STR("[ZoneLoad] нет {} — нечего загружать\n"), ZoneSave::ZoneFile.wstring());
            return;
        }
        // Свой инвентарь: берём из кооп-слота Client до его удаления и переносим в присланную зону (DECISIONS.md 2026-10-30).
        // Любая ошибка — грузим зону как есть: зона важнее инвентаря
        std::vector<uint8_t> merged = zone_with_own_inventory(slot);

        // Игра держит прочитанные слоты в памяти (меню читает все): без удаления слота подменённый файл игнорируется и
        // грузится старая зона (тест 2026-10-27). Слот наш (coop/slot.txt) — удалить можно
        UFunctionCall delete_slot(ZoneSave::manager(), STR("DeleteGame"));
        auto* d_slot = delete_slot ? delete_slot.param<int32_t>(STR("Slot")) : nullptr;
        auto* d_backups = delete_slot ? delete_slot.param<bool>(STR("bDeleteBackups")) : nullptr;
        if (d_slot && d_backups)
        {
            *d_slot = slot;
            *d_backups = true;
            delete_slot.call();
        }
        std::error_code ec;
        if (!merged.empty())
            std::ofstream(ZoneSave::slot_path(slot), std::ios::binary).write(reinterpret_cast<const char*>(merged.data()), static_cast<std::streamsize>(merged.size()));
        else
            std::filesystem::copy_file(ZoneSave::ZoneFile, ZoneSave::slot_path(slot), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec || !std::filesystem::exists(ZoneSave::slot_path(slot)))
        {
            Output::send<LogLevel::Error>(STR("[ZoneLoad] не удалось скопировать {} в слот {} (нет файла? сначала F10)\n"), ZoneSave::ZoneFile.wstring(), slot);
            return;
        }
        auto* mgr = ZoneSave::manager();
        // Игра кэширует метаданные слотов — после подмены файла кэш устарел
        UFunctionCall invalidate(mgr, STR("InvalidateMetadataCacheSlot"));
        if (auto* p = invalidate ? invalidate.param<int32_t>(STR("Slot")) : nullptr)
        {
            *p = slot;
            invalidate.call();
        }
        UFunctionCall fn(mgr, STR("UserLoadGame"));
        auto* p_slot = fn ? fn.param<int32_t>(STR("Slot")) : nullptr;
        auto* p_mode = fn ? fn.param<uint8_t>(STR("Mode")) : nullptr;
        auto* p_backup = fn ? fn.param<int32_t>(STR("BackupVersion")) : nullptr;
        if (!p_slot || !p_mode || !p_backup)
        {
            Output::send<LogLevel::Error>(STR("[ZoneLoad] нет UserLoadGame/параметров — загрузка НЕ вызвана\n"));
            return;
        }
        *p_slot = slot;
        *p_mode = PlayModeStory;
        *p_backup = MainFileNotBackup;
        // Загрузка уровня блокирует игровой поток ~10 с — надпись поднимаем до вызова (окно живёт в своём потоке)
        g_loading_since_ms = now_ms();
        if (g_net_config.status_overlay) StatusOverlay::set(L"Co-op: loading the host's world - please wait...");
        fn.call();
        auto* ret = fn.param<bool>(STR("ReturnValue"));
        if (ret && !*ret) g_loading_since_ms = 0; // игра отказала — карта грузиться не будет
        Output::send<LogLevel::Verbose>(STR("[ZoneLoad] UserLoadGame(slot={}) → {}\n"), slot, ret ? (*ret ? STR("true") : STR("false")) : STR("?"));
    }

    void tick()
    {
        // Ждём ответ Host-а про сохранённый инвентарь (не дольше 20 с): иначе зона загрузится с чужим инвентарём и затрёт слот
        if (!PassengerSaveSync::restore_pending() && g_requested.exchange(false)) load();
    }
} // namespace ZoneLoad

// AutoSave: Host после каждой загрузки карты ждёт, пока спавн (USpawnHistory::IsSpawnSystemRunning) затихнет, и один раз
// сохраняет и шлёт клиенту. Спавн амортизирован и идёт секунды после загрузки — сохраняться раньше нельзя (docs/SEED_SYNC.md).
// Зона из сейва (продолжение заезда) спавна не запускает вовсе — поэтому критерий «тихо N опросов», а не «спавн закончился».
// Гараж тоже отправляется: клиент получает тот же гараж (SessionStart, docs/SAVES.md).
namespace AutoSave
{
    constexpr uint64_t PollMs = 500, MinAfterLoadMs = 3000;
    constexpr int QuietPollsNeeded = 4; // 2 с без спавна
    std::atomic<bool> g_map_loaded{false};
    bool g_waiting_quiet = false, g_zone_ready = false, g_error_logged = false;
    int g_quiet_polls = 0;
    uint64_t g_last_poll_ms = 0, g_map_loaded_ms = 0;

    std::optional<bool> spawn_running(UObject* world_context)
    {
        auto* cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/PenDriverPro.Default__SpawnHistory"));
        UFunctionCall fn(cdo, STR("IsSpawnSystemRunning"));
        auto* ctx = fn ? fn.param<UObject*>(STR("WorldContextObject")) : nullptr;
        auto* ret = fn ? fn.param<bool>(STR("ReturnValue")) : nullptr;
        if (!ctx || !ret) return std::nullopt;
        *ctx = world_context;
        fn.call();
        return *ret;
    }

    void tick()
    {
        using namespace std::chrono;
        uint64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        if (g_net_config.role != Net::Role::Host) return;
        if (g_map_loaded.exchange(false))
        {
            g_map_loaded_ms = now;
            g_waiting_quiet = true;
            g_zone_ready = false;
            g_quiet_polls = 0;
        }
        if (!g_waiting_quiet || now - g_last_poll_ms < PollMs || now - g_map_loaded_ms < MinAfterLoadMs) return;
        g_last_poll_ms = now;
        auto* character = UObjectGlobals::FindFirstOf(STR("BaseCharacter")); // любой актор мира годится как WorldContext; в меню его нет
        if (!character) return;
        auto running = spawn_running(character);
        if (!running)
        {
            if (!g_error_logged) Output::send<LogLevel::Error>(STR("[AutoSave] нет SpawnHistory::IsSpawnSystemRunning — автосохранение выключено, есть Ctrl+F10\n"));
            g_error_logged = true;
            return;
        }
        if (*running)
        {
            if (g_quiet_polls >= 0) Output::send<LogLevel::Verbose>(STR("[AutoSave] спавн зоны идёт...\n"));
            g_quiet_polls = -1; // лог один раз за цикл спавна
            return;
        }
        if (++g_quiet_polls < QuietPollsNeeded) return;
        g_waiting_quiet = false;
        g_zone_ready = true;
        Output::send<LogLevel::Verbose>(STR("[AutoSave] карта готова{}\n"), Net::connected() ? STR(" — сохраняю и отправляю") : STR(", клиента нет — отправлю при подключении"));
        if (Net::connected()) ZoneSave::g_requested = true;
    }

    void on_client_connected()
    {
        if (g_zone_ready) ZoneSave::g_requested = true;
    }
} // namespace AutoSave

std::string g_version; // ModVersion для окна статуса

// Надпись поверх игры (Game/StatusOverlay), текст для игрока — по-английски. Только Client: Host ничего не ждёт.
namespace Overlay
{
    constexpr uint64_t LoadingTimeoutMs = 90'000; // карта так и не загрузилась — не висим вечно
    constexpr uint64_t StartDelayMs = 3'000;      // окно-надпись не раньше, чем через 3 с после первой загруженной карты
    bool g_world_synced = false;                  // присланная зона загружена в этом подключении
    uint64_t g_ready_at_ms = 0;                   // 0 — карта ещё ни разу не загружалась

    void on_map_loaded()
    {
        if (!g_ready_at_ms) g_ready_at_ms = ZoneLoad::now_ms() + StartDelayMs;
        if (ZoneLoad::g_loading_since_ms.exchange(0)) g_world_synced = true;
    }

    std::wstring text()
    {
        if (g_net_config.role != Net::Role::Client || !g_net_config.status_overlay) return {};
        // Второй ПК упал при запуске игры, через ~2 с после загрузки мода (dxgi, RTSS в процессе; дамп 23:51): в первые секунды
        // игра ещё создаёт окно и устройство отрисовки — наше окно topmost и сообщения её окну в это время ни к чему
        if (!g_ready_at_ms || ZoneLoad::now_ms() < g_ready_at_ms) return {};
        if (!Net::connected()) g_world_synced = false;
        uint64_t since = ZoneLoad::g_loading_since_ms;
        if (since && ZoneLoad::now_ms() - since < LoadingTimeoutMs) return L"Co-op: loading the host's world - please wait...";
        if (int percent = Net::zone_progress(); percent >= 0) return std::format(L"Co-op: receiving the world from the host - {}%", percent);
        if (!Net::connected()) return L"Co-op: waiting for the host...";
        if (PassengerSaveSync::restore_pending()) return L"Co-op: restoring your inventory from the host - please wait...";
        if (!g_world_synced) return L"Co-op: connected - waiting for the host's world...";
        return {};
    }
} // namespace Overlay

class PacificDriveCoop : public CppUserModBase
{
public:
    PacificDriveCoop() : CppUserModBase()
    {
        ModName = STR("PacificDriveCoop");
        ModVersion = STR("0.16.3");
        for (auto c : ModVersion) g_version += static_cast<char>(c); // версия — ASCII
        ModDescription = STR("2-player LAN coop: driver + passenger");
        ModAuthors = STR("sanokoo");

        // Ctrl: голые F10/F11 жмут случайно (F11 — полный экран во многих программах), а F11 грузит кооп-слот
        register_keydown_event(Input::Key::F10, {Input::ModifierKey::CONTROL}, [] { ZoneSave::g_requested = true; });
        register_keydown_event(Input::Key::F11, {Input::ModifierKey::CONTROL}, [] { ZoneLoad::g_requested = true; });
        register_keydown_event(Input::Key::E, {Input::ModifierKey::CONTROL}, [] { PassengerSync::g_key_pressed = true; });
        register_keydown_event(Input::Key::F12, {Input::ModifierKey::CONTROL}, [] { LootSync::g_simulate_peer_pick = true; });
        register_keydown_event(Input::Key::F8, {Input::ModifierKey::CONTROL}, [] { g_dump_item_props = true; }); // диагностика свойств предметов рюкзака
        register_keydown_event(Input::Key::F4, {Input::ModifierKey::CONTROL}, [] { g_dump_tintable = true; });
        register_keydown_event(Input::Key::F5, {Input::ModifierKey::CONTROL}, [] { g_dump_object = true; });
        register_keydown_event(Input::Key::F9, {Input::ModifierKey::CONTROL}, [] { g_selftest_item_props = true; }); // самопроверка записи свойств
        Output::send<LogLevel::Verbose>(STR("[PacificDriveCoop] {} loaded, Ctrl+F10 = сохранить зону, Ctrl+F11 = загрузить зону (клиент), Ctrl+E = сесть/выйти (пассажир)\n"), ModVersion);
    }

    ~PacificDriveCoop() override
    {
        if (ZoneSave::g_tick_id) Hook::UnregisterCallback(ZoneSave::g_tick_id);
        if (m_load_map_id) Hook::UnregisterCallback(m_load_map_id);
        Net::stop();
        StatusOverlay::stop();
    }

    auto on_unreal_init() -> void override
    {
        g_net_events.log = [](const std::wstring& msg) { Output::send<LogLevel::Verbose>(STR("{}\n"), msg); };
        g_net_events.on_connected = [] {
            AutoSave::on_client_connected();
            DoorSync::on_connected(g_net_config.role == Net::Role::Host);
            SystemsSync::on_connected(g_net_config.role == Net::Role::Host);
            GarageSync::on_connected(g_net_config.role == Net::Role::Host);
            PartsSync::on_connected(g_net_config.role == Net::Role::Host);
            TrunkSync::on_connected(g_net_config.role == Net::Role::Host);
            PassengerSaveSync::on_connected(g_net_config.role == Net::Role::Host);
        };
        g_net_events.on_passenger_save = [](std::vector<uint8_t>&& file) { PassengerSaveSync::on_save(std::move(file), g_net_config.role == Net::Role::Host); };
        g_net_events.on_passenger_request = [](const Coop::SaveRequestBody& body) { PassengerSaveSync::on_request(body, g_net_config.role == Net::Role::Host); };
        PassengerSaveSync::init(&g_net_events, ZoneSave::CoopDir / "passenger.sav");
        g_net_events.on_systems = SystemsSync::on_systems;
        g_net_events.on_garage_door = [](Coop::PacketId id, const Coop::GarageDoorBody& body) { GarageSync::on_garage_door(id, body, g_net_config.role == Net::Role::Host); };
        g_net_events.on_part = PartsSync::on_part;
        g_net_events.on_part_request = PartsSync::on_part_request;
        g_net_events.on_trunk = [](Coop::PacketId id, uint16_t count, const std::string& path, const std::vector<Coop::TrunkValue>& values) {
            TrunkSync::on_packet(id, count, path, values, g_net_config.role == Net::Role::Host);
        };
        g_net_events.on_door = [](Coop::PacketId id, const Coop::DoorBody& body) { DoorSync::on_door(id, body, g_net_config.role == Net::Role::Host); };
        g_net_events.on_pose = Presence::on_pose;
        g_net_events.on_car_state = CarSync::on_car_state;
        g_net_events.on_loot = [](Coop::PacketId id, const Coop::LootBody& body) { LootSync::on_loot(id, body, g_net_config.role == Net::Role::Host); };
        g_net_events.on_seat = [](Coop::PacketId id, const Coop::SeatBody& body) { PassengerSync::on_seat(id, body, g_net_config.role == Net::Role::Host); };
        g_net_events.on_zone_save = [](std::vector<uint8_t>&& file) {
            std::error_code ec;
            std::filesystem::create_directories(ZoneSave::CoopDir, ec);
            std::ofstream(ZoneSave::ZoneFile, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
            ZoneLoad::g_requested = true; // клиент сразу грузит присланную зону
        };
        g_net_config = Net::load_config(ZoneSave::CoopDir.parent_path() / "coop.ini");
        PassengerSync::set_seat_offsets(g_net_config.seat_x, g_net_config.seat_y, g_net_config.seat_z, g_net_config.seat_right_x, g_net_config.seat_right_y,
                                        g_net_config.seat_right_z);
        PassengerSync::set_restrict_interactions(g_net_config.restrict_interactions);
        PassengerSync::set_exit_side_cm(g_net_config.exit_side_cm);
        PartsSync::set_client_parts(g_net_config.client_parts);
        if (g_net_config.role == Net::Role::Off)
            Output::send<LogLevel::Verbose>(STR("[Net] выключено (нет role=host|client в Mods\\PacificDriveCoop\\coop.ini)\n"));
        else
            Net::start(g_net_config, g_net_events);

        ZoneSave::g_tick_id = Hook::RegisterEngineTickPreCallback(
                [](Hook::TCallbackIterationData<void>&, UEngine*, float delta_seconds, bool) {
                    Net::tick(g_net_events);
                    Presence::tick(delta_seconds);
                    CarSync::tick(delta_seconds, g_net_config.role == Net::Role::Host);
                    PassengerSync::tick(g_net_config.role == Net::Role::Host);
                    LootSync::tick(g_net_config.role == Net::Role::Host);
                    DoorSync::tick(g_net_config.role == Net::Role::Host);
                    SystemsSync::tick(g_net_config.role == Net::Role::Host);
                    GarageSync::tick(g_net_config.role == Net::Role::Host);
                    PartsSync::tick(g_net_config.role == Net::Role::Host);
                    TrunkSync::tick(g_net_config.role == Net::Role::Host);
                    PassengerSaveSync::tick(g_net_config.role == Net::Role::Host, ZoneLoad::current_slot_file);
                    StatusFile::tick(ZoneSave::CoopDir / "status.txt", g_version, g_net_config.role);
                    if (g_dump_item_props.exchange(false)) ItemProps::dump_backpack();
                    if (g_selftest_item_props.exchange(false)) ItemProps::selftest_write();
                    if (g_dump_tintable.exchange(false)) ItemProps::dump_tintable();
                    if (g_dump_object.exchange(false)) ObjectDump::dump_target();
                    StatusOverlay::set(Overlay::text());
                    AutoSave::tick();
                    ZoneSave::tick();
                    ZoneLoad::tick();
                },
                {false, false, STR("PacificDriveCoop"), STR("ZoneSave")});
        m_load_map_id = Hook::RegisterLoadMapPostCallback(
                [](Hook::TCallbackIterationData<bool>&, UEngine*, FWorldContext&, FURL, UPendingNetGame*, FString&) {
                    AutoSave::g_map_loaded = true;
                    Overlay::on_map_loaded();
                    Presence::on_map_loaded();
                    CarSync::on_map_loaded();
                    PassengerSync::on_map_loaded();
                    LootSync::on_map_loaded();
                    DoorSync::on_map_loaded();
                    SystemsSync::on_map_loaded();
                    GarageSync::on_map_loaded();
                    PartsSync::on_map_loaded();
                    TrunkSync::on_map_loaded();
                },
                {false, true, STR("PacificDriveCoop"), STR("AutoSave")});
        Output::send<LogLevel::Verbose>(STR("[PacificDriveCoop] хуки: EngineTick {}, LoadMap {}\n"), ZoneSave::g_tick_id ? STR("есть") : STR("НЕТ"),
                                        m_load_map_id ? STR("есть") : STR("НЕТ"));
    }

private:
    Hook::GlobalCallbackId m_load_map_id{};
};

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new PacificDriveCoop();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
