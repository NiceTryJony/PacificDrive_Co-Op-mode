// Состояние мода для окна в терминале (tools/CoopConsole): текстовый файл key=value, раз в секунду. Формат — docs/WORKFLOW.md «Окно в терминале».
#pragma once
#include <filesystem>
#include <string>

#include "Networking/Net.h"

namespace StatusFile
{
    // phase — что сейчас происходит (строка для лаунчера), пусто — всё готово
    void tick(const std::filesystem::path& file, const std::string& version, Net::Role role, const std::string& phase); // игровой поток
} // namespace StatusFile
