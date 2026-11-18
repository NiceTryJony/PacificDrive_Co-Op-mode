// FindFirstOf обходит весь GUObjectArray (сотни тысяч объектов) — на кадровом пути держим результат в слабом указателе.
#pragma once
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace RC::Unreal::Cached
{
    // Поколение карты: All перечитывает список после загрузки карты, даже если срок не вышел (dllmain, LoadMap)
    inline uint32_t g_map_generation = 0;
    inline void on_map_loaded() { ++g_map_generation; }

    struct First
    {
        const TCHAR* Class;
        FWeakObjectPtr Ptr{};
        uint64_t RetryMs = 0;
        uint32_t Misses = 0, Generation = 0;

        UObject* get()
        {
            if (UObject* o = Ptr.Get()) return o;
            auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if (Generation != g_map_generation) Generation = g_map_generation, Misses = 0, RetryMs = 0; // новая карта — искать сразу
            if (now < RetryMs) return nullptr; // меню/загрузка: объекта нет, не ищем каждый кадр
            UObject* o = UObjectGlobals::FindFirstOf(Class);
            if (o) Ptr = FWeakObjectPtr(o), Misses = 0;
            // Объекта нет на этой карте (станция гаража в зоне, буря в гараже): полный обход раз в 250 мс шёл впустую — откат до 4 с
            else RetryMs = now + (250ull << (std::min)(Misses++, 4u));
            return o;
        }
    };

    // Сырой UObject*, который живёт между кадрами, уничтожается и без смены карты (машину эвакуировали, актёр убрала игра) — тогда
    // ProcessEvent по нему падает. Слабая копия: track() после присвоения, drop_if_dead() в начале тика.
    inline void track(UObject* raw, FWeakObjectPtr& weak) { weak = raw ? FWeakObjectPtr(raw) : FWeakObjectPtr{}; }
    inline bool drop_if_dead(UObject*& raw, const FWeakObjectPtr& weak)
    {
        if (!raw || weak.Get()) return false;
        raw = nullptr;
        return true;
    }

    inline uint64_t now_ms()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Все объекты класса, как FindAllOf, но список держится со слабыми указателями и перечитывается не чаще, чем просит вызывающий:
    // FindAllOf обходит весь GUObjectArray, а несколько модулей каждые 0,3–0,4 с ищут одно и то же (замер [Perf] 2026-11-16: Trunk до 17 мс).
    // Уничтоженные объекты отсеиваются при каждом get(); новые появятся после перечитывания — вызывающий просит его (refresh), если не нашёл своё
    struct All
    {
        const TCHAR* Class;
        std::vector<FWeakObjectPtr> Weak{};
        uint64_t AtMs = 0;
        uint32_t Generation = 0;

        uint64_t age_ms() const { return AtMs ? now_ms() - AtMs : UINT64_MAX; }

        // max_age_ms: список старше — перечитать; 0 — перечитать сейчас
        std::vector<UObject*> get(uint64_t max_age_ms)
        {
            std::vector<UObject*> out;
            if (!AtMs || Generation != g_map_generation || age_ms() >= max_age_ms)
            {
                UObjectGlobals::FindAllOf(Class, out);
                Weak.clear();
                for (UObject* o : out) Weak.emplace_back(o);
                AtMs = now_ms();
                Generation = g_map_generation;
                return out;
            }
            out.reserve(Weak.size());
            for (auto& w : Weak)
                if (UObject* o = w.Get()) out.push_back(o);
            return out;
        }
    };

    // Слоты машины (SlotSceneComponent): создаются вместе с машиной — общий список для PartsSync и TrunkSync
    inline All& slot_components() { static All a{STR("SlotSceneComponent")}; return a; }

    // Компоненты слотов своей машины. Нет ни одного — машина новая (появилась после перечитывания): перечитать, но не чаще retry_ms
    inline std::vector<UObject*> car_slot_components(UObject* car, uint64_t max_age_ms = 2000, uint64_t retry_ms = 300)
    {
        auto pick = [car](const std::vector<UObject*>& all) {
            std::vector<UObject*> mine;
            for (auto* c : all)
                if (c->GetOuterPrivate() == car) mine.push_back(c);
            return mine;
        };
        auto& cache = slot_components();
        auto mine = pick(cache.get(max_age_ms));
        if (mine.empty() && cache.age_ms() >= retry_ms) mine = pick(cache.get(0));
        return mine;
    }

    inline UObject* character() { static First f{STR("BaseCharacter")}; return f.get(); }
    inline UObject* car() { static First f{STR("NewVehiclePawn")}; return f.get(); }
} // namespace RC::Unreal::Cached
