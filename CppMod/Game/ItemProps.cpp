#include "ItemProps.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include "Game/Items.h"
#include "Game/ObjectDump.h"
#include "Game/UFunctionCall.h"

#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>

// То же объявление, что в Windows.h (HMODULE = HINSTANCE__*) — без тяжёлого заголовка рядом с Unreal
extern "C" __declspec(dllimport) struct HINSTANCE__* __stdcall GetModuleHandleW(const wchar_t* name);

using namespace RC;
using namespace RC::Unreal;

// Живые значения свойств предмета лежат в data-block экземпляра (instance+0xA0): таблица изменённых свойств
// (DataBlock+0x28, её копирует Duplicate), шаблон архетипа — DataBlock+0x18. GetItemProperties через ProcessEvent не
// пускает по типу выходного параметра, поэтому зовём те же функции игры напрямую: «блок готов?» → «найти запись свойства»
// → память структуры FIP_* (vtable[3] записи) → поле по смещению из рефлексии структуры.
// shortcut: адреса exe+0x... — только для этой сборки игры; при обновлении игры сверка байт отключит чтение
// (available() == false). Заменить поиском по сигнатуре, если игра обновится.
namespace ItemProps
{
    namespace
    {
        constexpr uintptr_t InstanceDataBlockOffset = 0xA0; // UItemInstance: неотражённый указатель на data-block
        constexpr uintptr_t RvaDataBlockReady = 0x1190FF0;  // bool(DataBlock*) — ленивая загрузка data-block
        constexpr uintptr_t RvaDataBlockFind = 0x11997E0;   // void(DataBlock*, TSharedPtr* out, UItemProperties*)
        constexpr uint8_t SigReady[] = {0x40, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x80, 0x39, 0x00, 0x48, 0x8B, 0xF1};
        constexpr uint8_t SigFind[] = {0x40, 0x55, 0x53, 0x56, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xC9};

        struct RefController // TSharedPtr, режим NotThreadSafe (игра уменьшает счётчики без lock)
        {
            void** VTable;
            int32_t Shared;
            int32_t Weak;
        };
        struct SharedPtr
        {
            void* Object;
            RefController* Ctrl;
        };

        using ReadyFn = bool (*)(void* data_block);
        using FindFn = void (*)(void* data_block, SharedPtr* out, UObject* property_struct);
        using MemoryFn = uint8_t* (*)(void* object);
        using DestroyFn = void (*)(RefController*);
        using DeleteFn = void (*)(RefController*, int32_t);

        ReadyFn g_ready = nullptr;
        FindFn g_find = nullptr;
        bool g_resolved = false;

        bool resolve()
        {
            if (g_resolved) return g_ready && g_find;
            g_resolved = true;
            auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            if (std::memcmp(base + RvaDataBlockReady, SigReady, sizeof(SigReady)) || std::memcmp(base + RvaDataBlockFind, SigFind, sizeof(SigFind)))
            {
                Output::send<LogLevel::Error>(STR("[ItemProps] байты функций data-block не совпали — другая версия игры, чтение свойств выключено\n"));
                return false;
            }
            g_ready = reinterpret_cast<ReadyFn>(base + RvaDataBlockReady);
            g_find = reinterpret_cast<FindFn>(base + RvaDataBlockFind);
            return true;
        }

        void release(SharedPtr& p)
        {
            auto* c = p.Ctrl;
            if (!c || --c->Shared) return;
            reinterpret_cast<DestroyFn>(c->VTable[0])(c);
            if (!--c->Weak) reinterpret_cast<DeleteFn>(c->VTable[1])(c, 1);
        }

        // Имена полей BP-структур: «Durability_5_448CDA0B4D9935979EBC4AA6A500ADAF» → «Durability» (как в сейве)
        std::string clean_field_name(const std::string& name)
        {
            auto guid = name.rfind('_');
            if (guid == std::string::npos || name.size() - guid - 1 != 32) return name;
            auto num = name.rfind('_', guid - 1);
            return num == std::string::npos ? name : name.substr(0, num);
        }

