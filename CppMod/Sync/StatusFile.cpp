#include "StatusFile.h"

#include <chrono>
#include <format>
#include <fstream>

#include "DoorSync.h"
#include "SystemsSync.h"

namespace StatusFile
{
    namespace
    {
        constexpr int64_t IntervalMs = 1000;
        int64_t g_last_ms = 0, g_written_ms = 0;
        constexpr int64_t HeartbeatMs = 3000; // лаунчер считает файл свежим до 5 с
        std::string g_last_body;              // содержимое без строки updated
    } // namespace

    void tick(const std::filesystem::path& file, const std::string& version, Net::Role role, const std::string& phase)
    {
        using namespace std::chrono;
        int64_t now_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        if (now_ms - g_last_ms < IntervalMs) return;
        g_last_ms = now_ms;

        std::string text = std::format("version={}\nrole={}\nconnected={}\n", version, role == Net::Role::Host ? "host" : role == Net::Role::Client ? "client" : "off",
                                       Net::connected() ? 1 : 0);
        text += "phase=" + phase + "\n";
        if (Net::connected()) text += std::format("rtt_ms={}\nloss_pct={:.1f}\n", Net::rtt_ms(), Net::loss_percent());
        if (int doors = DoorSync::known_mask(); doors >= 0) text += std::format("doors={}\n", doors); // биты 0..5: открыта ли дверь
        if (auto* s = SystemsSync::last())
            text += std::format("ignition={}\nlights={}\nfuel_pct={}\nbattery_pct={}\n", s->Ignition, s->Lights, s->Fuel, s->Battery);

        // Запись свежего файла у антивируса стоит ~10 мс: пишем, только если что-то изменилось или пора подтвердить, что игра жива
        if (text == g_last_body && now_ms - g_written_ms < HeartbeatMs) return;
        g_last_body = text;
        g_written_ms = now_ms;
        text = std::format("updated={}\n", duration_cast<seconds>(system_clock::now().time_since_epoch()).count()) + text;

        // Пишем в тот же файл на месте, без усечения (новый файл Windows проверяет ~10-25 мс при открытии, HUD держит этот файл открытым).
        // Длина фиксирована: хвост добиваем пустыми строками, читатель не увидит обрезанного
        constexpr size_t FileSize = 512;
        if (text.size() < FileSize) text.append(FileSize - text.size(), '\n');
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) std::ofstream(file, std::ios::binary).put('\n');
        std::fstream out(file, std::ios::in | std::ios::out | std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size())); // нет папки/места — статус не критичен, молча пропускаем
    }
} // namespace StatusFile
