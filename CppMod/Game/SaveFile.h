// Файл сейва игры (odsave<слот>.sav): заголовок SAVC + таблица блоков + блоки OPTS/META/GAME/CHKP (RECON.md «5b.9»).
// Чистый C++ без Unreal — общий для мода и tools/SaveInvTool. Python-эталон: tools/sav_unpack.py.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SaveFile
{
    struct Block
    {
        std::string Tag;           // OPTS / META / GAME / CHKP
        uint32_t Type = 0;         // 2 — как есть, 6 — LZ4 (у игры так хранится GAME)
        std::vector<uint8_t> Data; // распакованные байты
    };

    struct Container
    {
        std::vector<uint8_t> Header; // всё до первого блока (SAVC + таблица с пустыми строками) — шаблон для записи
        std::vector<Block> Blocks;   // в порядке таблицы

        Block* find(const std::string& tag);
    };

    // nullopt + error — файл не сейв игры или повреждён
    std::optional<Container> read(const std::vector<uint8_t>& file, std::string& error);

    // Файл с заголовком и таблицей из Header и тем же видом каждого блока (GAME — LZ4, остальные — как есть).
    // Как pack() в tools/sav_inventory.py, но GAME сжат по-настоящему: сверять с эталоном по распакованным блокам, не по байтам
    std::vector<uint8_t> write(const Container& c);

    // LZ4 block (без кадра); nullopt — данные битые или размер не совпал с ожидаемым
    std::optional<std::vector<uint8_t>> lz4_decompress(const uint8_t* src, size_t size, size_t expected);

    // Настоящий LZ4 block (жадный, хэш 4 байт): GAME ~7 МБ → ~2,6 МБ, как у игры. Раньше писали одни литералы — файл пассажира раздувался в 2,7 раза
    std::vector<uint8_t> lz4_compress(const std::vector<uint8_t>& src);
} // namespace SaveFile