        // Свойства, разрешённые экземпляру (их же проверяет GetItemProperties). FindAllOf не годится: UItemProperties —
        // UScriptStruct, а UE4SS исключает такие из FindAllOf (тест 0.8.3)
        // Имена структур и найденные поля — кэш: чтение идёт сотни раз за опрос (детали машины, багажник), а каждое сравнение
        // строило строки из FName. Структуры IP_* — ассеты: держатся всю сессию; на случай выгрузки сверяем FName структуры
        struct StructInfo
        {
            FName Name;
            std::string Text;
        };
        std::unordered_map<UObject*, StructInfo> g_struct_names;
        std::map<std::pair<UObject*, const Spec*>, std::pair<FName, FProperty*>> g_fields;

        const std::string& struct_name_of(UObject* ps)
        {
            FName name = ps->GetNamePrivate();
            auto& info = g_struct_names[ps];
            if (info.Text.empty() || !info.Name.Equals(name)) info = {name, Items::narrow(ps->GetName())};
            return info.Text;
        }

        UObject* allowed_struct(UObject* instance, const char* struct_name)
        {
            auto* set = property<TSet<UObject*>>(instance, STR("AllowedProperties"));
            if (!set) return nullptr;
            for (UObject* ps : *set)
                if (ps && struct_name_of(ps) == struct_name) return ps;
            return nullptr;
        }

        FProperty* find_field(UObject* property_struct, const Spec& spec)
        {
            FName name = property_struct->GetNamePrivate();
            auto& cached = g_fields[{property_struct, &spec}];
            if (cached.second && cached.first.Equals(name)) return cached.second;
            for (FProperty* p : TFieldRange<FProperty>(static_cast<UStruct*>(property_struct), EFieldIterationFlags::IncludeDeprecated))
                if (clean_field_name(Items::narrow(p->GetName())) == spec.Field)
                {
                    cached = {name, p};
                    return p;
                }
            return nullptr;
        }

