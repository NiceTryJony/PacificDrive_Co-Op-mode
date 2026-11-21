#include "WeatherSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Items.h"
#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>

using namespace RC;
using namespace RC::Unreal;

namespace WeatherSync
{
    namespace
    {
        constexpr uint64_t PollMs = 1000, SettleMs = 5000, ResendMs = 10'000, RetryMs = 5000;

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        uint64_t g_loaded_ms = 0, g_last_poll_ms = 0, g_sent_ms = 0, g_last_try_ms = 0;
        std::string g_sent = "\x01"; // Host: что уже отправлено (заведомо не путь — первая отправка обязательна)
        std::string g_host;          // Client: погода Host-а
        bool g_have_host = false;

        UObject* director() { static Cached::First f{STR("BP_ToDDirector_C")}; return f.get(); }

        // Текущее событие погоды: эффект WEV_* на служебном предмете директора; "" — события нет
        std::string current(UObject* dir)
        {
            auto** item = property<UObject*>(dir, STR("IA_ActiveTODEventItem"));
            if (!item || !*item) return {};
            for (auto& effect : Items::effects_of(*item))
                if (effect.find(".WEV_") != std::string::npos) return effect;
            return {};
        }

        std::wstring short_name(const std::string& path) { return path.empty() ? L"(нет)" : Items::wide(path.substr(path.rfind('.') + 1)); }

        // Client: привести свою погоду к Host-у. Своё событие снимаем, чужое запускаем; совпадение проверит следующий опрос
        void follow_host(UObject* dir, const std::string& mine)
        {
            if (!mine.empty())
                if (UFunctionCall clear(dir, STR("ClearCurrentTODEvent")); clear) clear.call();
            if (g_host.empty())
            {
                Output::send<LogLevel::Verbose>(STR("[Weather] у Host погода без события — снимаю своё {}\n"), short_name(mine));
                return;
            }
            auto* cls = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, Items::wide(g_host).c_str());
            UFunctionCall start(dir, STR("StartTODEvent"));
            auto* p = start ? start.param<UObject*>(STR("TODEvent To Start")) : nullptr;
            if (!cls || !p)
            {
                Output::send<LogLevel::Warning>(STR("[Weather] не запустить {}: класс {}, StartTODEvent {}\n"), short_name(g_host), cls ? STR("есть") : STR("НЕ ЗАГРУЖЕН"),
                                                p ? STR("есть") : STR("НЕТ"));
                return;
            }
            *p = cls;
            start.call();
            Output::send<LogLevel::Verbose>(STR("[Weather] погода как у Host: {} → {} (теперь {})\n"), short_name(mine), short_name(g_host), short_name(current(dir)));
        }

        // Уровни погоды BP_ToDManager_C (тест 0.17.7 2026-11-08): при одном и том же WEV у Host дождь 0.5–1.0 с молниями, у Client 0.0 —
        // дождь складывается и из других модификаторов (TargetWeatherOverrideArray), не только события. Host шлёт текущие уровни,
        // Client подменяет их в SetCurrentWeatherOverrideValues (через неё их пишет игра — предположение) и пишет в свойства сам
        constexpr const TCHAR* LevelProps[] = {STR("TOD_Override_Rain_Current"), STR("TOD_Override_Fog_Current"), STR("TOD_Override_Wind_Current"),
                                               STR("TOD_Override_Darkness_Current"), STR("TOD Override Zone Storm Current"), STR("TOD Override Acid Current")};
        constexpr const TCHAR* LevelParams[] = {STR("TOD_Override_Rain_Current"), STR("TOD_Override_Fog_Current"), STR("TOD_Override_Wind_Current"),
                                                STR("TOD_Override_Darkness_Current"), STR("TOD_Override_ZoneStorm_Current"), STR("TOD_Override_Acid_Current")};
        constexpr size_t LevelCount = std::size(LevelProps);
        constexpr float LevelStep = 0.03f;
        float g_levels[LevelCount]{}, g_sent_levels[LevelCount]{};
        bool g_have_levels = false, g_levels_hooked = false;
        uint64_t g_levels_sent_ms = 0, g_levels_log_ms = 0;
        size_t g_param_offset[LevelCount];
        int g_set_calls = 0; // сколько раз игра звала SetCurrentWeatherOverrideValues — в лог, чтобы понять, через неё ли она пишет

