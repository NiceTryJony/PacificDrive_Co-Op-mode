#include "RadioSync.h"

#include <DynamicOutput/DynamicOutput.hpp>

#include "Networking/Net.h"

#include <Unreal/UFunction.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <cctype>
#include <chrono>
#include <fstream>

using namespace RC;
using namespace RC::Unreal;

namespace RadioSync
{
    namespace
    {
        constexpr int64_t PollMs = 250, ResendMs = 10'000; // повтор нужен: Client мог подключиться позже или сменить карту
        std::filesystem::path g_host_file, g_cmd_file;
        std::streamoff g_offset = 0;
        std::string g_kind, g_name, g_last_cmd;
        int64_t g_poll_ms = 0, g_sent_ms = 0;
        bool g_was_connected = false, g_dirty = false;
        bool g_noseek = true; // coop.ini radio_noseek=0 выключает

        int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        std::string g_waiting, g_waiting_name; // Client: трек Хоста, файла которого у нас ещё нет
        int64_t g_waiting_check_ms = 0;

        // Папка, откуда Client играет треки Хоста: лаунчер пишет путь в coop\music_dir.txt (UTF-8, прямые слэши); нет файла —
        // %LOCALAPPDATA%\PenDriverPro\Custom\Radio (без лаунчера и у Host)
        std::filesystem::path music_dir()
        {
            std::ifstream f(g_host_file.parent_path() / "music_dir.txt", std::ios::binary);
            std::string d;
            if (f && std::getline(f, d))
            {
                while (!d.empty() && (d.back() == '\r' || d.back() == '/' || d.back() == ' ')) d.pop_back();
                if (d.size() > 2 && d[1] == ':') return std::filesystem::path(reinterpret_cast<const char8_t*>(d.c_str()));
            }
            const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
            return local ? std::filesystem::path(local) / L"PenDriverPro" / L"Custom" / L"Radio" : std::filesystem::path();
        }