        // Значение поля как текст; пусто — не получилось (нет записи свойства, не тот тип)
        std::string read_one(UObject* instance, const Spec& spec)
        {
            auto* property_struct = allowed_struct(instance, spec.Struct);
            if (!property_struct) return {}; // у этого предмета такого свойства нет
            auto* field = find_field(property_struct, spec);
            bool type_ok = field && (spec.Type == 'i' ? CastField<FIntProperty>(field) != nullptr : CastField<FFloatProperty>(field) != nullptr);
            if (!type_ok) return {};
            void* data_block = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(instance) + InstanceDataBlockOffset);
            if (!data_block || !g_ready(data_block)) return {};
            SharedPtr found{};
            g_find(data_block, &found, property_struct);
            if (!found.Object) return {};
            std::string text;
            if (auto* memory = reinterpret_cast<MemoryFn>((*reinterpret_cast<void***>(found.Object))[3])(found.Object))
            {
                uint8_t* at = memory + field->GetOffset_ForInternal();
                text = spec.Type == 'i' ? std::to_string(*reinterpret_cast<int32_t*>(at)) : std::format("{}", *reinterpret_cast<float*>(at)); // float — кратчайшая точная запись
            }
            release(found);
            return text;
        }
    } // namespace

    bool available() { return resolve(); }

    bool write(UObject* instance, const Spec& spec, const std::string& text)
    {
        if (!instance || text.empty()) return false;
        auto* property_struct = allowed_struct(instance, spec.Struct);
        if (!property_struct) return false;
        static auto* library = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/PenDriverInventory.Default__InventoryBlueprintLibrary"));
        UFunctionCall fn(library, STR("SetItemProperty"));
        auto* p_component = fn ? fn.param<UObject*>(STR("PropertyComponent")) : nullptr;
        auto* p_instance = fn ? fn.param<UObject*>(STR("Instance")) : nullptr;
        auto* p_name = fn ? fn.param<FName>(STR("PropertyName")) : nullptr;
        auto* p_value = fn ? fn.param<int32_t>(STR("Value")) : nullptr;
        auto* p_ret = fn ? fn.param<bool>(STR("ReturnValue")) : nullptr;
        if (!p_component || !p_instance || !p_name || !p_value || !p_ret) return false;
        try
        {
            if (spec.Type == 'i') *p_value = std::stoi(text);
            else
            {
                float value = std::stof(text);
                std::memcpy(p_value, &value, sizeof(value)); // float битами в int32
            }
        }
        catch (const std::exception&) // мусор из сети не должен ронять игру
        {
            return false;
        }
        *p_component = property_struct;
        *p_instance = instance;
        *p_name = FName(Items::wide(spec.Field).c_str(), FNAME_Add);
        fn.call();
        return *p_ret;
    }

    std::string read(UObject* instance, const Spec& spec)
    {
        if (!instance || !resolve()) return {};
        return read_one(instance, spec);
    }

    std::vector<Value> read(UObject* instance)
    {
        std::vector<Value> out;
        if (!instance || !resolve()) return out;
        for (const Spec& spec : Specs)
            if (auto text = read_one(instance, spec); !text.empty()) out.push_back({&spec, text});
        return out;
    }

    void selftest_write()
    {
        if (!resolve()) return;
        auto* manager = UObjectGlobals::FindFirstOf(STR("BP_InventoryManager_C"));
        auto** container = manager ? property<UObject*>(manager, STR("Inventory_Player")) : nullptr;
        auto* slots = container && *container ? property<Items::RawArray>(*container, STR("Slots")) : nullptr;
        if (!slots || !slots->Data)
        {
            Output::send<LogLevel::Warning>(STR("[ItemProps] самопроверка: нет рюкзака\n"));
            return;
        }
        int checked = 0, ok = 0;
        for (int32_t i = 0; i < slots->Num; ++i)
        {
            auto list = Items::items_of(slots->Data[i]);
            for (int32_t k = 0; k < list.Num; ++k)
            {
                auto* instance = list.Data[k];
                if (!instance) continue;
                for (const Value& before : read(instance))
                {
                    ++checked;
                    const Spec& spec = *before.Spec;
                    // −10 %: для целого и дробного; ноль остаётся нулём — тогда проверяем на +1
                    std::string changed;
                    if (spec.Type == 'i') { int v = std::stoi(before.Text); changed = std::to_string(v > 10 ? v - v / 10 : v + 1); }
                    else { float v = std::stof(before.Text); changed = std::format("{}", v > 0.5f ? v * 0.9f : v + 0.25f); }
                    bool wrote = write(instance, spec, changed);
                    std::string after = wrote ? read_one(instance, spec) : std::string{};
                    bool restored = write(instance, spec, before.Text);
                    std::string again = restored ? read_one(instance, spec) : std::string{};
                    bool pass = wrote && after == changed && restored && again == before.Text;
                    ok += pass;
                    auto path = Items::archetype_path(instance);
                    Output::send<LogLevel::Verbose>(STR("[ItemProps] проверка записи {}.{}: было {} → записано {} → прочитано {} → возвращено {} → прочитано {} — {}\n"),
                                                    Items::wide(path.substr(path.rfind('.') + 1)) + STR(" ") + Items::wide(spec.Struct), Items::wide(spec.Field),
                                                    Items::wide(before.Text), Items::wide(changed), Items::wide(after), Items::wide(before.Text), Items::wide(again),
                                                    pass ? STR("OK") : STR("ОШИБКА"));
                }
            }
        }
        Output::send<LogLevel::Verbose>(STR("[ItemProps] проверка записи: {} из {} совпали\n"), ok, checked);
    }

    void dump_backpack()
    {
        if (!resolve()) return;
        auto* manager = UObjectGlobals::FindFirstOf(STR("BP_InventoryManager_C"));
        if (!manager)
        {
            Output::send<LogLevel::Warning>(STR("[ItemProps] нет менеджера инвентаря (меню/загрузка)\n"));
            return;
        }
        int items = 0, with_values = 0;
        for (auto [name, prop] : {std::pair{STR("рюкзак"), STR("Inventory_Player")}, std::pair{STR("рука"), STR("Inventory_PlayerHand")}})
        {
            auto** container = property<UObject*>(manager, prop);
            auto* slots = container && *container ? property<Items::RawArray>(*container, STR("Slots")) : nullptr;
            if (!slots || !slots->Data) continue;
            for (int32_t i = 0; i < slots->Num; ++i)
            {
                auto list = Items::items_of(slots->Data[i]);
                for (int32_t k = 0; k < list.Num; ++k)
                {
                    auto* instance = list.Data[k];
                    if (!instance) continue;
                    ++items;
                    auto values = read(instance);
                    if (values.empty()) continue;
                    ++with_values;
                    std::string text;
                    for (auto& v : values) text += std::string(" ") + v.Spec->Struct + "." + v.Spec->Field + "=" + v.Text;
                    auto path = Items::archetype_path(instance);
                    auto short_name = path.substr(path.rfind('.') + 1);
                    Output::send<LogLevel::Verbose>(STR("[ItemProps] {}\n"), StringType(name) + STR(": ") + Items::wide(short_name) + Items::wide(text));
                }
            }
        }
        Output::send<LogLevel::Verbose>(STR("[ItemProps] предметов {}, со значениями {}\n"), items, with_values);
    }

    std::string item_json(UObject* instance)
    {
        if (!instance || !resolve()) return {};
        auto* set = property<TSet<UObject*>>(instance, STR("AllowedProperties"));
        void* data_block = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(instance) + InstanceDataBlockOffset);
        if (!set || !data_block || !g_ready(data_block)) return {};
        std::string json = "{\"archetype\":\"" + Items::archetype_path(instance) + "\",\"effects\":[";
        bool first = true;
        for (auto& effect : Items::effects_of(instance))
        {
            json += (first ? "\"" : ",\"") + effect + "\"";
            first = false;
        }
        json += "],\"ip\":{";
        first = true;
        for (UObject* property_struct : *set)
        {
            if (!property_struct) continue;
            SharedPtr found{};
            g_find(data_block, &found, property_struct);
            if (!found.Object) continue;
            if (auto* memory = reinterpret_cast<MemoryFn>((*reinterpret_cast<void***>(found.Object))[3])(found.Object))
            {
                json += (first ? "\"" : ",\"") + Items::narrow(property_struct->GetName()) + "\":" + ObjectDump::struct_to_json(static_cast<UStruct*>(property_struct), memory);
                first = false;
            }
            release(found);
        }
        return json + "}}";
    }

    // Диагностика краски/оклейки (Ctrl+F4): все поля IP_Tintable у деталей-дверей. Указатели — именем объекта, остальное — байтами
    void dump_tintable()
    {
        if (!resolve()) return;
        std::vector<UObject*> instances;
        UObjectGlobals::FindAllOf(STR("ItemInstance"), instances);
        int printed = 0;
        for (auto* instance : instances)
        {
            if (!instance || printed >= 300) break;
            auto path = Items::archetype_path(instance);
            if (path.find("Door") == std::string::npos) continue;
            auto* property_struct = allowed_struct(instance, "IP_Tintable");
            if (!property_struct) continue;
            void* data_block = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(instance) + InstanceDataBlockOffset);
            if (!data_block || !g_ready(data_block)) continue;
            SharedPtr found{};
            g_find(data_block, &found, property_struct);
            if (!found.Object) continue;
            ++printed;
            std::string text;
            if (auto* memory = reinterpret_cast<MemoryFn>((*reinterpret_cast<void***>(found.Object))[3])(found.Object))
                for (FProperty* p : TFieldRange<FProperty>(static_cast<UStruct*>(property_struct), EFieldIterationFlags::IncludeDeprecated))
                {
                    uint8_t* at = memory + p->GetOffset_ForInternal();
                    text += " " + clean_field_name(Items::narrow(p->GetName())) + "=";
                    if (CastField<FObjectProperty>(p))
                    {
                        auto* object = *reinterpret_cast<UObject**>(at);
                        text += object ? Items::narrow(object->GetName()) : "null"; // короткое имя: дверей много
                    }
                    else
                        for (int i = 0; i < std::min<int>(p->GetElementSize(), 8); ++i) text += std::format("{:02X}", at[i]);
                }
            release(found);
            Output::send<LogLevel::Verbose>(STR("[ItemProps] краска {} {}:{}\n"), Items::wide(path.substr(path.rfind('.') + 1)), instance->GetName(), Items::wide(text));
        }
        Output::send<LogLevel::Verbose>(STR("[ItemProps] краска: дверей с IP_Tintable {}\n"), printed);
    }
} // namespace ItemProps
