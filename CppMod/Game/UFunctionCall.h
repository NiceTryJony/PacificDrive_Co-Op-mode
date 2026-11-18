// Вызов UFunction и чтение свойств из мода (на игровом потоке).
#pragma once
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace RC::Unreal
{
    // Буфер параметров раскладываем по смещениям из рефлексии игры, без хардкода структуры
    class UFunctionCall
    {
    public:
        UFunctionCall(UObject* object, const TCHAR* name) : m_object(object)
        {
            if (!object) return;
            // Поиск по цепочке классов и обход параметров — по строкам; результат помним по (класс, адрес литерала имени)
            Key key{object->GetClassPrivate(), name};
            auto& entry = cache()[key];
            if (!entry.Fn)
            {
                entry.Fn = object->GetFunctionByNameInChain(name);
                if (!entry.Fn) return; // не кэшируем промах: функция может появиться (подгрузка класса)
                for (FProperty* p : TFieldRange<FProperty>(entry.Fn, EFieldIterationFlags::IncludeDeprecated))
                    entry.Size = (std::max)(entry.Size, p->GetOffset_ForInternal() + p->GetSize());
            }
            m_fn = entry.Fn;
            m_ok = entry.Size <= static_cast<int32_t>(m_params.size());
        }

        // Кэш держит указатели на UFunction и FProperty: сбрасывать при смене карты (классы могут выгрузиться)
        static void clear_cache()
        {
            cache().clear();
            offsets().clear();
            property_cache().clear();
        }

        // Свойство по (класс, имя). GetValuePtrByPropertyNameInChain на каждый вызов строит FName и перебирает все свойства
        // класса с предками (у актёров — сотни); моды зовут это сотни раз за опрос ([Perf] 2026-11-17: Parts 2 мс, Trunk 3,7 мс пики).
        // Ключ — адрес строки имени, сам текст сверяется (имя может прийти не литералом). Промах тоже кэшируется: список свойств класса не меняется.
        // Имя класса сверяется тоже: сборщик мусора мог выгрузить класс посреди карты, а на его адрес встать другой
        static FProperty* find_property(UClass* cls, UObject* object, const TCHAR* name)
        {
            auto& entry = property_cache()[Key{cls, name}];
            FName class_name = cls->GetNamePrivate();
            if (!entry.Checked || !entry.ClassName.Equals(class_name) || entry.Name != name)
            {
                entry.Prop = object->GetPropertyByNameInChain(name);
                entry.Name = name;
                entry.ClassName = class_name;
                entry.Checked = true;
            }
            return entry.Prop;
        }

        explicit operator bool() const { return m_fn && m_ok; }

        template <typename T>
        T* param(const TCHAR* name)
        {
            auto& slot = offsets().try_emplace({m_fn, name}, Param{}).first->second;
            if (slot.Offset < 0)
            {
                for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated))
                    if (p->GetName() == name)
                    {
                        slot = {p->GetOffset_ForInternal(), p->GetSize()};
                        break;
                    }
                if (slot.Offset < 0) return nullptr;
            }
            // T больше параметра — запись затёрла бы соседние параметры (путаница типов, напр. int32 вместо uint8-перечисления)
            if (static_cast<int32_t>(sizeof(T)) > slot.Size) return nullptr;
            return reinterpret_cast<T*>(m_params.data() + slot.Offset);
        }

        void call() { m_object->ProcessEvent(m_fn, m_params.data()); }

    private:
        struct Key
        {
            const void* A;
            const TCHAR* Name;
            bool operator==(const Key&) const = default;
        };
        struct KeyHash
        {
            size_t operator()(const Key& k) const { return std::hash<const void*>()(k.A) * 31 + std::hash<const void*>()(k.Name); }
        };
        struct Entry
        {
            UFunction* Fn = nullptr;
            int32_t Size = 0;
        };
        static std::unordered_map<Key, Entry, KeyHash>& cache() { static std::unordered_map<Key, Entry, KeyHash> c; return c; }
        struct Param
        {
            int32_t Offset = -1;
            int32_t Size = 0;
        };
        static std::unordered_map<Key, Param, KeyHash>& offsets() { static std::unordered_map<Key, Param, KeyHash> c; return c; }
        struct PropertyEntry
        {
            FProperty* Prop = nullptr;
            std::basic_string<TCHAR> Name;
            FName ClassName{};
            bool Checked = false;
        };
        static std::unordered_map<Key, PropertyEntry, KeyHash>& property_cache() { static std::unordered_map<Key, PropertyEntry, KeyHash> c; return c; }

        UObject* m_object;
        UFunction* m_fn = nullptr;
        bool m_ok = false;
        // Нули = пустые FString/false/0; выравнивание 16 — для FTransform/FQuat (SSE)
        alignas(16) std::array<uint8_t, 512> m_params{};
    };

    // Значение свойства объекта по имени (в т.ч. унаследованного) или nullptr
    template <typename T>
    T* property(UObject* object, const TCHAR* name)
    {
        if (!object) return nullptr;
        UClass* cls = object->GetClassPrivate();
        // Сам объект — класс/структура: UE4SS ищет в его собственных полях, а не в полях класса — без кэша, как раньше
        if (!cls || cls->HasAllCastFlags(static_cast<EClassCastFlags>(UStruct::StaticClassCastFlags()))) return object->GetValuePtrByPropertyNameInChain<T>(name);
        FProperty* prop = UFunctionCall::find_property(cls, object, name);
        return prop ? prop->ContainerPtrToValuePtr<T>(object) : nullptr;
    }
} // namespace RC::Unreal
