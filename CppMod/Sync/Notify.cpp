#include "Notify.h"

#include <fstream>

namespace Notify
{
    namespace
    {
        std::filesystem::path g_file;
        bool g_was_connected = false;
        int g_doors = -1;
    } // namespace

    void init(const std::filesystem::path& file)
    {
        g_file = file;
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream(file, std::ios::binary | std::ios::trunc);
    }

    void push(Kind kind, const std::string& text)
    {
        if (g_file.empty()) return;
        std::ofstream out(g_file, std::ios::binary | std::ios::app); // события редкие — открывать на каждое дёшево
        out << (kind == Kind::Warn ? "!" : kind == Kind::Error ? "-" : "") << text << "\n";
    }

    void tick(bool connected, bool host, int doors)
    {
        // двери: сообщаем только при живой связи и только о смене уже известного состояния; одинаковые строки HUD склеивает сам ("x3")
        if (connected && g_was_connected && doors >= 0 && g_doors >= 0 && doors != g_doors)
        {
            int opened = doors & ~g_doors, closed = g_doors & ~doors;
            for (int i = 0; i < 6; ++i)
            {
                if (opened & (1 << i)) push(Kind::Info, "A door was opened");
                if (closed & (1 << i)) push(Kind::Info, "A door was closed");
            }
        }
        g_doors = doors;
        if (connected == g_was_connected) return;
        g_was_connected = connected;
        if (connected) push(Kind::Info, host ? "Your passenger joined" : "Connected to the driver");
        else push(Kind::Warn, host ? "Your passenger left" : "Lost connection to the driver");
    }
} // namespace Notify
