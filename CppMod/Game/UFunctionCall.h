// Вызов UFunction и чтение свойств из мода (на игровом потоке).
#pragma once
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
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

        // Кэш держит указатели на UFunction: сбрасывать при смене карты (классы могут выгрузиться)
        static void clear_cache()
        {
            cache().clear();
            offsets().clear();
        }

        explicit operator bool() const { return m_fn && m_ok; }

        template <typename T>
        T* param(const TCHAR* name)
        {
            auto& offset = offsets().try_emplace({m_fn, name}, -1).first->second;
            if (offset < 0)
            {
                for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated))
                    if (p->GetName() == name)
                    {
                        offset = p->GetOffset_ForInternal();
                        break;
                    }
                if (offset < 0) return nullptr;
            }
            return reinterpret_cast<T*>(m_params.data() + offset);
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
        static std::unordered_map<Key, int32_t, KeyHash>& offsets() { static std::unordered_map<Key, int32_t, KeyHash> c; return c; }

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
        return object ? object->GetValuePtrByPropertyNameInChain<T>(name) : nullptr;
    }
} // namespace RC::Unreal
