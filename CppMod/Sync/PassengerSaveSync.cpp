#include "PassengerSaveSync.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include <chrono>
#include <fstream>

using namespace RC;

// shortcut: шлём весь кооп-слот (~3 МБ), а не только объекты инвентаря; заменить, когда понадобится слать чаще (SaveInventory::find даёт объекты).
namespace PassengerSaveSync
{
    namespace
    {
        constexpr uint64_t PollMs = 2000;
        constexpr uint64_t RestoreWaitMs = 20'000;
        constexpr auto SettleTime = std::chrono::seconds(3); // файл моложе — игра могла не дописать

        const Net::Events* g_events = nullptr;
        std::filesystem::path g_stored;
        bool g_request_sent = false;
        uint64_t g_pending_until_ms = 0, g_last_poll_ms = 0;
        std::filesystem::file_time_type g_last_sent{};

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        std::vector<uint8_t> read_file(const std::filesystem::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        }
    } // namespace

    void init(const Net::Events* events, const std::filesystem::path& stored_file)
    {
        g_events = events;
        g_stored = stored_file;
    }

    const std::filesystem::path& stored_file() { return g_stored; }

    void on_connected(bool is_host)
    {
        if (is_host) return;
        g_request_sent = false;
        g_pending_until_ms = 0;
    }

    bool restore_pending() { return g_pending_until_ms && now_ms() < g_pending_until_ms; }

    void on_save(std::vector<uint8_t>&& file, bool is_host)
    {
        g_pending_until_ms = 0; // Client: ответ пришёл
        // пишем рядом и переименовываем: оборванный приём не должен затереть прошлую копию
        auto tmp = g_stored;
        tmp += ".tmp";
        std::error_code ec;
        std::filesystem::create_directories(g_stored.parent_path(), ec);
        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
            if (!out)
            {
                Output::send<LogLevel::Error>(STR("[PassengerSave] не удалось записать {}\n"), tmp.wstring());
                return;
            }
        }
        std::filesystem::rename(tmp, g_stored, ec);
        if (ec) Output::send<LogLevel::Error>(STR("[PassengerSave] не удалось заменить {}\n"), g_stored.wstring());
        else Output::send<LogLevel::Verbose>(STR("[PassengerSave] {}: сейв пассажира сохранён, {} байт\n"), is_host ? STR("Host") : STR("получен от Host"), file.size());
    }

    void on_request(const Coop::SaveRequestBody& body, bool is_host)
    {
        if (!g_events) return;
        if (!is_host)
        {
            if (!body.Have) // Host ответил «нет сохранённого» — грузим зону как есть
            {
                g_pending_until_ms = 0;
                Output::send<LogLevel::Verbose>(STR("[PassengerSave] у Host нет сохранённого инвентаря — начинаем с инвентаря зоны\n"));
            }
            return;
        }
        auto file = read_file(g_stored);
        if (file.empty())
        {
            Net::send_passenger_request({0});
            Output::send<LogLevel::Verbose>(STR("[PassengerSave] Client просит сейв, сохранённого нет\n"));
            return;
        }
        Net::send_passenger_save(file, *g_events);
    }

    void tick(bool is_host, std::filesystem::path (*slot_file)())
    {
        if (is_host || !g_events || !Net::connected()) return;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs) return;
        g_last_poll_ms = now;

        std::error_code ec;
        auto path = slot_file();
        if (path.empty() || !std::filesystem::exists(path, ec))
        {
            if (!g_request_sent) // слота нет — просим копию у Host-а, пока зона не загружена (ZoneLoad ждёт restore_pending)
            {
                g_request_sent = true;
                g_pending_until_ms = now + RestoreWaitMs;
                Net::send_passenger_request({1});
                Output::send<LogLevel::Verbose>(STR("[PassengerSave] своего слота нет — прошу у Host-а сохранённый сейв\n"));
            }
            return;
        }
        auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec || mtime == g_last_sent || std::filesystem::file_time_type::clock::now() - mtime < SettleTime) return;
        auto file = read_file(path);
        if (file.empty()) return;
        g_last_sent = mtime;
        Output::send<LogLevel::Verbose>(STR("[PassengerSave] слот изменился — отправляю Host-у на хранение\n"));
        Net::send_passenger_save(file, *g_events);
    }
} // namespace PassengerSaveSync
