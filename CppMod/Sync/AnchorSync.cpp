#include "AnchorSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <chrono>
#include <cmath>
#include <map>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace AnchorSync
{
    namespace
    {
        constexpr uint64_t PollMs = 300, SettleMs = 8000;

        struct Vec
        {
            float X, Y, Z;
        };

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        uint64_t g_loaded_ms = 0, g_last_poll_ms = 0;
        bool g_is_host = false;
        std::map<UObject*, bool> g_taken; // основание → якорь уже взят (на прошлом опросе); только для сравнения

        std::vector<UObject*> bases()
        {
            std::vector<UObject*> out;
            UObjectGlobals::FindAllOf(STR("BP_Spawn_Anchor_Base_01_C"), out);
            return out;
        }

        bool taken(UObject* base)
        {
            auto* flag = property<bool>(base, STR("AnchorRemoved"));
            auto** instance = property<UObject*>(base, STR("ItemInstance"));
            return (flag && *flag) || !instance || !*instance;
        }

        Vec location(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {NAN, NAN, NAN};
            fn.call();
            return *ret;
        }

        // Якорь у собеседника взят — у нас его больше нет: экземпляр убрать, флаг поднять
        void take_here(UObject* base)
        {
            if (auto** instance = property<UObject*>(base, STR("ItemInstance")); instance && *instance)
            {
                UFunctionCall remove(*instance, STR("Remove"));
                if (auto* actors = remove ? remove.param<bool>(STR("bRemoveActors")) : nullptr)
                {
                    *actors = true;
                    remove.call();
                }
            }
            if (auto* flag = property<bool>(base, STR("AnchorRemoved"))) *flag = true;
            if (auto* grabbed = property<bool>(base, STR("SuccessfullyGrabbed"))) *grabbed = true;
            // Взятие идёт через Highlight основания и выдаёт новый предмет из ItemToGrab (тест 2026-11-06 08:24: после снятия
            // ItemInstance Host всё равно взял якорь) — убираем точку взятия, сам якорь и луч; «вилка» (SM_Anchor_Base) остаётся
            int removed = 0;
            for (auto* name : {STR("Highlight"), STR("GrabbableMesh"), STR("NS_AnchorBeam"), STR("NS_Anchor_Ambient")})
            {
                auto** comp = property<UObject*>(base, name);
                if (!comp || !*comp) continue;
                UFunctionCall destroy(*comp, STR("K2_DestroyComponent"));
                if (auto* obj = destroy ? destroy.param<UObject*>(STR("Object")) : nullptr)
                {
                    *obj = *comp;
                    destroy.call();
                    ++removed;
                }
            }
            Output::send<LogLevel::Verbose>(STR("[Anchor] у основания убрано компонентов: {} из 4\n"), removed);
            g_taken[base] = true; // не принять за своё взятие
            if (!g_is_host) return;
            // Host решает за зону: «возмущение» от взятия якоря (аномалии) — как если бы взял он сам
            auto* type = property<uint8_t>(base, STR("AnchorType"));
            auto* size = property<uint8_t>(base, STR("AnchorSize"));
            UObject* hazards = UObjectGlobals::FindFirstOf(STR("BP_HazardManager_C"));
            UFunctionCall fn(hazards, STR("CauseHazardDisturbance"));
            auto* p_type = fn ? fn.param<uint8_t>(STR("Type")) : nullptr;
            auto* p_size = fn ? fn.param<uint8_t>(STR("Size")) : nullptr;
            if (!p_type || !p_size || !type || !size)
            {
                Output::send<LogLevel::Warning>(STR("[Anchor] возмущение зоны не вызвано: менеджер {}, функция {}, параметры Type/Size {}/{}, у основания AnchorType/AnchorSize {}/{}\n"),
                                                hazards ? STR("есть") : STR("НЕТ"), fn ? STR("есть") : STR("НЕТ"), p_type ? STR("есть") : STR("НЕТ"),
                                                p_size ? STR("есть") : STR("НЕТ"), type ? STR("есть") : STR("НЕТ"), size ? STR("есть") : STR("НЕТ"));
                return;
            }
            *p_type = *type;
            *p_size = *size;
            fn.call();
            Output::send<LogLevel::Verbose>(STR("[Anchor] возмущение зоны вызвано: тип {}, размер {}\n"), *type, *size);
        }
    } // namespace

    void on_map_loaded()
    {
        g_loaded_ms = now_ms();
        g_taken.clear();
    }

    void on_anchor(const Coop::AnchorBody& body)
    {
        UObject* best = nullptr;
        float best_d2 = static_cast<float>(Coop::AnchorMatchCm) * Coop::AnchorMatchCm;
        for (auto* base : bases())
        {
            Vec p = location(base);
            float dx = p.X - body.X, dy = p.Y - body.Y, dz = p.Z - body.Z, d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2) best = base, best_d2 = d2;
        }
        if (!best)
        {
            Output::send<LogLevel::Verbose>(STR("[Anchor] собеседник взял якорь у ({}, {}, {}), у меня там основания нет\n"), body.X, body.Y, body.Z);
            return;
        }
        if (taken(best))
        {
            // Оба взяли почти одновременно — у каждого свой якорь; Host не отбирает (shortcut: редкий случай, отбирать — если станет мешать)
            Output::send<LogLevel::Verbose>(STR("[Anchor] этот якорь я тоже уже взял — оставляю\n"));
            g_taken[best] = true;
            return;
        }
        take_here(best);
        Output::send<LogLevel::Verbose>(STR("[Anchor] {} взял якорь у ({}, {}, {}) — у меня снят\n"), g_is_host ? STR("пассажир") : STR("Host"), body.X, body.Y, body.Z);
    }

    void tick(bool is_host)
    {
        g_is_host = is_host;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs) return;
        g_last_poll_ms = now;
        bool settled = now - g_loaded_ms >= SettleMs;
        for (auto* base : bases())
        {
            bool now_taken = taken(base);
            auto it = g_taken.find(base);
            // впервые видим основание или карта ещё грузится — только запоминаем: взятое до нас взято в сейве
            if (it == g_taken.end() || !settled || !Net::connected())
            {
                g_taken[base] = now_taken;
                continue;
            }
            if (now_taken && !it->second)
            {
                Vec p = location(base);
                if (!std::isnan(p.X))
                {
                    Net::send_anchor({static_cast<int32_t>(std::lround(p.X)), static_cast<int32_t>(std::lround(p.Y)), static_cast<int32_t>(std::lround(p.Z))});
                    Output::send<LogLevel::Verbose>(STR("[Anchor] я взял якорь у ({:.0f}, {:.0f}, {:.0f}) → сообщаю собеседнику\n"), static_cast<double>(p.X),
                                                    static_cast<double>(p.Y), static_cast<double>(p.Z));
                }
            }
            it->second = now_taken;
        }
    }
} // namespace AnchorSync