        FWeakObjectPtr g_tod_weak; // g_tod между опросами могут уничтожить — см. Cached::drop_if_dead
        UObject* g_tod = nullptr; // кэш BP_ToDManager_C на кадр: обновляется раз в PollMs, сбрасывается при загрузке карты

        UObject* manager() { static Cached::First f{STR("BP_ToDManager_C")}; return f.get(); }

        bool read_levels(UObject* tod, float out[LevelCount])
        {
            for (size_t i = 0; i < LevelCount; ++i)
            {
                auto* v = property<float>(tod, LevelProps[i]);
                if (!v) return false;
                out[i] = *v;
            }
            return true;
        }

        void write_levels(UObject* tod)
        {
            for (size_t i = 0; i < LevelCount; ++i)
                if (auto* v = property<float>(tod, LevelProps[i])) *v = g_levels[i];
        }

        void install_levels_hook(UObject* tod)
        {
            if (g_levels_hooked) return;
            g_levels_hooked = true;
            UFunction* fn = tod->GetFunctionByNameInChain(STR("SetCurrentWeatherOverrideValues"));
            if (!fn)
            {
                Output::send<LogLevel::Warning>(STR("[Weather] нет SetCurrentWeatherOverrideValues — уровни пишу только в свойства\n"));
                return;
            }
            size_t found = 0;
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
                for (size_t i = 0; i < LevelCount; ++i)
                    if (p->GetName() == LevelParams[i]) g_param_offset[i] = static_cast<size_t>(p->GetOffset_ForInternal()), ++found;
            if (found != LevelCount)
            {
                Output::send<LogLevel::Warning>(STR("[Weather] у SetCurrentWeatherOverrideValues найдено {} из {} параметров — перехват не ставлю\n"), found, LevelCount);
                return;
            }
            UObjectGlobals::RegisterHook(
                    tod->GetClassPrivate()->GetPathName() + STR(":SetCurrentWeatherOverrideValues"),
                    [](UnrealScriptFunctionCallableContext& ctx, void*) {
                        ++g_set_calls;
                        auto* locals = static_cast<uint8_t*>(ctx.TheStack.Locals());
                        if (!g_have_levels || !locals || !Net::connected()) return;
                        for (size_t i = 0; i < LevelCount; ++i) *reinterpret_cast<float*>(locals + g_param_offset[i]) = g_levels[i];
                    },
                    [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
            Output::send<LogLevel::Verbose>(STR("[Weather] перехват SetCurrentWeatherOverrideValues установлен\n"));
        }

        std::wstring levels_text(const float v[LevelCount])
        {
            std::wstring s;
            for (size_t i = 0; i < LevelCount; ++i) s += std::format(L"{}{:.2f}", i ? L" " : L"", v[i]);
            return s;
        }

        // События директора GEV_* (буря, кислотный шквал, сейсмика…, docs/recon/reference/gameplay-events.md): у каждого ПК свой
        // BP_GameplayEventDirector_C со случайным таймером (тест 2026-11-17: буря у Host 06:35, у Client своя 06:37). Client выключает
        // свой директор (ForceNoEvents) и повторяет событие Host-а (Internal_StartNewEvent), зону события ставит туда же, где у Host
        struct Vec
        {
            float X, Y, Z;
        };
        constexpr float GevMoveCm = 300; // Host: зона сдвинулась на столько — сообщить; Client: разошлась на столько — поправить
        std::string g_gev_sent = "\x01", g_gev_host;
        Vec g_gev_sent_at{NAN, NAN, NAN}, g_gev_host_at{NAN, NAN, NAN};
        bool g_have_gev = false, g_forced_no_events = false;
        uint64_t g_gev_sent_ms = 0, g_gev_try_ms = 0;

        UObject* event_director() { static Cached::First f{STR("BP_GameplayEventDirector_C")}; return f.get(); }

        UObject* current_event(UObject* dir)
        {
            auto** gev = property<UObject*>(dir, STR("CurrentEvent"));
            return gev ? *gev : nullptr;
        }

        std::string class_path(UObject* object) { return object ? Items::narrow(object->GetClassPrivate()->GetPathName()) : std::string{}; }

        UObject* event_zone(UObject* gev)
        {
            auto** actor = gev ? property<UObject*>(gev, STR("LocationEventActor")) : nullptr;
            return actor ? *actor : nullptr;
        }

        Vec actor_location(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {NAN, NAN, NAN};
            fn.call();
            return *ret;
        }

        float distance(const Vec& a, const Vec& b) { return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z)); }

