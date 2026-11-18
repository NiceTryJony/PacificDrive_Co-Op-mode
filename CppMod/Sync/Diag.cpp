#include "Diag.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/ItemProps.h"
#include "Game/Items.h"
#include "Game/ObjectDump.h"
#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Sync/PassengerSync.h"
#include "Sync/TrunkSync.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
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

        // FindFirstOf обходит все объекты игры, а Diag спрашивает каждую секунду ([Perf] Diag пики 2,7 мс)
        Cached::First g_effect_manager{STR("BP_StatusEffectManager_C")};

        // Установленные детали машины: тег слота → экземпляр
        std::map<std::string, UObject*> car_parts()
        {
            std::map<std::string, UObject*> out;
            auto* car = Cached::car();
            if (!car) return out;
            // Список как у PartsSync: FindAllOf каждую секунду обходил все объекты игры ([Perf] Diag пики 2,7 мс)
            for (auto* comp : Cached::car_slot_components(car))
            {
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
            auto* manager = g_effect_manager.get();
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
            // Списки перечитываются не чаще раза в 5 с: строка диагностическая, а FindAllOf каждую секунду давал пики [Perf] Diag
            static Cached::All manifestations{STR("ItemWorldManifestation")}, anchor_bases{STR("BP_Spawn_Anchor_Base_01_C")};
            int ground = 0;
            for (auto* actor : manifestations.get(5000))
            {
                auto** instance = property<UObject*>(actor, STR("Instance"));
                if (instance && *instance && Items::archetype_path(*instance).find("Anchor") != std::string::npos) ++ground;
            }
            auto bases = anchor_bases.get(5000); // базовый класс всех оснований (CXXHeaderDump: там AnchorRemoved)
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

        // Шторм нестабильности (BP_StormManager_C, разбор FModel 2026-11-09): допуск со случайным разбросом у каждого ПК свой?
        // Возвращает {строка без времени — для сравнения, накопленное время}
        std::pair<std::string, float> storm_state()
        {
            static Cached::First storm_manager{STR("BP_StormManager_C")};
            auto* mgr = storm_manager.get();
            if (!mgr) return {"no storm manager", NAN};
            auto* tol = property<float>(mgr, STR("FinalInstabilityTollerance"));
            auto* before = property<float>(mgr, STR("TimeBeforeInstabilityStarts"));
            auto* state = property<uint8_t>(mgr, STR("CurrentStormState"));
            auto** storm = property<UObject*>(mgr, STR("SpawnedStormIfValid"));
            float time = NAN;
            UFunctionCall get(mgr, STR("GetCurrentInstabilityTime"));
            if (auto* out = get ? get.param<float>(STR("CurrentInstabilityTime")) : nullptr)
            {
                get.call();
                time = *out;
            }
            return {std::format("tolerance {:.1f}, before-start {:.1f}, state {}, storm {}", tol ? *tol : -1.f, before ? *before : -1.f, state ? *state : -1,
                                storm && *storm ? Items::narrow((*storm)->GetName()) : std::string("none")),
                    time};
        }

        // Ctrl+F6: каждая причуда целиком — чем отличаются 4 одинаковых SE_Quirk_MixAndMatch_C (какую снимать при синхронизации починки)
        void dump_quirks()
        {
            auto* manager = g_effect_manager.get();
            auto* quirks = manager ? property<Items::RawArray>(manager, STR("CarQuirks")) : nullptr;
            for (int32_t i = 0; quirks && quirks->Data && i < quirks->Num && i < 8; ++i)
                if (auto* q = quirks->Data[i]) Output::send<LogLevel::Verbose>(STR("[Diag] quirk #{}: {}\n"), i, Items::wide(ObjectDump::to_json(q).substr(0, 6000)));
        }

        // Содержимое багажников машин-доноров: контейнер = ItemContainer с Outer = ItemInstance актёра (RECON.md 6.27)
        void donor_trunks()
        {
            std::vector<UObject*> parts, containers;
            UObjectGlobals::FindAllOf(STR("BP_TemplatePart_TrunkContents_C"), parts);
            UObjectGlobals::FindAllOf(STR("BP_TemplatePart_TruckBedContainer_C"), parts); // кузова грузовиков: разыгрывают предметы при открытии
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
                    StringType names; // состав по архетипам — сравнить с собеседником, совпадает ли розыгрыш
                    if (auto* slots = property<Items::RawArray>(container, STR("Slots")); slots && slots->Data)
                        for (int32_t i = 0; i < slots->Num; ++i)
                        {
                            auto items = Items::items_of(slots->Data[i]);
                            total += items.Num;
                            for (int32_t k = 0; items.Data && k < items.Num; ++k)
                                if (auto** arch = items.Data[k] ? property<UObject*>(items.Data[k], STR("Archetype")) : nullptr; arch && *arch)
                                    names += STR(" ") + (*arch)->GetName();
                        }
                    auto* rolled = property<bool>(part, STR("RolledOnOpenItems")); // есть только у кузова грузовика
                    // имя актёра: по нему видно один и тот же багажник у Host и Client (порядок FindAllOf у них разный)
                    // машина-владелец (родитель актёра багажника) — её класс и позиция совпадают у Host и Client, это ключ багажника
                    struct Vec
                    {
                        float X, Y, Z;
                    };
                    UFunctionCall get_parent(part, STR("GetAttachParentActor"));
                    auto* parent = get_parent ? get_parent.param<UObject*>(STR("ReturnValue")) : nullptr;
                    if (parent) get_parent.call();
                    UObject* car = parent ? *parent : nullptr;
                    UFunctionCall loc(car, STR("K2_GetActorLocation"));
                    auto* pos = loc ? loc.param<Vec>(STR("ReturnValue")) : nullptr;
                    if (pos) loc.call();
                    Output::send<LogLevel::Verbose>(STR("[Diag] донор #{} {}{}: {} предметов, машина {} @ {} {} {}:{}\n"), ++index, part->GetName(),
                                                    rolled ? (*rolled ? STR(" [кузов, разыгран]") : STR(" [кузов, не разыгран]")) : STR(""), total,
                                                    car ? car->GetClassPrivate()->GetName() : StringType(STR("?")), pos ? std::lround(pos->X) : 0, pos ? std::lround(pos->Y) : 0,
                                                    pos ? std::lround(pos->Z) : 0, names);
                }
            }
            Output::send<LogLevel::Verbose>(STR("[Diag] багажников доноров на карте: {}\n"), parts.size());
        }

        // Ctrl+F6: гоблер целиком (свойства, заряд IP_Gobbler, слоты) и поиск «вилки» — любых объектов с Fork/Prong/Capsule в классе или имени
        // (CXXHeaderDump отдельного класса «вилка» не показал: возможно, это SM_Anchor_Capsule_01 на самом гоблере)
        void dump_gobbler()
        {
            std::vector<UObject*> gobblers;
            UObjectGlobals::FindAllOf(STR("BP_Carpart_Internal_Gobbler_C"), gobblers);
            Output::send<LogLevel::Verbose>(STR("[Diag] гоблеров на карте: {}\n"), gobblers.size());
            for (auto* gobbler : gobblers)
                Output::send<LogLevel::Verbose>(STR("[Diag] гоблер {}: {}\n"), gobbler->GetName(), Items::wide(ObjectDump::to_json(gobbler).substr(0, 8000)));
            int found = 0;
            UObjectGlobals::ForEachUObject([&](UObject* object, int32_t, int32_t) {
                if (found >= 60) return LoopAction::Break;
                auto name = Items::narrow(object->GetName());
                auto cls = Items::narrow(object->GetClassPrivate()->GetName());
                if (name.rfind("Default__", 0) == 0 || cls == "Class" || cls == "BlueprintGeneratedClass") return LoopAction::Continue;
                for (const char* key : {"Fork", "Prong", "AnchorCapsule", "Anchor_Capsule"})
                    if (name.find(key) != std::string::npos || cls.find(key) != std::string::npos)
                    {
                        Output::send<LogLevel::Verbose>(STR("[Diag] вилка? {} класс {} outer {}\n"), object->GetName(), object->GetClassPrivate()->GetName(),
                                                        object->GetOuterPrivate() ? object->GetOuterPrivate()->GetName() : STR("?"));
                        ++found;
                        break;
                    }
                return LoopAction::Continue;
            });
            Output::send<LogLevel::Verbose>(STR("[Diag] кандидатов на «вилку»: {} (предел 60)\n"), found);
        }

        // Ctrl+F6: маршрут на карте — UBaseRouteState и отрезки UBaseLegState (PenDriverPro.hpp:2309). Сравнить у Host и Client после
        // загрузки одного сейва: фаза, семя формы, текущий отрезок (events.md, «Второй проход» 2026-11-10)
        void dump_route()
        {
            std::vector<UObject*> routes, legs;
            UObjectGlobals::FindAllOf(STR("BaseRouteState"), routes);
            UObjectGlobals::FindAllOf(STR("BaseLegState"), legs);
            Output::send<LogLevel::Verbose>(STR("[Diag] маршрутов: {}, отрезков: {}\n"), routes.size(), legs.size());
            for (auto* route : routes)
            {
                auto* phase = property<uint8_t>(route, STR("CurrentPhase"));
                auto* seed = property<int32_t>(route, STR("WorldShapeRandSeed"));
                auto** leg = property<UObject*>(route, STR("CurrentLeg"));
                auto** shape = property<UObject*>(route, STR("WorldShape"));
                Output::send<LogLevel::Verbose>(STR("[Diag] маршрут {} класс {}: фаза {}, семя формы {}, текущий отрезок {}, форма {}\n"), route->GetName(),
                                                route->GetClassPrivate()->GetName(), phase ? static_cast<int>(*phase) : -1, seed ? *seed : -1,
                                                leg && *leg ? (*leg)->GetName() : STR("нет"), shape && *shape ? (*shape)->GetName() : STR("нет"));
                Output::send<LogLevel::Verbose>(STR("[Diag] маршрут JSON: {}\n"), Items::wide(ObjectDump::to_json(route).substr(0, 8000)));
            }
            for (size_t i = 0; i < legs.size() && i < 40; ++i)
                Output::send<LogLevel::Verbose>(STR("[Diag] отрезок {}: {}\n"), legs[i]->GetName(), Items::wide(ObjectDump::to_json(legs[i]).substr(0, 1500)));
        }

        // Ctrl+F6: хранилища гаража через BP_InventoryManager_C (GetGarageStationContainers / GetPartLockerInventories) — отдаёт ли игра
        // весь список одним вызовом и совпадает ли он у Host и Client (число, владелец, сколько предметов)
        void dump_garage_containers()
        {
            static Cached::First manager{STR("BP_InventoryManager_C")};
            auto* mgr = manager.get();
            for (const TCHAR* name : {STR("GetGarageStationContainers"), STR("GetPartLockerInventories")})
            {
                std::vector<UObject*> containers;
                if (!Items::call_array(mgr, name, containers))
                {
                    Output::send<LogLevel::Verbose>(STR("[Diag] {}: НЕ вызвана (нет менеджера или функции, либо первый параметр — не массив объектов)\n"), name);
                    continue;
                }
                Output::send<LogLevel::Verbose>(STR("[Diag] {}: контейнеров {}\n"), name, containers.size());
                for (auto* container : containers)
                {
                    int total = 0, slot_count = 0;
                    auto* slots_prop = container->GetPropertyByNameInChain(STR("Slots"));
                    auto* slots = slots_prop ? property<Items::RawArray>(container, STR("Slots")) : nullptr;
                    if (slots && CastField<FArrayProperty>(slots_prop)) // UItemContainer: TArray<UItemSlot*>
                    {
                        slot_count = slots->Num;
                        for (int32_t i = 0; slots->Data && i < slots->Num; ++i) total += Items::items_of(slots->Data[i]).Num;
                    }
                    else if (slots) // UNamedItemContainer (шкаф деталей): Slots — TMap<FName, UItemSlot*>, как массив читать нельзя
                    {               // (вылеты 0.17.12/13). Берём только счётчики TMap: ячеек и предметов (с учётом свободных мест — примерно)
                        auto* items = property<Items::RawArray>(container, STR("ReverseSlotItemLookup"));
                        slot_count = slots->Num;
                        total = items ? items->Num : -1;
                    }
                    auto* outer = container->GetOuterPrivate();
                    Output::send<LogLevel::Verbose>(STR("[Diag]   {} класс {} владелец {}: слотов {}, предметов {}\n"), container->GetName(),
                                                    container->GetClassPrivate()->GetName(), outer ? outer->GetName() : STR("?"), slot_count, total);
                }
            }
        }

        // Контейнеры зоны (ящики, рюкзаки, шкафчики, багажники брошенных машин): класс, имя актёра, позиция, число предметов. Сверить у Host и Client
        // после ZoneLoad: совпадают ли имена (ключ контейнера между ПК, DECISIONS 2026-11-12). Только контейнеры, чей владелец — актёр уровня
        void dump_world_containers()
        {
            std::vector<UObject*> containers;
            UObjectGlobals::FindAllOf(STR("ItemContainer"), containers);
            struct Vec
            {
                float X, Y, Z;
            };
            int shown = 0;
            for (auto* container : containers)
            {
                auto* owner = container->GetOuterPrivate();
                if (!owner || owner->GetFullName().find(STR("PersistentLevel.")) == StringType::npos) continue;
                UFunctionCall loc(owner, STR("K2_GetActorLocation"));
                auto* pos = loc ? loc.param<Vec>(STR("ReturnValue")) : nullptr;
                if (pos) loc.call();
                int total = 0;
                auto* slots = property<Items::RawArray>(container, STR("Slots"));
                for (int32_t i = 0; slots && slots->Data && i < slots->Num; ++i) total += Items::items_of(slots->Data[i]).Num;
                Output::send<LogLevel::Verbose>(STR("[Diag] контейнер зоны {} {} @ {} {} {}: предметов {}\n"), owner->GetClassPrivate()->GetName(), owner->GetName(),
                                                pos ? std::lround(pos->X) : 0, pos ? std::lround(pos->Y) : 0, pos ? std::lround(pos->Z) : 0, total);
                ++shown;
            }
            Output::send<LogLevel::Verbose>(STR("[Diag] контейнеров зоны: {}\n"), shown);
        }

        // Машины и грузовики зоны (багажники брошенных машин): класс, имя, позиция — ключ «класс + позиция» сверить у Host и Client. Контейнер
        // машины, судя по Host, появляется при открытии, поэтому ищем сами актёры
        void dump_world_vehicles()
        {
            struct Vec
            {
                float X, Y, Z;
            };
            int shown = 0;
            for (const TCHAR* cls : {STR("BP_Template_StrippedCar_01_C"), STR("BP_Template_GovCar_01_C"), STR("BP_Template_CivCar_01_C"), STR("BP_Template_StrippedCar_Junkyard_C"),
                                     STR("BP_Template_SciTruck_C"), STR("BP_Template_CosmeticsTruck_C")})
            {
                std::vector<UObject*> found;
                UObjectGlobals::FindAllOf(cls, found);
                for (auto* actor : found)
                {
                    UFunctionCall loc(actor, STR("K2_GetActorLocation"));
                    auto* pos = loc ? loc.param<Vec>(STR("ReturnValue")) : nullptr;
                    if (pos) loc.call();
                    Output::send<LogLevel::Verbose>(STR("[Diag] машина зоны {} {} @ {} {} {}\n"), cls, actor->GetName(), pos ? std::lround(pos->X) : 0, pos ? std::lround(pos->Y) : 0,
                                                    pos ? std::lround(pos->Z) : 0);
                    ++shown;
                }
            }
            Output::send<LogLevel::Verbose>(STR("[Diag] машин зоны: {}\n"), shown);
        }

        // Автосейв игры (BP_AutoSaveDirector_C): флаг ставит таймер, запись — при посадке/высадке. Срабатывает ли у Client
        std::string autosave_state()
        {
            static Cached::First director{STR("BP_AutoSaveDirector_C")};
            auto* flag = property<bool>(director.get(), STR("AutosaveFlagged"));
            return flag ? (*flag ? "flagged" : "idle") : "no director";
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
            dump_gobbler();
            dump_route();
            dump_garage_containers();
            dump_world_containers();
            dump_world_vehicles();
            PassengerSync::describe();
            Output::send<LogLevel::Verbose>(STR("[Diag] autosave: {}\n"), Items::wide(autosave_state()));
            ObjectDump::dump_target(); // объект под прицелом (JSON-файл; лаунчер заберёт)
            Output::send<LogLevel::Verbose>(STR("[Diag] ===== конец среза =====\n"));
        }

        // Запросы к запущенной игре без нажатий клавиш: команды в coop/query.txt (по одной в строке), мод раз в секунду выполняет их
        // на игровом потоке, пишет ответ в coop/query_out.txt и удаляет запрос. Объект — имя класса (все живые объекты) или путь
        // объекта (как в "path" дампа). Команды:
        //   dump <объект> [сколько=3]  — JSON объектов (ObjectDump); всего найдено — в заголовке
        //   class <объект>             — свойства и функции класса: тип, ret/out у параметров (в CXXHeaderDump результат не виден)
        //   call <объект> <Функция>    — вызвать функцию, у которой нет входных параметров; в ответе её выходные
        std::filesystem::path g_query_dir;

        std::vector<UObject*> query_objects(std::string what)
        {
            std::vector<UObject*> found;
            if (what.find_first_of("/.") == std::string::npos) UObjectGlobals::FindAllOf(what, found);
            else if (auto* object = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, Items::wide(what).c_str())) found.push_back(object);
            return found;
        }

        std::string type_of(FProperty* p)
        {
            std::string type = Items::narrow(p->GetClass().GetName());
            if (auto* s = CastField<FStructProperty>(p); s && s->GetStruct()) type += "<" + Items::narrow(s->GetStruct()->GetName()) + ">";
            if (auto* a = CastField<FArrayProperty>(p); a && a->GetInner()) type += "<" + type_of(a->GetInner()) + ">";
            return type;
        }

        std::string signature(UFunction* fn)
        {
            std::string line = Items::narrow(fn->GetName()) + "(";
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
                if (p->HasAnyPropertyFlags(CPF_Parm)) // у функций блюпринта в том же списке идут локальные переменные
                    line += std::format("{}{} {}; ", p->HasAnyPropertyFlags(CPF_ReturnParm) ? "ret " : p->HasAnyPropertyFlags(CPF_OutParm) ? "out " : "", type_of(p),
                                    Items::narrow(p->GetName()));
            return line + ")";
        }

        std::string run_query(const std::string& command, const std::string& what, const std::string& arg)
        {
            auto objects = query_objects(what);
            if (objects.empty()) return "объект не найден\n";
            std::string out = std::format("найдено: {}\n", objects.size());
            if (command == "dump")
            {
                size_t limit = 3;
                try { if (!arg.empty()) limit = (std::min)(std::stoul(arg), 20ul); }
                catch (const std::exception&) {}
                for (size_t i = 0; i < objects.size() && i < limit; ++i) out += ObjectDump::to_json(objects[i]) + "\n";
            }
            else if (command == "class")
            {
                auto* type = objects[0]->GetClassPrivate();
                out += "класс " + Items::narrow(type->GetPathName()) + "\n";
                for (FProperty* p : type->ForEachPropertyInChain()) out += std::format("  prop {} {}\n", type_of(p), Items::narrow(p->GetName()));
                for (UFunction* fn : type->ForEachFunctionInChain()) out += "  fn " + signature(fn) + "\n";
            }
            else if (command == "call")
            {
                auto* fn = objects[0]->GetFunctionByNameInChain(Items::wide(arg).c_str());
                if (!fn) return out + "функции нет\n";
                alignas(16) uint8_t params[512]{};
                int32_t size = 0;
                for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
                {
                    // входной параметр остался бы нулём — игра может упасть; такие функции не зовём
                    if (p->HasAnyPropertyFlags(CPF_Parm) && !p->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) return out + "есть входные параметры — не вызываю: " + signature(fn) + "\n";
                    size = (std::max)(size, p->GetOffset_ForInternal() + p->GetSize());
                }
                if (size > static_cast<int32_t>(sizeof(params))) return out + "параметры больше 512 байт — не вызываю\n";
                objects[0]->ProcessEvent(fn, params);
                out += signature(fn) + "\n" + ObjectDump::struct_to_json(fn, params) + "\n"; // массивы результата не освобождаем: диагностика
            }
            else if (command == "doors") // doors BP_CarManager_C: GetDoorState(0..5), как опрашивает DoorSync
            {
                for (uint8_t door = 0; door < 6; ++door)
                {
                    UFunctionCall fn(objects[0], STR("GetDoorState"));
                    auto* p_door = fn ? fn.param<uint8_t>(STR("Door")) : nullptr;
                    auto* p_open = fn ? fn.param<bool>(STR("Open")) : nullptr;
                    if (!p_door || !p_open) return out + "GetDoorState: нет функции или параметров\n";
                    *p_door = door;
                    fn.call();
                    out += std::format("  дверь {}: {}\n", door, *p_open ? "открыта" : "закрыта");
                }
            }
            else out += "неизвестная команда (dump | class | call | doors)\n";
            return out;
        }

        void query_tick()
        {
            auto file = g_query_dir / "query.txt";
            std::error_code ec;
            if (g_query_dir.empty() || !std::filesystem::exists(file, ec)) return;
            std::string out, line;
            {
                std::ifstream in(file);
                while (std::getline(in, line))
                {
                    std::istringstream words(line);
                    std::string command, what, arg;
                    if (!(words >> command >> what)) continue;
                    words >> arg;
                    Output::send<LogLevel::Verbose>(STR("[Diag] запрос: {}\n"), Items::wide(line));
                    out += "### " + line + "\n" + run_query(command, what, arg);
                }
            }
            std::filesystem::remove(file, ec); // не удалился — выполнится повторно; ответ тот же
            std::ofstream(g_query_dir / "query_out.txt", std::ios::binary) << out;
        }

        std::map<std::string, std::pair<float, float>> g_levels;
        std::string g_quirks, g_part_quirks, g_anchors;
        uint64_t g_last_ms = 0, g_storm_ms = 0;
        bool g_first = true;
        std::string g_storm, g_autosave = "?";
        constexpr uint64_t StormTimeLogMs = 30'000; // время нестабильности растёт каждый кадр — в лог не чаще
    } // namespace

    void init(const std::filesystem::path& coop_dir) { g_query_dir = coop_dir; }

    void tick(bool is_host)
    {
        if (g_snapshot_requested.exchange(false)) snapshot();
        auto now = now_ms();
        if (now - g_last_ms < PollMs) return;
        g_last_ms = now;
        query_tick();
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
        auto [storm, storm_time] = storm_state();
        if (g_first || storm != g_storm || now - g_storm_ms >= StormTimeLogMs)
        {
            Output::send<LogLevel::Verbose>(STR("[Diag] {} storm: {}, instability time {:.0f}\n"), who, Items::wide(storm), std::isnan(storm_time) ? -1.0 : storm_time);
            g_storm = storm;
            g_storm_ms = now;
        }
        // flagged → idle = игра записала автосейв (TriggerAutosave при посадке/высадке)
        if (auto autosave = autosave_state(); g_first || autosave != g_autosave)
        {
            Output::send<LogLevel::Verbose>(STR("[Diag] {} autosave: {} -> {}\n"), who, Items::wide(g_autosave), Items::wide(autosave));
            g_autosave = autosave;
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
        static const char* names[PerfCount] = {"Net", "Presence", "Car", "Passenger", "Loot", "Door", "Systems", "Garage", "Parts", "Trunk", "Diag", "PassSave", "Status", "Quirk", "Anchor", "Zone", "HudFeed"};
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
