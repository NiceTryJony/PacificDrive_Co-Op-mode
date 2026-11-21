#pragma once
// Пропуск лаунчера: без launch.token мод в игре спит (ни хуков, ни сети, ни оверлея).
// Лаунчер пишет в файл секунды с 1970 г. перед запуском игры; пропуск живёт 10 минут до старта игры (обновление Steam и т.п.).
// Мод файл не удаляет: его читают C++ и Lua моды и два окна игры на одном ПК; устаревший пропуск просто истекает.
#include <chrono>
#include <filesystem>
#include <fstream>

namespace LaunchGate
{
    constexpr long long WindowSec = 600;
    constexpr long long FutureSec = 60; // токен «из будущего» (часы были сбиты) не принимаем

    // shortcut: сравниваем со временем загрузки мода, а не старта процесса (разница секунды), хватает для окна в 10 минут
    inline bool valid(const std::filesystem::path& token, long long now_sec)
    {
        std::ifstream in(token);
        long long t = 0;
        return in >> t && t >= now_sec - WindowSec && t <= now_sec + FutureSec;
    }

    inline bool valid_now()
    {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return valid(L"ue4ss/Mods/PacificDriveCoop/launch.token", now); // рабочая папка игры — Binaries/Win64
    }
}
