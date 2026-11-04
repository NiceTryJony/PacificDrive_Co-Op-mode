// Универсальный снимок объекта игры в JSON: все свойства по рефлексии, с жёсткими лимитами (глубина, длина массива, число значений).
// Замена одноразовых Lua-проб: навёл на объект — получил всё, что игра о нём знает. Читать только на игровом потоке.
#pragma once
#include <string>

namespace RC::Unreal
{
    class UObject;
    class UStruct;
}

namespace ObjectDump
{
    // JSON одного объекта: {"class", "path", "truncated", "props":{имя: значение}}. Ссылки на другие объекты — только путём
    // (не раскрываются), структуры раскрываются до глубины 3, массивы режутся до 64 элементов — обрезанное помечено.
    std::string to_json(RC::Unreal::UObject* object);

    // JSON полей структуры по адресу в памяти (для живых свойств предметов IP_*, которых нет в полях объекта)
    std::string struct_to_json(RC::Unreal::UStruct* type, void* memory);

    // Объект под прицелом: владелец текущей подсветки (BP_HighlightManager_C.CurrentHighlight); nullptr — ничего не подсвечено
    RC::Unreal::UObject* target();

    // Ctrl+F5: снимок объекта под прицелом в файл ue4ss/Mods/PDCoopRecon/obj_<время>_<класс>.json. Обход — на игровом потоке, запись
    // в файл — в фоновом, так что игра не ждёт диск. Лаунчер забирает такие файлы вместе с остальными дампами
    void dump_target();
} // namespace ObjectDump