        // Есть ли файл трека в этой папке. Имя — хвост ExternalUrl Хоста в UTF-8, возможны %XX
        bool have_track(const std::string& name)
        {
            std::string plain;
            for (size_t i = 0; i < name.size(); ++i)
            {
                if (name[i] == '%' && i + 2 < name.size() && std::isxdigit(static_cast<unsigned char>(name[i + 1])) && std::isxdigit(static_cast<unsigned char>(name[i + 2])))
                {
                    plain += static_cast<char>(std::stoi(name.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else plain += name[i];
            }
            auto dir = music_dir();
            if (dir.empty() || plain.empty() || plain.find_first_of("/\\") != std::string::npos) return false;
            std::error_code ec;
            return std::filesystem::exists(dir / std::filesystem::path(reinterpret_cast<const char8_t*>(plain.c_str())), ec);
        }
    } // namespace

    void init(const std::filesystem::path& coop_dir)
    {
        g_host_file = coop_dir / "radio_host.txt";
        g_cmd_file = coop_dir / "radio_cmd.txt";
        std::error_code ec;
        std::filesystem::create_directories(coop_dir, ec);
        std::ofstream(g_host_file, std::ios::trunc); // начинаем с пустых файлов: старые строки прошлого запуска не нужны
        std::ofstream(g_cmd_file, std::ios::trunc);
        std::ifstream ini(coop_dir.parent_path() / "coop.ini");
        for (std::string line; std::getline(ini, line);)
            if (line.rfind("radio_noseek", 0) == 0 && line.find('0') != std::string::npos && line.find('=') != std::string::npos) g_noseek = false;
    }

    namespace
    {
        bool g_seek_patched = false, g_start_patched = false;
        int64_t g_patch_try_ms = 0;

        // Тело Blueprint-функции делаем пустым: в её байткоде лежит TArray<uint8> Script, последний байт — EX_EndOfScript (0x53).
        // Смещение не зашиваем, а ищем сканом по самой функции; нашли — первые три байта = EX_Return, EX_Nothing, EX_EndOfScript.
        // Не нашли (версия игры другая) — ничего не трогаем
        bool make_noop(UFunction* fn)
        {
            struct Script
            {
                uint8_t* Data;
                int32_t Num;
                int32_t Max;
            };
            auto* base = reinterpret_cast<uint8_t*>(fn);
            for (size_t off = 0x30; off <= 0xA0; off += 8)
            {
                auto* s = reinterpret_cast<Script*>(base + off);
                if (!s->Data || s->Num < 3 || s->Num > 65536 || s->Max < s->Num || s->Data[s->Num - 1] != 0x53) continue;
                s->Data[0] = 0x04;
                s->Data[1] = 0x0B;
                s->Data[2] = 0x53;
                return true;
            }
            return false;
        }

        bool patch(const TCHAR* object_class, const TCHAR* function_name, bool& done)
        {
            if (done) return true;
            UObject* object = UObjectGlobals::FindFirstOf(object_class);
            UFunction* fn = object ? object->GetFunctionByNameInChain(function_name) : nullptr;
            if (!fn) return false;
            done = true; // один раз: нашли функцию — больше не пытаемся при любом исходе
            bool ok = make_noop(fn);
            if (ok) Output::send<LogLevel::Verbose>(STR("[Radio] случайный старт трека ({}) отключён\n"), function_name);
            else Output::send<LogLevel::Warning>(STR("[Radio] случайный старт трека ({}): байткод не найден, оставлен как есть\n"), function_name);
            return true;
        }
    } // namespace

    void tick(bool is_host)
    {
        // Игра начинает каждый трек со случайной позиции (RandomSeekMusic + RandomizeStartTime на каждый трек, лог 2026-11-06 23:49),
        // у двух ПК позиции разные — выключаем у обоих, тогда треки идут с 0:00 и расходятся лишь на время доставки пакета
        if (g_noseek && !(g_seek_patched && g_start_patched) && now_ms() - g_patch_try_ms > 2000)
        {
            g_patch_try_ms = now_ms();
            patch(STR("BP_MusicManager_C"), STR("RandomSeekMusic"), g_seek_patched);
            patch(STR("BP_RadioComponent_C"), STR("RandomizeStartTime"), g_start_patched);
        }
        if (!is_host)
        {
            // Лаунчер докачивает треки Хоста в фоне — появился файл ожидаемого трека, отдаём его Lua
            if (!g_waiting.empty() && now_ms() - g_waiting_check_ms > 1000)
            {
                g_waiting_check_ms = now_ms();
                if (have_track(g_waiting_name))
                {
                    std::string line = g_waiting;
                    g_waiting.clear();
                    g_last_cmd = line;
                    std::ofstream(g_cmd_file, std::ios::app | std::ios::binary) << line << '\n';
                }
            }
            return;
        }
        int64_t now = now_ms();
        if (now - g_poll_ms < PollMs) return;
        g_poll_ms = now;

        // Новые строки radio_host.txt (дописывает Lua); последняя строка — текущий трек
        std::ifstream in(g_host_file, std::ios::binary);
        if (in)
        {
            in.seekg(g_offset);
            for (std::string line; std::getline(in, line);)
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                auto bar = line.find('|');
                if (bar == std::string::npos || bar == 0 || bar + 1 >= line.size()) continue;
                g_kind = line.substr(0, bar);
                g_name = line.substr(bar + 1);
                g_dirty = true;
            }
            in.clear();
            g_offset = in.tellg() == std::streampos(-1) ? g_offset : static_cast<std::streamoff>(in.tellg());
        }

        bool connected = Net::connected();
        if (!connected || g_kind.empty())
        {
            g_was_connected = connected;
            return;
        }
        if (g_dirty || !g_was_connected || now - g_sent_ms > ResendMs)
        {
            if (Net::send_part(g_kind, g_name, Coop::RadioTrack))
            {
                if (g_dirty || !g_was_connected) Output::send<LogLevel::Verbose>(STR("[Radio] отправлен трек: {} {}\n"), std::wstring(g_kind.begin(), g_kind.end()), std::wstring(g_name.begin(), g_name.end()));
                g_sent_ms = now;
                g_dirty = false;
            }
        }
        g_was_connected = connected;
    }

    void on_radio(const std::string& kind, const std::string& name)
    {
        std::string line = kind + "|" + name;
        if (line == g_last_cmd || line == g_waiting) return; // Host повторяет трек каждые 10 с
        g_waiting.clear();
        if (kind == "file" && !have_track(name))
        {
            // Трек Хоста ещё не скопирован лаунчером: ждём файл, а Lua пока играет своё. Проверка здесь, а не в Lua:
            // io.open в Lua открывает по ANSI-кодировке и не находит имена с кириллицей
            g_waiting = line;
            g_waiting_name = name;
            return;
        }
        g_last_cmd = line;
        std::ofstream(g_cmd_file, std::ios::app | std::ios::binary) << line << '\n';
    }
} // namespace RadioSync
