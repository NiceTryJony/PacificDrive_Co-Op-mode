// Свойства предметов, которые меняются в игре: прочность, топливо, заряд (RECON.md «6.13»).
// Чтение — из живой памяти data-block экземпляра (те же функции игры, что внутри GetItemProperties); запись — штатный
// UInventoryBlueprintLibrary.SetItemProperty (прямая запись в память интерфейс не обновляет, тесты 0.8.4–0.8.6).
#pragma once
#include <string>
#include <vector>

// Без заголовков UE4SS: fmt из DynamicOutput.hpp обязан подключаться раньше Unreal/UObject.hpp, иначе предупреждения fmt
// становятся ошибками (C4582) — поэтому только объявление класса
namespace RC::Unreal
{
    class UObject;
}

namespace ItemProps
{
    // Что синхронизируем. Список короткий намеренно: каждое свойство проверено на экране (Lua F3/F2)
    struct Spec
    {
        const char* Struct; // IP_Durability — объект класса ItemProperties
        const char* Field;  // чистое имя поля (как в сейве)
        char Type;          // 'i' — int32, 'f' — float
    };
    // Порядок важен: индекс уходит в сеть (TrunkValue.Spec) — новые только в конец.
    // IP_Health — здоровье деталей машины (полоса прочности, чинится Repair Putty, лечит станция регенерации)
    inline constexpr Spec Specs[] = {{"IP_Durability", "Durability", 'i'}, {"IP_Fuelable", "Fuel", 'f'}, {"IP_Battery", "Battery", 'f'}, {"IP_Health", "Health", 'f'}};
    inline constexpr const Spec& Health = Specs[3];

    struct Value
    {
        const Spec* Spec;
        std::string Text; // число как текст: целое или кратчайшая точная запись float
    };

    // Значения whitelist-свойств, которые есть у предмета; пусто — нет ни одного или data-block не готов
    std::vector<Value> read(RC::Unreal::UObject* instance);

    // Одно свойство; пусто — у предмета его нет или data-block не готов
    std::string read(RC::Unreal::UObject* instance, const Spec& spec);

    // Записать значение (текст из read()) предмету штатным SetItemProperty — интерфейс обновляется сам. Типы как у Spec:
    // целое пишется как есть, float — битами в int32 (так принимает параметр «любого типа», проверено Lua F2).
    // false — у предмета нет такого свойства или игра отказала
    bool write(RC::Unreal::UObject* instance, const Spec& spec, const std::string& text);

    // Проверка версии игры (байты функций совпали). false — читать нельзя, в логе причина
    bool available();

    // Диагностика: значения всех предметов рюкзака и руки в лог (клавиша Ctrl+F8)
    void dump_backpack();

    // JSON предмета: архетип, эффекты и все разрешённые ему свойства IP_* с живыми значениями (для ObjectDump); пусто — data-block не готов
    std::string item_json(RC::Unreal::UObject* instance);

    // Диагностика краски и оклейки (клавиша Ctrl+F4): поля IP_Tintable у деталей-дверей в лог
    void dump_tintable();

    // Самопроверка записи (клавиша Ctrl+F9): у каждого предмета рюкзака с whitelist-свойствами записать значение −10 %
    // от прочитанного, прочитать обратно, вернуть исходное и прочитать ещё раз; итог в лог. На экране значение на миг меняется
    void selftest_write();
} // namespace ItemProps
