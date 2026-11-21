#include "ZoneTrace.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UClass.hpp>

#include "Game/Cached.h"
#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/Notify.h"

#include <Unreal/UObjectGlobals.hpp>

#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <unordered_set>

using namespace RC;
using namespace RC::Unreal;

namespace ZoneTrace
{
    std::atomic<bool> g_mark_requested{false};

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
        // Срез перебирает всех актёров (рывок ~30 мс): только раз после загрузки карты и по метке Ctrl+F7 (своей или собеседника)
        bool g_census_due = false;
        std::wstring g_census_line;

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

        // Исчезновение — и тех, что пришли из сейва (сломанный щит, взятый предмет, разведка зоны B): у них появления в логе нет.
        // Выгрузку карты (массовый EndPlay) режет budget()
        void on_end(AActor* actor)
        {
            if (!actor) return;
            bool seen = g_seen.erase(actor);
            if (!seen && now_ms() < g_quiet_until) return;
            auto cls = actor->GetClassPrivate()->GetName();
            if (!seen && (noise(cls) || !budget())) return;
            Vec at{NAN, NAN, NAN};
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            if (auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr) fn.call(), at = *ret;
            Output::send<LogLevel::Verbose>(STR("[Zone] {} - {} {} @ {:.0f} {:.0f} {:.0f}\n"), g_who, cls, actor->GetName(), static_cast<double>(at.X), static_cast<double>(at.Y),
                                            static_cast<double>(at.Z));
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

    // Мимолётные актёры (эффекты, звук, камера) — в срезе только мешают; растительность и StaticMeshActor оставлены:
    // сейв показал, что машина их ломает (docs/sync/ZONE_RECON_PLAN.md, A1)
    static bool census_noise(const StringType& cls)
    {
        for (auto* part : {STR("Emitter"), STR("Niagara"), STR("Decal"), STR("Sound"), STR("AkAmbient"), STR("Camera"), STR("PlayerController"), STR("HUD"),
                           STR("FieldSystem"), STR("Projectile_Debris")})
            if (cls.find(part) != StringType::npos) return true;
        return false;
    }

    // Сверка зоны в игре (ZoneCensus): каждый срез уходит собеседнику, расхождения — в лог [ZoneDiff], стойкие (≥ 60 с) по аномалиям
    // и событиям — уведомлением на экран. Без классов, которые у двоих законно разные (A3, docs/sync/SYNC_MAP.md «Корзины зоны»):
    // помощники генерации у Host, инструмент в руках, растительность и декор по подгрузке у каждого свои
    static bool compare_noise(const StringType& cls)
    {
        if (cls == STR("Actor")) return true;
        // BP_ToDDirector.SetupTODEventItemAndActor создаёт новый IA_WeatherEventItem при каждой загрузке карты, не ища сохранённые (байткод 2026-11-18):
        // они копятся в сейве, а Client к сейву Host добавляет свой — всегда на один больше. На игру не влияет
        if (cls.ends_with(STR(":IA_WeatherEventItem"))) return true;
        for (auto* part : {STR("BP_IF_"), STR("BP_ClumpedSpawnMarker"), STR("BP_ClumpedArea"), STR("BP_SpawnPoint_C"), STR("PrefabSettingsActor"),
                           STR("BP_Spawn_PreHazardHelper"), STR("BP_Spawn_Resource"), STR("BP_PrefabSelfDeletor"), STR("PFX_"), STR("BP_Tool_Target_"),
                           STR("BP_HandItem_"), STR("BP_CarItemInstall"), STR("BP_AmbientEvent"), STR("PointLight"), STR("SpotLight"), STR("BP_CarScrapeFX"),
                           STR("BP_FoliageInstanceKillerVolume"), STR("BP_DebugLabel"), STR("BP_Vista_Tile"), STR("TriggerBox"), STR("Spline"), STR("BP_LeafSpawner"),
                           STR("StaticMeshActor"), STR("BP_Destruct_Branch"), STR("ParticleEventManager")})
            if (cls.find(part) != StringType::npos) return true;
        return false;
    }
    // в уведомление — только то, что влияет на игру
    static bool notify_worthy(const std::string& cls)
    {
        for (auto* part : {"Hazard", "Leech", "Anchor", "Portal", "Storm", "GameplayEvent"})
            if (cls.find(part) != std::string::npos) return true;
        return false;
    }
    constexpr uint64_t PeerFreshMs = 45'000, DiffNotifyMs = 60'000;
    static std::map<std::string, int> g_mine, g_peer;
    static std::string g_mine_map, g_peer_map;
    static uint64_t g_mine_ms = 0, g_peer_ms = 0;
    static std::map<std::string, uint64_t> g_diff_since; // класс → с какого момента расходится без перерыва
    static std::set<std::string> g_notified;             // уже показано на этой карте
    static std::string g_diff_line;

    static std::string current_map()
    {
        auto* player = UObjectGlobals::FindFirstOf(STR("PDP_MainCharacter_C")); // актёр персистентного уровня: внешний — ULevel, его внешний — UWorld
        auto* level = player ? player->GetOuterPrivate() : nullptr;
        auto* world = level ? level->GetOuterPrivate() : nullptr;
        return world ? Items::narrow(world->GetName()) : std::string{};
    }

    static std::string short_cls(std::string cls)
    {
        if (cls.starts_with("BP_")) cls = cls.substr(3);
        if (auto c = cls.find("_C"); c != std::string::npos && (c + 2 == cls.size() || cls[c + 2] == ':')) cls.erase(c, 2);
        return cls;
    }

    static void compare()
    {
        uint64_t now = now_ms();
        if (g_mine.empty() || g_peer.empty() || g_mine_map.empty() || g_mine_map != g_peer_map) return;
        if ((g_mine_ms > g_peer_ms ? g_mine_ms - g_peer_ms : g_peer_ms - g_mine_ms) > PeerFreshMs) return;
        bool host = g_who[0] == L'H';
        std::string line, notify;
        std::set<std::string> differ;
        auto keys = g_mine;
        for (auto& [cls, n] : g_peer) keys.emplace(cls, 0);
        for (auto& [cls, unused] : keys)
        {
            int mine = g_mine.contains(cls) ? g_mine[cls] : 0, peer = g_peer.contains(cls) ? g_peer[cls] : 0;
            if (mine == peer) continue;
            differ.insert(cls);
            g_diff_since.emplace(cls, now);
            int h = host ? mine : peer, c = host ? peer : mine;
            line += std::format(" {} {}/{}", cls, h, c);
            if (now - g_diff_since[cls] >= DiffNotifyMs && notify_worthy(cls) && g_notified.insert(cls).second && notify.size() < 80)
                notify += std::format("{}{} {}/{}", notify.empty() ? "" : ", ", short_cls(cls), h, c);
        }
        std::erase_if(g_diff_since, [&](auto& kv) { return !differ.contains(kv.first); });
        if (line != g_diff_line)
        {
            g_diff_line = line;
            Output::send<LogLevel::Verbose>(STR("[ZoneDiff] {} (Host/Client), классов {}:{}\n"), g_who, differ.size(), Items::wide(line.empty() ? std::string(" нет расхождений") : line));
        }
        if (!notify.empty()) Notify::push(Notify::Kind::Warn, "Zone differs: " + notify);
    }

    static void compare_send(const std::map<StringType, int>& counts)
    {
        g_mine.clear();
        std::string text;
        for (auto& [cls, n] : counts)
        {
            if (compare_noise(cls)) continue;
            auto name = Items::narrow(cls);
            g_mine[name] = n;
            text += std::format("{}{}={}", text.empty() ? "" : " ", name, n);
        }
        g_mine_map = current_map();
        g_mine_ms = now_ms();
        if (!Net::connected()) return;
        if (!Net::send_part(g_mine_map, text, Coop::ZoneCensus)) Output::send<LogLevel::Warning>(STR("[ZoneDiff] срез длиннее пакета — не отправлен\n"));
        compare();
    }

    // Срез «что есть в зоне сейчас»: число актёров по всем классам (A2 разведки зоны). Сравнивает состояние, а не события
    // появления (Host доспавнивает зону минутами, Client получает её сразу из сейва — по событиям это не различить, RECON.md 6.30).
    // Сравнение двух ПК — tools/compare_census.py
    // Погода этого ПК (WEV_* на IA_ActiveTODEventItem директора, GEV_* директора событий, пресет и уровни) раз в 10 с при изменении —
    // по ней видно, держит ли WeatherSync погоду Host-а. shortcut: только разведка, убрать, когда погода пару тестов подряд совпадает
    static StringType class_name(UObject* object) { return object ? object->GetClassPrivate()->GetName() : StringType(STR("-")); }
    static StringType class_ref(UObject* object, const TCHAR* prop)
    {
        auto** cls = property<UObject*>(object, prop); // TSubclassOf — указатель на UClass
        return cls && *cls ? (*cls)->GetName() : StringType(STR("-"));
    }
    static void weather_probe()
    {
        static uint64_t last = 0;
        static std::wstring last_line;
        uint64_t now = now_ms();
        if (now < g_quiet_until || now - last < 10'000) return;
        last = now;
        UObject* tod_dir = UObjectGlobals::FindFirstOf(STR("BP_ToDDirector_C"));
        UObject* tod = UObjectGlobals::FindFirstOf(STR("BP_ToDManager_C"));
        UObject* gev_dir = UObjectGlobals::FindFirstOf(STR("BP_GameplayEventDirector_C"));
        std::wstring wev;
        if (auto** item = property<UObject*>(tod_dir, STR("IA_ActiveTODEventItem")); item && *item)
            for (auto& e : Items::effects_of(*item)) wev += std::wstring(e.begin() + e.rfind('.') + 1, e.end()) + L" ";
        auto f = [&](const TCHAR* name) { auto* v = property<float>(tod, name); return v ? std::round(*v * 10) / 10 : -1.0f; };
        auto* is_day = property<bool>(tod, STR("IsDay"));
        auto** gev = property<UObject*>(gev_dir, STR("CurrentEvent"));
        std::wstring line = std::format(L"WEV [{}] GEV {} пресет {} дождь {:.1f} туман {:.1f} темнота {:.1f} {}", wev, class_name(gev ? *gev : nullptr),
                                        class_ref(tod, STR("Target TOD Preset")), f(STR("TOD_Override_Rain_Current")), f(STR("TOD_Override_Fog_Current")),
                                        f(STR("TOD_Override_Darkness_Current")), is_day ? (*is_day ? L"день" : L"ночь") : L"?");
        if (line == last_line) return;
        last_line = line;
        Output::send<LogLevel::Verbose>(STR("[Weather] {} {}\n"), g_who, line);
    }

    void tick()
    {
        weather_probe();
        if (g_mark_requested.exchange(false))
        {
            Vec at{NAN, NAN, NAN};
            UFunctionCall fn(UObjectGlobals::FindFirstOf(STR("PDP_MainCharacter_C")), STR("K2_GetActorLocation"));
            if (auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr) fn.call(), at = *ret;
            static int marks = 0;
            Output::send<LogLevel::Verbose>(STR("[Mark] {} МЕТКА #{} у ({:.0f}, {:.0f}, {:.0f})\n"), g_who, ++marks, static_cast<double>(at.X), static_cast<double>(at.Y),
                                            static_cast<double>(at.Z));
            if (Net::connected() && !std::isnan(at.X))
                Net::send_anchor({static_cast<int32_t>(std::lround(at.X)), static_cast<int32_t>(std::lround(at.Y)), static_cast<int32_t>(std::lround(at.Z))}, Coop::Mark);
            Notify::push(Notify::Kind::Info, "Mark saved to the log");
            g_census_due = true;
            g_census_line.clear(); // срез в лог, даже если ничего не изменилось
        }
        if (!g_census_due || now_ms() < g_quiet_until) return;
        g_census_due = false;
        std::vector<UObject*> actors;
        Cached::find_settled(STR("Actor"), actors);
        std::map<StringType, int> counts;
        for (auto* a : actors)
        {
            auto cls = a->GetClassPrivate()->GetName();
            if (census_noise(cls)) continue;
            // предметы событий различаются только архетипом (у Client в зоне на один больше, RECON.md 6.53) — считаем по архетипам
            if (cls == STR("BP_GameplayEventItem_World_C"))
                if (auto** inst = property<UObject*>(a, STR("Instance")); inst && *inst)
                {
                    auto path = Items::archetype_path(*inst);
                    cls += STR(":") + Items::wide(path.substr(path.rfind('.') + 1));
                }
            ++counts[cls];
        }
        compare_send(counts);
        std::wstring line;
        int total = 0;
        for (auto& [cls, n] : counts) line += std::format(L" {}x{}", cls, n), total += n;
        if (line == g_census_line) return;
        g_census_line = line;
        Output::send<LogLevel::Verbose>(STR("[Zone] {} census {} {}:{}\n"), g_who, counts.size(), total, line);
    }

    void on_peer_census(const std::string& map, const std::string& text)
    {
        g_peer.clear();
        for (size_t at = 0; at < text.size();)
        {
            size_t end = text.find(' ', at);
            if (end == std::string::npos) end = text.size();
            auto item = text.substr(at, end - at);
            if (auto eq = item.rfind('='); eq != std::string::npos) g_peer[item.substr(0, eq)] = std::atoi(item.c_str() + eq + 1);
            at = end + 1;
        }
        g_peer_map = map;
        g_peer_ms = now_ms();
        compare(); // метка Ctrl+F7: свежие срезы у обоих — сравнить сразу
    }

    void on_peer_mark(const Coop::AnchorBody& at)
    {
        Output::send<LogLevel::Verbose>(STR("[Mark] {} метка собеседника у ({}, {}, {})\n"), g_who, at.X, at.Y, at.Z);
        g_census_due = true;
        g_census_line.clear();
    }

    void on_map_loaded()
    {
        g_seen.clear(); // старая карта выгружена — указатели больше не наши
        g_mine.clear();
        g_peer.clear();
        g_diff_since.clear();
        g_notified.clear();
        g_diff_line.clear();
        g_quiet_until = now_ms() + SettleMs;
        g_census_due = true; // срез, когда карта устоится
        Output::send<LogLevel::Verbose>(STR("[Zone] {} карта загружена, пишу появления через {} с\n"), g_who, SettleMs / 1000);
    }
} // namespace ZoneTrace
