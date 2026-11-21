// Состояние мода для окна в терминале (tools/CoopConsole), лаунчера и HUD: текстовый файл key=value, раз в секунду. Формат — docs/workflow/WORKFLOW.md «Окно в терминале».
#pragma once
#include <filesystem>
#include <fstream>
#include <string>

#include "Networking/Net.h"

namespace StatusFile
{
    // phase — что сейчас происходит (строка для лаунчера), пусто — всё готово
    void tick(const std::filesystem::path& file, const std::string& version, Net::Role role, const std::string& phase, const std::string& stage_lines); // игровой поток
    void mark_world_saved(); // Host записал сейв зоны / Client принял его — в status.txt world_saved_at=

    // Запись на месте фиксированной длиной: открытие свежего файла стоит ~10 мс (антивирус), читатели держат файл открытым и читают с начала.
    // out — поток вызывающего, открывается один раз
    void write_fixed(std::fstream& out, const std::filesystem::path& file, std::string text, size_t size);
} // namespace StatusFile
