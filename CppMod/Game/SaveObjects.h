// Двоичный «JSON» блока GAME сейва (версия 6): значения и объекты сейва по ID (RECON.md «5b.8»).
// Чистый C++ без Unreal. Python-эталон: tools/sav_json.py — разбор/запись обязаны совпадать с ним байт в байт.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace SaveObjects
{
    // Значение: у составных — дети, у остальных — байты как в файле (без байта типа), их смысл знают помощники ниже
    struct Value
    {
        uint8_t Type = 0;
        std::vector<uint8_t> Raw;                          // скаляры, строки, ссылки, FText, …
        std::vector<std::pair<std::string, Value>> Fields; // 0x10 структура, 0x15 объект, 0x16 запись объекта
        uint8_t KeyType = 0, ElemType = 0;                 // 0x14 словарь: тип ключа; 0x13/0x14: тип элемента/значения
        std::vector<Value> Keys, Items;                    // 0x14: ключи; 0x13/0x14: элементы/значения

        Value* field(const std::string& name);
        const Value* field(const std::string& name) const;
    };

    struct Object
    {
        std::string Id;      // 32 hex — ключ в _objects
        size_t Start = 0;    // запись объекта в блоке GAME: [Start, End)
        size_t End = 0;
        size_t MapCount = 0; // позиция счётчика карты _objects, в которой лежит объект
        Value Record;        // Type 0x16: _class, _name, _outer, _data
    };

    struct Game
    {
        std::vector<uint8_t> Data;
        std::vector<Object> Objects; // разобранные (неразобранные — только в Data; инвентаря они не касаются)
        size_t Unparsed = 0;

        const Object* find(const std::string& id) const;
        Object* find(const std::string& id);
    };

    Game parse(std::vector<uint8_t> data);

    std::vector<uint8_t> dump_object(const Object& o); // ключ ID + 0x16 + поля — как в файле

    // Помощники для значений
    std::string class_of(const Object& o);                       // _class
    std::string ref_id(const Value& v);                          // 0x12 вида 01 (объект сейва) → ID; иначе ""
    std::string guid_to_id(const uint8_t* guid);                 // FGuid (4 × uint32 LE) → 32 hex
    std::vector<uint8_t> id_to_guid(const std::string& id);
    std::string string_of(const Value& v);                       // 0x04/0x05/0x0E/0x11/0x19/0x1A → текст
    std::string asset_path(const Value& v);                      // 0x12 вида 03 → путь ассета; иначе ""
} // namespace SaveObjects
