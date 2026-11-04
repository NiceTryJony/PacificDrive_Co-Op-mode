// Вызов UFunction и чтение свойств из мода (на игровом потоке).
#pragma once
#include <Unreal/UObject.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace RC::Unreal
{
    // Буфер параметров раскладываем по смещениям из рефлексии игры, без хардкода структуры
    class UFunctionCall
    {
    public:
        UFunctionCall(UObject* object, const TCHAR* name) : m_object(object), m_fn(object ? object->GetFunctionByNameInChain(name) : nullptr)
        {
            if (!m_fn) return;
            int32_t size = 0;
            for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated))
                size = (std::max)(size, p->GetOffset_ForInternal() + p->GetSize());
            m_ok = size <= static_cast<int32_t>(m_params.size());
        }

        explicit operator bool() const { return m_fn && m_ok; }

        template <typename T>
        T* param(const TCHAR* name)
        {
            for (FProperty* p : TFieldRange<FProperty>(m_fn, EFieldIterationFlags::IncludeDeprecated))
                if (p->GetName() == name) return reinterpret_cast<T*>(m_params.data() + p->GetOffset_ForInternal());
            return nullptr;
        }

        void call() { m_object->ProcessEvent(m_fn, m_params.data()); }

    private:
        UObject* m_object;
        UFunction* m_fn;
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