        // Класс события: у Client он может быть не загружен (директор грузит выбранное асинхронно) — Items::find_or_load
        UClass* load_class(const std::string& path) { return static_cast<UClass*>(Items::find_or_load(path)); }

        void gev_host(UObject* dir, uint64_t now)
        {
            UObject* gev = current_event(dir);
            std::string mine = class_path(gev);
            Vec at = actor_location(event_zone(gev)); // нет зоны — NaN
            bool moved = !std::isnan(at.X) && (std::isnan(g_gev_sent_at.X) || distance(at, g_gev_sent_at) >= GevMoveCm);
            if (mine == g_gev_sent && !moved && now - g_gev_sent_ms < ResendMs) return;
            if (mine != g_gev_sent)
                Output::send<LogLevel::Verbose>(STR("[Gev] моё событие: {} у ({:.0f}, {:.0f}, {:.0f}) → отправляю\n"), short_name(mine), at.X, at.Y, at.Z);
            char text[512];
            std::snprintf(text, sizeof text, "%s %.0f %.0f %.0f", mine.empty() ? "-" : mine.c_str(), at.X, at.Y, at.Z);
            Net::send_part("gev", text, Coop::WeatherEvent);
            g_gev_sent = mine;
            g_gev_sent_at = at;
            g_gev_sent_ms = now;
        }

        void gev_client(UObject* dir, uint64_t now)
        {
            if (auto* off = property<bool>(dir, STR("ForceNoEvents")); off && !*off)
            {
                *off = true; // свои события не начинать — ждём Host-а; снимается при отключении
                g_forced_no_events = true;
                Output::send<LogLevel::Verbose>(STR("[Gev] директор событий выключен (ForceNoEvents) — события ведёт Host\n"));
            }
            if (!g_have_gev) return;
            UObject* gev = current_event(dir);
            std::string mine = class_path(gev);
            if (mine != g_gev_host)
            {
                if (now - g_gev_try_ms < RetryMs) return;
                g_gev_try_ms = now;
                // Снять своё: ClearCurrentEvent только забывает событие — зона живёт и едет дальше; RemoveFrom события уводит зону
                // в (0,0,0) и потом удаляет, но CurrentEvent не трогает — нужны оба (проверено в зоне 2026-11-17 через мост)
                if (gev)
                {
                    if (UFunctionCall remove(gev, STR("RemoveFrom")); remove) remove.call();
                    if (UFunctionCall clear(dir, STR("ClearCurrentEvent")); clear) clear.call();
                }
                if (g_gev_host.empty())
                {
                    Output::send<LogLevel::Verbose>(STR("[Gev] у Host события нет — снимаю своё {}\n"), short_name(mine));
                    return;
                }
                UClass* cls = load_class(g_gev_host);
                UFunctionCall start(dir, STR("Internal_StartNewEvent"));
                auto* p = start ? start.param<UClass*>(STR("New Event")) : nullptr;
                if (cls && p) *p = cls, start.call();
                Output::send<LogLevel::Verbose>(STR("[Gev] событие как у Host: {} → {}: {}\n"), short_name(mine), short_name(g_gev_host),
                                                !cls ? STR("класс НЕ ЗАГРУЖЕН") : !p ? STR("нет Internal_StartNewEvent") : STR("запущено"));
                return;
            }
            UObject* zone = event_zone(gev);
            if (!zone || std::isnan(g_gev_host_at.X)) return;
            Vec at = actor_location(zone);
            if (!std::isnan(at.X) && distance(at, g_gev_host_at) < GevMoveCm) return;
            UFunctionCall move(zone, STR("K2_SetActorLocation"));
            auto* p_loc = move ? move.param<Vec>(STR("NewLocation")) : nullptr;
            auto* p_teleport = move ? move.param<bool>(STR("bTeleport")) : nullptr;
            if (!p_loc || !p_teleport) return;
            *p_loc = g_gev_host_at;
            *p_teleport = true;
            move.call();
            static uint64_t logged_ms = 0;
            if (now - logged_ms >= ResendMs)
                logged_ms = now, Output::send<LogLevel::Verbose>(STR("[Gev] зона события ({:.0f}, {:.0f}, {:.0f}) → как у Host ({:.0f}, {:.0f}, {:.0f})\n"), at.X, at.Y, at.Z,
                                                                g_gev_host_at.X, g_gev_host_at.Y, g_gev_host_at.Z);
        }

