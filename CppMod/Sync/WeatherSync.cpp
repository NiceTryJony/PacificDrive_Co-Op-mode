#include "WeatherSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

        UObject* director() { return UObjectGlobals::FindFirstOf(STR("BP_ToDDirector_C")); }

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

        UObject* g_tod = nullptr; // кэш BP_ToDManager_C на кадр: обновляется раз в PollMs, сбрасывается при загрузке карты

        UObject* manager() { return UObjectGlobals::FindFirstOf(STR("BP_ToDManager_C")); }

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
    } // namespace

    void on_map_loaded()
    {
        g_loaded_ms = now_ms();
        g_sent = "\x01";
        g_have_host = false; // погода прошлой карты больше не действует — ждём новую от Host-а
        g_have_levels = false;
        g_tod = nullptr;
        g_levels_sent_ms = 0;
    }

    void on_weather(const std::string& tag, const std::string& path)
    {
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
        // Client: уровни Host-а каждый кадр — игра пересчитывает свои тоже каждый кадр
        if (!is_host && g_tod && g_have_levels && Net::connected()) write_levels(g_tod);
        if (!Net::connected() || now - g_last_poll_ms < PollMs || now - g_loaded_ms < SettleMs) return;
        g_last_poll_ms = now;
        g_tod = manager();
        if (g_tod && !is_host) install_levels_hook(g_tod);
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
