#include "StatusFile.h"

#include <atomic>
#include <chrono>
#include <format>

#include "DoorSync.h"
#include "PassengerSaveSync.h"
#include "SystemsSync.h"

namespace StatusFile
{
    namespace
    {
        constexpr int64_t IntervalMs = 1000;
        int64_t g_last_ms = 0, g_written_ms = 0;
        constexpr int64_t HeartbeatMs = 3000; // лаунчер считает файл свежим до 5 с
        std::string g_last_body;              // содержимое без строки updated
        std::fstream g_out;
        int64_t g_session_at = 0;                  // unix-время подключения, 0 — нет связи
        std::atomic<int64_t> g_world_saved_at{0}; // unix-время последнего сейва зоны (Host записал / Client принял)

        int64_t unix_now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    } // namespace

    void mark_world_saved() { g_world_saved_at = unix_now(); }

    void write_fixed(std::fstream& out, const std::filesystem::path& file, std::string text, size_t size)
    {
        // Длина фиксирована: хвост добиваем пустыми строками, читатель не увидит обрезанного
        if (text.size() < size) text.append(size - text.size(), '\n');
        if (!out.is_open())
        {
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) std::ofstream(file, std::ios::binary).put('\n');
            out.open(file, std::ios::in | std::ios::out | std::ios::binary);
        }
        out.clear();
        out.seekp(0);
        out.write(text.data(), static_cast<std::streamsize>(text.size())); // нет папки/места — файл не критичен, молча пропускаем
        out.flush();
        if (!out) out.close(); // открыть заново в следующий раз
    }

    void tick(const std::filesystem::path& file, const std::string& version, Net::Role role, const std::string& phase)
    {
        using namespace std::chrono;
        int64_t now_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        if (now_ms - g_last_ms < IntervalMs) return;
        g_last_ms = now_ms;

        bool connected = Net::connected();
        if (!connected) g_session_at = 0;
        else if (!g_session_at) g_session_at = unix_now();

        std::string text = std::format("version={}\nrole={}\nconnected={}\n", version, role == Net::Role::Host ? "host" : role == Net::Role::Client ? "client" : "off",
                                       connected ? 1 : 0);
        text += "phase=" + phase + "\n";
        // Время — абсолютное (unix): файл не меняется каждую секунду, HUD считает «сколько прошло» сам
        if (g_session_at) text += std::format("session_at={}\n", g_session_at);
        if (int64_t saved = g_world_saved_at) text += std::format("world_saved_at={}\n", saved);
        if (int percent = Net::zone_progress(); percent >= 0) text += std::format("recv_pct={}\n", percent);
        if (PassengerSaveSync::restore_pending()) text += "restoring=1\n";
        if (connected) text += std::format("rtt_ms={}\nloss_pct={:.1f}\n", Net::rtt_ms(), Net::loss_percent());
        if (int doors = DoorSync::known_mask(); doors >= 0) text += std::format("doors={}\n", doors); // биты 0..5: открыта ли дверь
        if (auto* s = SystemsSync::last())
            text += std::format("ignition={}\nlights={}\nfuel_pct={}\nbattery_pct={}\n", s->Ignition, s->Lights, s->Fuel, s->Battery);

        // Запись у антивируса не бесплатна: пишем, только если что-то изменилось или пора подтвердить, что игра жива
        if (text == g_last_body && now_ms - g_written_ms < HeartbeatMs) return;
        g_last_body = text;
        g_written_ms = now_ms;
        write_fixed(g_out, file, std::format("updated={}\n", unix_now()) + text, 1024);
    }
} // namespace StatusFile
