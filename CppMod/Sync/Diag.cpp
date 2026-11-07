#include "Diag.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/ItemProps.h"
#include "Game/Items.h"
#include "Game/ObjectDump.h"
#include "Sync/TrunkSync.h"

#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace Diag
{
    std::atomic<bool> g_snapshot_requested{false};

    namespace
    {
        constexpr uint64_t PollMs = 1000;
        constexpr float LevelStep = 1.0f; // топливо/заряд в единицах игры (макс 35–75): мельче — шум расхода

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        float to_float(const std::string& text)
        {
            try { return std::stof(text); }
            catch (...) { return NAN; }
        }

        // Установленные детали машины: тег слота → экземпляр
        std::map<std::string, UObject*> car_parts()
        {
            std::map<std::string, UObject*> out;
            auto* car = UObjectGlobals::FindFirstOf(STR("NewVehiclePawn"));
            if (!car) return out;
            std::vector<UObject*> comps;
            UObjectGlobals::FindAllOf(STR("SlotSceneComponent"), comps);
            for (auto* comp : comps)
            {
                if (comp->GetOuterPrivate() != car) continue;
                auto* tag = property<FName>(comp, STR("SlotName"));
                auto** item_slot = property<UObject*>(comp, STR("ItemSlot"));
                if (!tag || !item_slot || !*item_slot) continue;
                auto items = Items::items_of(*item_slot);
                if (items.Num > 0 && items.Data[0]) out[Items::narrow(tag->ToString())] = items.Data[0];
            }
            return out;
        }

        // «слот=топливо/заряд;…» только у деталей, где есть хотя бы одно значение
        std::map<std::string, std::pair<float, float>> read_levels()
        {
            std::map<std::string, std::pair<float, float>> out;
            for (auto& [tag, instance] : car_parts())
            {
                float fuel = to_float(ItemProps::read(instance, ItemProps::Specs[1]));
                float battery = to_float(ItemProps::read(instance, ItemProps::Specs[2]));
                if (!std::isnan(fuel) || !std::isnan(battery)) out[tag] = {fuel, battery};
            }
            return out;
        }

        std::string levels_line(const std::map<std::string, std::pair<float, float>>& levels)
        {
            std::string out;
            for (auto& [tag, v] : levels)
                out += std::format(" {}=[fuel {:.1f}, charge {:.1f}]", tag.substr(tag.rfind('.') + 1), std::isnan(v.first) ? -1.f : v.first, // ASCII: Items::wide расширяет побайтно
                                   std::isnan(v.second) ? -1.f : v.second);
            return out;
        }

        bool levels_changed(const std::map<std::string, std::pair<float, float>>& a, const std::map<std::string, std::pair<float, float>>& b)
        {
            if (a.size() != b.size()) return true;
            auto differ = [](float x, float y) { return std::isnan(x) != std::isnan(y) || (!std::isnan(x) && std::fabs(x - y) >= LevelStep); };
            for (auto& [tag, v] : a)
            {
                auto it = b.find(tag);
                if (it == b.end() || differ(v.first, it->second.first) || differ(v.second, it->second.second)) return true;
            }
            return false;
        }

        // Причуды машины: BP_StatusEffectManager_C.CarQuirks — число и класс/владелец каждой
        std::string quirks_line()
        {
            auto* manager = UObjectGlobals::FindFirstOf(STR("BP_StatusEffectManager_C"));
            auto* quirks = manager ? property<Items::RawArray>(manager, STR("CarQuirks")) : nullptr;
            if (!quirks) return "no effect manager";
            std::string out = std::format("CarQuirks: {}", quirks->Data ? quirks->Num : 0);
            for (int32_t i = 0; quirks->Data && i < quirks->Num && i < 8; ++i)
                if (auto* q = quirks->Data[i])
                    out += std::format(" [{} owner={}]", Items::narrow(q->GetClassPrivate()->GetName()),
                                       q->GetOuterPrivate() ? Items::narrow(q->GetOuterPrivate()->GetName()) : std::string("?"));
            return out;
        }

        // Эффекты с «Quirk» в названии на установленных деталях (если причуда лежит и в ActiveEffects — это видно здесь)
        std::string part_quirks_line()
        {
            std::string out;
            for (auto& [tag, instance] : car_parts())
                for (auto& effect : Items::effects_of(instance))
                    if (effect.find("Quirk") != std::string::npos) out += std::format(" {}:{}", tag.substr(tag.rfind('.') + 1), effect.substr(effect.rfind('/') + 1));
            return out.empty() ? "no quirks on parts" : "on parts:" + out;
        }

        // Якоря: предметы на земле/в руке и «вилки» (основания BP_Spawn_Anchor_*, 2026-11-06: якорь лежит в их ItemInstance,
        // флаг AnchorRemoved). Пункт 10 теста: у кого из двоих основание пустеет, когда якорь берёт другой
        std::string anchors_line()
        {
            std::vector<UObject*> actors;
            UObjectGlobals::FindAllOf(STR("ItemWorldManifestation"), actors);
            int ground = 0;
            for (auto* actor : actors)
            {
                auto** instance = property<UObject*>(actor, STR("Instance"));
                if (instance && *instance && Items::archetype_path(*instance).find("Anchor") != std::string::npos) ++ground;
            }
            std::vector<UObject*> bases;
            UObjectGlobals::FindAllOf(STR("BP_Spawn_Anchor_Base_01_C"), bases); // базовый класс всех оснований (CXXHeaderDump: там AnchorRemoved)
            int full = 0, removed = 0;
            for (auto* base : bases)
            {
                auto* flag = property<bool>(base, STR("AnchorRemoved"));
                auto** instance = property<UObject*>(base, STR("ItemInstance"));
                if (flag && *flag) ++removed;
                else if (instance && *instance) ++full;
            }
            return std::format("anchors on ground/in hand: {}; anchor bases: {} (with anchor {}, AnchorRemoved {})", ground, bases.size(), full, removed);
        }

        // Ctrl+F6: каждая причуда целиком — чем отличаются 4 одинаковых SE_Quirk_MixAndMatch_C (какую снимать при синхронизации починки)
        void dump_quirks()
        {
            auto* manager = UObjectGlobals::FindFirstOf(STR("BP_StatusEffectManager_C"));
            auto* quirks = manager ? property<Items::RawArray>(manager, STR("CarQuirks")) : nullptr;
            for (int32_t i = 0; quirks && quirks->Data && i < quirks->Num && i < 8; ++i)
                if (auto* q = quirks->Data[i]) Output::send<LogLevel::Verbose>(STR("[Diag] quirk #{}: {}\n"), i, Items::wide(ObjectDump::to_json(q).substr(0, 6000)));
        }

        // Содержимое багажников машин-доноров: контейнер = ItemContainer с Outer = ItemInstance актёра (RECON.md 6.27)
        void donor_trunks()
        {
            std::vector<UObject*> parts, containers;
            UObjectGlobals::FindAllOf(STR("BP_TemplatePart_TrunkContents_C"), parts);
            UObjectGlobals::FindAllOf(STR("ItemContainer"), containers);
            int index = 0;
            for (auto* part : parts)
            {
                auto** instance = property<UObject*>(part, STR("ItemInstance"));
                if (!instance || !*instance) continue;
                for (auto* container : containers)
                {
                    if (container->GetOuterPrivate() != *instance) continue;
                    int total = 0;
                    if (auto* slots = property<Items::RawArray>(container, STR("Slots")); slots && slots->Data)
                        for (int32_t i = 0; i < slots->Num; ++i) total += Items::items_of(slots->Data[i]).Num;
                    // имя актёра: по нему видно один и тот же багажник у Host и Client (порядок FindAllOf у них разный)
                    Output::send<LogLevel::Verbose>(STR("[Diag] донор #{} {}: {} предметов\n"), ++index, part->GetName(), total);
                }
            }
            Output::send<LogLevel::Verbose>(STR("[Diag] багажников доноров на карте: {}\n"), parts.size());
        }

        void snapshot()
        {
            Output::send<LogLevel::Verbose>(STR("[Diag] ===== срез Ctrl+F6 =====\n"));
            Output::send<LogLevel::Verbose>(STR("[Diag] уровни:{}\n"), Items::wide(levels_line(read_levels())));
            Output::send<LogLevel::Verbose>(STR("[Diag] {}; {}\n"), Items::wide(quirks_line()), Items::wide(part_quirks_line()));
            Output::send<LogLevel::Verbose>(STR("[Diag] {}\n"), Items::wide(anchors_line()));
            TrunkSync::describe();
            donor_trunks();
            dump_quirks();
            ObjectDump::dump_target(); // объект под прицелом (JSON-файл; лаунчер заберёт)
            Output::send<LogLevel::Verbose>(STR("[Diag] ===== конец среза =====\n"));
        }

        std::map<std::string, std::pair<float, float>> g_levels;
        std::string g_quirks, g_part_quirks, g_anchors;
        uint64_t g_last_ms = 0;
        bool g_first = true;
    } // namespace

    void tick(bool is_host)
    {
        if (g_snapshot_requested.exchange(false)) snapshot();
        auto now = now_ms();
        if (now - g_last_ms < PollMs) return;
        g_last_ms = now;
        const wchar_t* who = is_host ? L"Host" : L"Client";

        auto levels = read_levels();
        if (g_first || levels_changed(g_levels, levels))
        {
            Output::send<LogLevel::Verbose>(STR("[Diag] {} уровни деталей:{}\n"), who, Items::wide(levels_line(levels)));
            g_levels = levels;
        }
        auto quirks = quirks_line();
        auto part_quirks = part_quirks_line();
        if (g_first || quirks != g_quirks || part_quirks != g_part_quirks)
        {
            Output::send<LogLevel::Verbose>(STR("[Diag] {} {}; {}\n"), who, Items::wide(quirks), Items::wide(part_quirks));
            g_quirks = quirks;
            g_part_quirks = part_quirks;
        }
        auto anchors = anchors_line();
        if (g_first || anchors != g_anchors)
        {
            Output::send<LogLevel::Verbose>(STR("[Diag] {} {}\n"), who, Items::wide(anchors));
            g_anchors = anchors;
        }
        g_first = false;
    }

    namespace
    {
        double g_perf_sum[PerfCount]{}, g_perf_max[PerfCount]{};
        uint64_t g_perf_frames = 0, g_perf_last_ms = 0;
    } // namespace

    void perf_add(int index, double ms)
    {
        g_perf_sum[index] += ms;
        if (ms > g_perf_max[index]) g_perf_max[index] = ms;
    }

    void perf_report()
    {
        ++g_perf_frames;
        auto now = now_ms();
        if (g_perf_last_ms == 0) g_perf_last_ms = now;
        if (now - g_perf_last_ms < 10'000) return;
        static const char* names[PerfCount] = {"Net", "Presence", "Car", "Passenger", "Loot", "Door", "Systems", "Garage", "Parts", "Trunk", "Diag", "PassSave", "Status", "Quirk", "Anchor", "Zone"};
        std::string line;
        double total = 0;
        for (int i = 0; i < PerfCount; ++i)
        {
            total += g_perf_sum[i];
            line += std::format(" {}={:.3f}/{:.1f}", names[i], g_perf_sum[i] / g_perf_frames, g_perf_max[i]);
            g_perf_sum[i] = g_perf_max[i] = 0;
        }
        Output::send<LogLevel::Verbose>(STR("[Perf] кадров {}, всего {:.3f} мс/кадр; среднее/макс мс:{}\n"), g_perf_frames, total / g_perf_frames, Items::wide(line));
        g_perf_frames = 0;
        g_perf_last_ms = now;
    }
} // namespace Diag
