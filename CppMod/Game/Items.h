// Предметы игры из мода (игровой поток): создать, удалить, путь архетипа. Общие для деталей и багажника машины (RECON.md «6.10», «6.11»).
#pragma once
#include <Unreal/UAssetRegistry.hpp>
#include <Unreal/UAssetRegistryHelpers.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/UFunctionCall.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Items
{
    using namespace RC;
    using namespace RC::Unreal;

    struct RawArray // TArray<UObject*> в памяти игры
    {
        UObject** Data;
        int32_t Num;
        int32_t Max;
    };

    inline std::string narrow(const StringType& s)
    {
        std::string out;
        for (auto c : s) out += static_cast<char>(c); // теги и пути ассетов — ASCII
        return out;
    }

    inline StringType wide(const std::string& s) { return StringType(s.begin(), s.end()); }

    // Предметы ячейки (UItemSlot.Items); пусто, если свойства нет
    inline RawArray items_of(UObject* item_slot)
    {
        auto* items = item_slot ? property<RawArray>(item_slot, STR("Items")) : nullptr;
        return items && items->Data ? *items : RawArray{};
    }

    inline std::string archetype_path(UObject* instance)
    {
        auto** archetype = instance ? property<UObject*>(instance, STR("Archetype")) : nullptr;
        return archetype && *archetype ? narrow((*archetype)->GetPathName()) : std::string{};
    }

    // Эффекты состояния предмета (UItemInstance.ActiveEffects — «протечка», «пробито» и т.п., классы SE_*): пути классов по порядку
    inline std::vector<std::string> effects_of(UObject* instance)
    {
        std::vector<std::string> out;
        auto* effects = instance ? property<RawArray>(instance, STR("ActiveEffects")) : nullptr;
        if (!effects || !effects->Data) return out;
        for (int32_t i = 0; i < effects->Num; ++i)
        {
            auto* effect = effects->Data[i];
            if (!effect) continue;
            auto path = narrow(effect->GetClassPrivate()->GetPathName());
            // «унаследованное сопротивление» от соседних деталей игра пересчитывает сама у каждого; синхронизация гоняла его по кругу:
            // снятие одной детали давало запросы по 4–5 соседним слотам (тест 0.17.4 2026-11-07 21:55)
            if (!path.ends_with("SE_InheritResist_C")) out.push_back(std::move(path));
        }
        std::sort(out.begin(), out.end()); // повторы остаются: у одной детали бывает несколько одинаковых эффектов (парадокс из четырёх), чинятся по одному
        return out;
    }

    // AddItemStatusEffect / RemoveItemStatusEffect менеджера эффектов. false — нет менеджера, класса эффекта или функции
    inline bool change_effect(UObject* instance, const std::string& effect_class, bool add)
    {
        static const TCHAR* manager_name = STR("BP_StatusEffectManager_C");
        auto* manager = UObjectGlobals::FindFirstOf(manager_name);
        auto* effect = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, wide(effect_class).c_str());
        if (!manager || !effect) return false;
        UFunctionCall fn(manager, add ? STR("AddItemStatusEffect") : STR("RemoveItemStatusEffect"));
        auto* p_instance = fn ? fn.param<UObject*>(STR("Instance")) : nullptr;
        auto* p_class = fn ? fn.param<UObject*>(STR("StatusEffectClass")) : nullptr;
        if (!p_instance || !p_class) return false;
        *p_instance = instance;
        *p_class = effect;
        fn.call();
        return true;
    }

    inline uint8_t create_context(UObject* instance)
    {
        auto* context = instance ? property<uint8_t>(instance, STR("CreateContext")) : nullptr;
        return context ? *context : 0;
    }

    // Вместе с проявлением в мире; игра сама убирает предмет из его ячейки
    inline void remove(UObject* instance)
    {
        UFunctionCall fn(instance, STR("Remove"));
        if (auto* p = fn ? fn.param<bool>(STR("bRemoveActors")) : nullptr)
        {
            *p = true;
            fn.call();
        }
    }

    // Объект по полному пути; не загружен — грузит пакет через реестр ассетов, как LoadAsset в Lua UE4SS (блокирующе, ~150 мс;
    // проверено 2026-11-17 на GEV_WindSquall). Класс блюпринта «…/X.X_C» лежит в реестре как «…/X.X». nullptr — нет такого ассета
    inline UObject* find_or_load(const std::string& path)
    {
        StringType wpath = wide(path);
        if (auto* found = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, wpath)) return found;
        auto* registry = static_cast<UAssetRegistry*>(UAssetRegistryHelpers::GetAssetRegistry().ObjectPointer);
        if (!registry) return nullptr;
        StringType asset = wpath.ends_with(STR("_C")) ? wpath.substr(0, wpath.size() - 2) : wpath;
        FAssetData data = registry->GetAssetByObjectPath(FName(asset, FNAME_Add));
        if (!data.PackageName().GetComparisonIndex()) return nullptr;
        UAssetRegistryHelpers::GetAsset(data);
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, wpath);
    }

    // nullptr — архетипа нет или вызов не прошёл. context — как у соседних предметов (значения enum в дампе обфусцированы)
    inline UObject* create(UObject* world_context, const std::string& archetype_path, uint8_t context)
    {
        auto* archetype = find_or_load(archetype_path);
        if (!archetype) return nullptr;
        static auto* lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/PenDriverInventory.Default__InventoryBlueprintLibrary"));
        UFunctionCall fn(lib, STR("CreateItemInstance"));
        auto* p_world = fn ? fn.param<UObject*>(STR("WorldContextObject")) : nullptr;
        auto* p_archetype = fn ? fn.param<UObject*>(STR("Archetype")) : nullptr;
        auto* p_context = fn ? fn.param<uint8_t>(STR("CreateContext")) : nullptr;
        auto* p_ret = fn ? fn.param<UObject*>(STR("ReturnValue")) : nullptr;
        if (!p_world || !p_archetype || !p_context || !p_ret) return nullptr;
        *p_world = world_context;
        *p_archetype = archetype;
        *p_context = context;
        fn.call();
        return *p_ret;
    }

    // Вызов функции игры, у которой первый параметр — выходной массив объектов (имя параметра в блюпринте неизвестно).
    // false — функции нет или первый параметр не массив. Массив, выделенный игрой, не освобождаем: десятки байт на вызов
    inline bool call_array(UObject* object, const TCHAR* name, std::vector<UObject*>& out)
    {
        auto* fn = object ? object->GetFunctionByNameInChain(name) : nullptr;
        if (!fn) return false;
        alignas(16) uint8_t params[512]{};
        int32_t size = 0, offset = -1;
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
        {
            // только массив объектов — иначе элементы нельзя читать как UObject*
            if (auto* a = size == 0 ? CastField<FArrayProperty>(p) : nullptr; a && a->GetInner() && CastField<FObjectProperty>(a->GetInner()))
                offset = p->GetOffset_ForInternal();
            size = (std::max)(size, p->GetOffset_ForInternal() + p->GetSize());
        }
        if (offset < 0 || size > static_cast<int32_t>(sizeof(params))) return false;
        object->ProcessEvent(fn, params);
        auto* array = reinterpret_cast<RawArray*>(params + offset);
        for (int32_t i = 0; array->Data && i < array->Num; ++i)
            if (array->Data[i]) out.push_back(array->Data[i]);
        return true;
    }

    // UItemSlot.AddItem; false — нет функции
    inline bool add_to_slot(UObject* item_slot, UObject* instance)
    {
        UFunctionCall add(item_slot, STR("AddItem"));
        auto* p_item = add ? add.param<UObject*>(STR("Item")) : nullptr;
        if (!p_item) return false;
        *p_item = instance;
        add.call();
        return true;
    }
} // namespace Items