        // ---- время суток и север (чужой мод pacific-together, docs/recon/reference/pacific-together.md; у нас не проверено) ----
        // Время суток — тег WB.WorldState.TimeOfDay глобальной доски (0..1), у каждого ПК идёт по своим часам. Север — случайный
        // поворот карты на 0/90/180/270° (RandomizeNorth), у Client без общего сейва свой. Host шлёт оба раз в TodSendMs, Client
        // правит, только если разошлись больше допуска — чтобы не дёргать каждую секунду
        constexpr uint64_t TodSendMs = 2000;
        constexpr float TodTolerance = 0.003f, NorthTolerance = 0.5f;
        struct Rot
        {
            float Pitch, Yaw, Roll;
        };
        float g_tod_host = NAN, g_north_host = NAN, g_tod_sent = NAN, g_north_sent = NAN;
        bool g_have_tod = false;
        uint64_t g_tod_sent_ms = 0, g_tod_log_ms = 0;
        int g_tod_sets = 0;

        UObject* map_bounds() { static Cached::First f{STR("BP_GobblerMapBounds_C")}; return f.get(); }

        bool read_tod(UObject* tod, float& time, float& yaw)
        {
            UFunctionCall get(tod, STR("Get Current Time Of Day"));
            auto* ret = get ? get.param<float>(STR("Returned Time Of Day")) : nullptr;
            auto* north = property<Rot>(tod, STR("Current North As Rotator"));
            if (!ret || !north) return false;
            get.call();
            time = *ret;
            yaw = north->Yaw;
            return true;
        }

        float angle_gap(float a, float b)
        {
            float d = std::fmod(std::fabs(a - b), 360.0f);
            return std::min(d, 360.0f - d);
        }

        void set_north(UObject* tod, float yaw)
        {
            const float rad = yaw * 3.14159265f / 180.0f;
            UFunctionCall set(tod, STR("SetTrueNorth"));
            auto* p_vec = set ? set.param<Vec>(STR("New North Vector")) : nullptr;
            auto* p_rot = set ? set.param<Rot>(STR("New North Rotator")) : nullptr;
            if (!p_vec || !p_rot) return;
            *p_vec = {std::cos(rad), std::sin(rad), 0};
            *p_rot = {0, yaw, 0};
            set.call();
            // как RandomizeNorth(true): солнце поворачивается вместе с севером
            auto** sim = property<UObject*>(tod, STR("ToDSimulationActor"));
            if (sim && *sim)
                if (UFunctionCall turn(*sim, STR("K2_SetActorRotation")); turn)
                    if (auto* p_new = turn.param<Rot>(STR("NewRotation")))
                        if (auto* p_tp = turn.param<bool>(STR("bTeleportPhysics"))) *p_new = {0, yaw, 0}, *p_tp = false, turn.call();
            // картинка карты ARC рисуется заново, иначе она остаётся повёрнутой по-старому
            if (UObject* bounds = map_bounds())
                if (UFunctionCall cap(bounds, STR("Capture Configure")); cap)
                    if (auto* p_re = cap.param<bool>(STR("Recapture"))) *p_re = true, cap.call();
        }

