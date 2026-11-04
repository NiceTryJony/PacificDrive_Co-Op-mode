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
        int64_t g_last_ms = 0;
    } // namespace

    void tick(const std::filesystem::path& file, const std::string& version, Net::Role role)
    {
        using namespace std::chrono;
        int64_t now_ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        if (now_ms - g_last_ms < IntervalMs) return;
        g_last_ms = now_ms;

        std::string text = std::format("updated={}\nversion={}\nrole={}\nconnected={}\n", duration_cast<seconds>(system_clock::now().time_since_epoch()).count(), version,
                                       role == Net::Role::Host ? "host" : role == Net::Role::Client ? "client" : "off", Net::connected() ? 1 : 0);
        if (Net::connected()) text += std::format("rtt_ms={}\nloss_pct={:.1f}\n", Net::rtt_ms(), Net::loss_percent());
        if (int doors = DoorSync::known_mask(); doors >= 0) text += std::format("doors={}\n", doors); // биты 0..5: открыта ли дверь
        if (auto* s = SystemsSync::last())
            text += std::format("ignition={}\nlights={}\nfuel_pct={}\nbattery_pct={}\n", s->Ignition, s->Lights, s->Fuel, s->Battery);

        // пишем рядом и переименовываем: консоль не должна прочитать недописанное
        auto tmp = file;
        tmp += ".tmp";
        std::error_code ec;
        {
            std::ofstream out(tmp, std::ios::binary);
            out << text;
            if (!out) return; // нет папки/места — статус не критичен, молча пропускаем
        }
        std::filesystem::rename(tmp, file, ec);
    }
} // namespace StatusFile
