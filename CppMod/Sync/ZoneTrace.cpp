#include "ZoneTrace.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UClass.hpp>

#include "Game/UFunctionCall.h"

#include <Unreal/UObjectGlobals.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>

using namespace RC;
using namespace RC::Unreal;

namespace ZoneTrace
{
    namespace
    {
        constexpr uint64_t SettleMs = 15'000;   // после LoadMap карта ещё наполняется — это одинаково у обоих (сейв Host), не пишем
        constexpr int MaxLinesPerSecond = 40;   // всплеск (взрыв, стая) не должен раздуть лог; пропущенные считаем
        Hook::GlobalCallbackId g_begin{}, g_end{};
        const wchar_t* g_who = L"?";
        uint64_t g_quiet_until = UINT64_MAX;    // до первой загрузки карты (меню) не пишем
        uint64_t g_second = 0;
        int g_lines = 0, g_dropped = 0;
        std::unordered_set<AActor*> g_seen;     // EndPlay пишем только для тех, чьё появление записали

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        struct Vec // FVector в UE 4.27 — float (double давал мусор в координатах, тест 2026-11-06)
        {
            float X, Y, Z;
        };

        // Шум, который появляется постоянно и у обоих: звук, эффекты, декали, служебные актёры
        bool noise(const StringType& cls)
        {
            for (auto* part : {STR("Emitter"), STR("Niagara"), STR("Decal"), STR("Sound"), STR("AkAmbient"), STR("Camera"), STR("Light"), STR("FieldSystem"),
                               STR("GeometryCollection"), STR("PlayerController"), STR("HUD"), STR("Projectile_Debris"), STR("GroundSpline"), STR("RoadSpline"),
                               STR("Prefab"), STR("ManageChildrenActor"), STR("StaticMeshActor"), STR("SkeletalMeshActor"), STR("RuntimeVirtualTexture"),
                               STR("MetaEcosystemVolume"), STR("PFX_")})
                if (cls.find(part) != StringType::npos) return true;
            // растительность (BP_IF_Shrub/Oak/Pine…) подгружается по мере езды тысячами — одинаково у обоих
            return cls.starts_with(STR("BP_IF_"));
        }

        bool budget()
        {
            uint64_t sec = now_ms() / 1000;
            if (sec != g_second)
            {
                if (g_dropped) Output::send<LogLevel::Verbose>(STR("[Zone] {} пропущено строк за секунду: {}\n"), g_who, g_dropped);
                g_second = sec;
                g_lines = g_dropped = 0;
            }
            if (g_lines >= MaxLinesPerSecond)
            {
                ++g_dropped;
                return false;
            }
            ++g_lines;
            return true;
        }

        void on_begin(AActor* actor)
        {
            if (!actor || now_ms() < g_quiet_until) return;
            auto cls = actor->GetClassPrivate()->GetName();
            if (noise(cls) || !budget()) return;
            Vec at{NAN, NAN, NAN};
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            if (auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr)
            {
                fn.call();
                at = *ret;
            }
            g_seen.insert(actor);
            Output::send<LogLevel::Verbose>(STR("[Zone] {} + {} {} @ {:.0f} {:.0f} {:.0f}\n"), g_who, cls, actor->GetName(), static_cast<double>(at.X), static_cast<double>(at.Y),
                                            static_cast<double>(at.Z));
        }

        void on_end(AActor* actor)
        {
            if (!actor || !g_seen.erase(actor)) return;
            Output::send<LogLevel::Verbose>(STR("[Zone] {} - {} {}\n"), g_who, actor->GetClassPrivate()->GetName(), actor->GetName());
        }
    } // namespace

    void install(bool is_host)
    {
        g_who = is_host ? L"Host" : L"Client";
        g_begin = Hook::RegisterBeginPlayPostCallback([](Hook::TCallbackIterationData<void>&, AActor* actor) { on_begin(actor); },
                                                      {false, false, STR("PacificDriveCoop"), STR("ZoneTraceBegin")});
        g_end = Hook::RegisterEndPlayPostCallback([](Hook::TCallbackIterationData<void>&, AActor* actor, auto) { on_end(actor); },
                                                  {false, false, STR("PacificDriveCoop"), STR("ZoneTraceEnd")});
        Output::send<LogLevel::Verbose>(STR("[Zone] хуки BeginPlay {}, EndPlay {}\n"), g_begin ? STR("есть") : STR("НЕТ"), g_end ? STR("есть") : STR("НЕТ"));
    }

    void uninstall()
    {
        if (g_begin) Hook::UnregisterCallback(g_begin);
        if (g_end) Hook::UnregisterCallback(g_end);
    }

    // Срез «какие аномалии есть сейчас»: число актёров по классам с Hazard/VolCascade в имени. Сравнивает состояние, а не события
    // появления (Host доспавнивает зону минутами, Client получает её сразу из сейва — по событиям это не различить, RECON.md 6.30)
    void tick()
    {
        static uint64_t last = 0;
        static std::wstring last_line;
        uint64_t now = now_ms();
        if (now < g_quiet_until || now - last < 30'000) return;
        last = now;
        std::vector<UObject*> actors;
        UObjectGlobals::FindAllOf(STR("Actor"), actors);
        std::map<StringType, int> counts;
        for (auto* a : actors)
        {
            auto cls = a->GetClassPrivate()->GetName();
            if (cls.find(STR("Hazard")) != StringType::npos || cls.find(STR("VolCascade")) != StringType::npos) ++counts[cls];
        }
        std::wstring line;
        int total = 0;
        for (auto& [cls, n] : counts) line += std::format(L" {}x{}", cls, n), total += n;
        if (line == last_line) return;
        last_line = line;
        Output::send<LogLevel::Verbose>(STR("[Zone] {} hazards {}:{}\n"), g_who, total, line);
    }

    void on_map_loaded()
    {
        g_seen.clear(); // старая карта выгружена — указатели больше не наши
        g_quiet_until = now_ms() + SettleMs;
        Output::send<LogLevel::Verbose>(STR("[Zone] {} карта загружена, пишу появления через {} с\n"), g_who, SettleMs / 1000);
    }
} // namespace ZoneTrace