        // Client: симуляция времени суток теряет связь с менеджером (у чужого мода: Host в дожде, гость сухой). Связь — актёр с тегом
        // ToDSim (SetupToDSimulationActor); пресет симуляции должен совпадать с целевым пресетом менеджера (WeatherChangeEvent).
        // Лечим только расхождение; в лог — раз в RetryMs
        uint64_t g_relink_ms = 0;
        void relink_sim(UObject* tod, uint64_t now)
        {
            auto** sim = property<UObject*>(tod, STR("ToDSimulationActor"));
            if (!sim || now - g_relink_ms < RetryMs) return;
            if (!*sim || !Cached::settled(*sim))
            {
                g_relink_ms = now;
                if (UFunctionCall setup(tod, STR("SetupToDSimulationActor")); setup) setup.call();
                Output::send<LogLevel::Verbose>(STR("[Weather] у менеджера нет симуляции времени суток — SetupToDSimulationActor\n"));
                return;
            }
            auto** want = property<UObject*>(tod, STR("Target TOD Preset"));
            auto** cur = property<UObject*>(*sim, STR("CurrentPresetObject"));
            if (!want || !*want || !cur || !*cur || (*cur)->GetClassPrivate() == static_cast<UClass*>(*want)) return;
            g_relink_ms = now;
            UFunctionCall change(*sim, STR("WeatherChangeEvent"));
            if (auto* p = change ? change.param<UObject*>(STR("Weather Weighting")) : nullptr) *p = *want, change.call();
            Output::send<LogLevel::Verbose>(STR("[Weather] пресет симуляции {} ≠ целевому {} — WeatherChangeEvent\n"), short_name(Items::narrow((*cur)->GetClassPrivate()->GetPathName())),
                                            short_name(Items::narrow((*want)->GetPathName())));
        }

        void tod_host(UObject* tod, uint64_t now)
        {
            if (now - g_tod_sent_ms < TodSendMs) return;
            float time, yaw;
            if (!read_tod(tod, time, yaw)) return;
            char text[64];
            std::snprintf(text, sizeof text, "%.4f %.2f", time, yaw);
            Net::send_part("tod", text, Coop::WeatherEvent);
            g_tod_sent_ms = now;
            if (std::isnan(g_north_sent) || angle_gap(yaw, g_north_sent) > NorthTolerance)
                Output::send<LogLevel::Verbose>(STR("[Weather] время суток {:.3f}, север {:.1f}° → отправляю\n"), time, yaw);
            g_tod_sent = time, g_north_sent = yaw;
        }

