// FindFirstOf обходит весь GUObjectArray (сотни тысяч объектов) — на кадровом пути держим результат в слабом указателе.
#pragma once
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <chrono>
#include <cstdint>

namespace RC::Unreal::Cached
{
    struct First
    {
        const TCHAR* Class;
        FWeakObjectPtr Ptr{};
        uint64_t RetryMs = 0;

        UObject* get()
        {
            if (UObject* o = Ptr.Get()) return o;
            auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if (now < RetryMs) return nullptr; // меню/загрузка: объекта нет, не ищем каждый кадр
            UObject* o = UObjectGlobals::FindFirstOf(Class);
            if (o) Ptr = FWeakObjectPtr(o);
            else RetryMs = now + 250;
            return o;
        }
    };

    inline UObject* character() { static First f{STR("BaseCharacter")}; return f.get(); }
    inline UObject* car() { static First f{STR("NewVehiclePawn")}; return f.get(); }
} // namespace RC::Unreal::Cached
