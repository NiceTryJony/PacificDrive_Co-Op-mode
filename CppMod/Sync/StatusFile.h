// Состояние мода для окна в терминале (tools/CoopConsole): текстовый файл key=value, раз в секунду. Формат — docs/WORKFLOW.md «Окно в терминале».
#pragma once
#include <filesystem>
#include <string>

#include "Networking/Net.h"

namespace StatusFile
{
    void tick(const std::filesystem::path& file, const std::string& version, Net::Role role); // игровой поток
} // namespace StatusFile