        void tod_client(UObject* tod, uint64_t now)
        {
            float time, yaw;
            if (!read_tod(tod, time, yaw)) return;
            float gap = std::fabs(time - g_tod_host);
            gap = std::min(gap, 1.0f - gap);
            if (gap > TodTolerance)
                if (UFunctionCall set(tod, STR("Set Current Time Of Day")); set)
                    if (auto* p = set.param<float>(STR("New Time Of Day"))) *p = g_tod_host, set.call(), ++g_tod_sets;
            if (angle_gap(yaw, g_north_host) > NorthTolerance)
            {
                set_north(tod, g_north_host);
                Output::send<LogLevel::Verbose>(STR("[Weather] север {:.1f}° → как у Host {:.1f}°\n"), yaw, g_north_host);
            }
            if (now - g_tod_log_ms >= ResendMs)
            {
                g_tod_log_ms = now;
                Output::send<LogLevel::Verbose>(STR("[Weather] время суток: у меня {:.3f}, у Host {:.3f}; поправлено {} раз за 10 с\n"), time, g_tod_host, g_tod_sets);
                g_tod_sets = 0;
            }
        }
    } // namespace

    void on_map_loaded()
    {
        g_have_tod = false;
        g_tod_sent_ms = 0;
        g_tod_sent = g_north_sent = NAN;
        g_loaded_ms = now_ms();
        g_sent = "\x01";
        g_have_host = false; // погода прошлой карты больше не действует — ждём новую от Host-а
        g_have_levels = false;
        g_tod = nullptr;
        g_levels_sent_ms = 0;
        g_gev_sent = "\x01";
        g_gev_sent_at = {NAN, NAN, NAN};
        g_have_gev = false;
    }

    void on_weather(const std::string& tag, const std::string& path)
    {
        if (tag == "gev")
        {
            char cls[400];
            Vec at{NAN, NAN, NAN};
            if (std::sscanf(path.c_str(), "%399s %f %f %f", cls, &at.X, &at.Y, &at.Z) < 1) return;
            std::string host = std::strcmp(cls, "-") == 0 ? std::string{} : std::string(cls);
            if (!g_have_gev || host != g_gev_host) Output::send<LogLevel::Verbose>(STR("[Gev] Host: {}\n"), short_name(host));
            g_gev_host = host;
            g_gev_host_at = at; // "nan" sscanf читает как NaN — зоны нет
            g_have_gev = true;
            return;
        }
        if (tag == "tod")
        {
            float time, yaw;
            if (std::sscanf(path.c_str(), "%f %f", &time, &yaw) != 2) return;
            g_tod_host = time, g_north_host = yaw, g_have_tod = true;
            return;
        }
        if (tag == "lvl")
        {
            float v[LevelCount];
            if (std::sscanf(path.c_str(), "%f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != LevelCount) return;
            std::copy(std::begin(v), std::end(v), g_levels);
            g_have_levels = true;
            return;
        }
        if (!g_have_host || g_host != path) Output::send<LogLevel::Verbose>(STR("[Weather] Host: {}\n"), short_name(path));
        g_host = path;
        g_have_host = true;
    }

    void tick(bool is_host)
    {
        uint64_t now = now_ms();
        Cached::drop_if_dead(g_tod, g_tod_weak);
        // Client: уровни Host-а каждый кадр — игра пересчитывает свои тоже каждый кадр
        if (!is_host && g_tod && g_have_levels && Net::connected()) write_levels(g_tod);
        if (g_forced_no_events && !Net::connected())
            if (auto* off = property<bool>(event_director(), STR("ForceNoEvents")))
            {
                *off = false; // напарник ушёл — свой директор событий снова работает
                g_forced_no_events = false;
                Output::send<LogLevel::Verbose>(STR("[Gev] связи нет — директор событий снова включён\n"));
            }
        if (!Net::connected() || now - g_last_poll_ms < PollMs || now - g_loaded_ms < SettleMs) return;
        g_last_poll_ms = now;
        if (UObject* gev_dir = event_director()) is_host ? gev_host(gev_dir, now) : gev_client(gev_dir, now);
        g_tod = manager();
        Cached::track(g_tod, g_tod_weak);
        if (g_tod && !is_host) install_levels_hook(g_tod);
        if (g_tod && !is_host) relink_sim(g_tod, now);
        if (g_tod) is_host ? tod_host(g_tod, now) : (g_have_tod ? tod_client(g_tod, now) : void());
        if (g_tod && is_host)
        {
            float v[LevelCount];
            bool changed = false;
            if (read_levels(g_tod, v))
            {
                for (size_t i = 0; i < LevelCount; ++i) changed |= std::fabs(v[i] - g_sent_levels[i]) >= LevelStep;
                if (changed || now - g_levels_sent_ms >= ResendMs)
                {
                    char text[128];
                    std::snprintf(text, sizeof text, "%.3f %.3f %.3f %.3f %.3f %.3f", v[0], v[1], v[2], v[3], v[4], v[5]);
                    Net::send_part("lvl", text, Coop::WeatherEvent);
                    std::copy(std::begin(v), std::end(v), g_sent_levels);
                    g_levels_sent_ms = now;
                }
            }
        }
        if (!is_host && g_tod && now - g_levels_log_ms >= ResendMs)
        {
            g_levels_log_ms = now;
            Output::send<LogLevel::Verbose>(STR("[Weather] уровни дождь/туман/ветер/темнота/буря/кислота: Host {}; Set вызывался {} раз за 10 с\n"),
                                            g_have_levels ? levels_text(g_levels) : std::wstring(L"(ещё нет)"), g_set_calls);
            g_set_calls = 0;
        }
        UObject* dir = director();
        if (!dir) return;
        std::string mine = current(dir);
        if (is_host)
        {
            if (mine == g_sent && now - g_sent_ms < ResendMs) return; // повтор раз в 10 с — для переподключившегося Client-а
            if (mine != g_sent) Output::send<LogLevel::Verbose>(STR("[Weather] моя погода: {} → отправляю\n"), short_name(mine));
            Net::send_part("wev", mine, Coop::WeatherEvent);
            g_sent = mine;
            g_sent_ms = now;
            return;
        }
        if (!g_have_host || mine == g_host || now - g_last_try_ms < RetryMs) return;
        g_last_try_ms = now;
        follow_host(dir, mine);
    }
} // namespace WeatherSync
