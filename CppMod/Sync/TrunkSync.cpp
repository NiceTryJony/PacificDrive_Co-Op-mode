#include "TrunkSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>

#include "Game/ItemProps.h"
#include "Game/Items.h"
#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Notify.h"
#include "Networking/Net.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string_view>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Багажник = деталь BP_Storage_Trunk_Base_C в слоте машины → компонент BP_InventoryContainerComponent → Container.
// Синхронизируем «стопки»: архетип + значения свойств (прочность, топливо, заряд — ItemProps::Specs), сколько таких предметов
// в багажнике. Не ячейки: игра сдвигает ячейки, когда одна пустеет, и по номеру ячейки мод удалял не то (тест 2026-10-30 14:51).
// Host шлёт изменившиеся стопки (TrunkItem). Client добавляет недостающее через AddItem багажника (значения пишет штатным
// SetItemProperty) и удаляет лишнее ровно на разницу.
// Действия пассажира: Client помнит, сколько каждой стопки должно быть по данным Host (g_known); расхождение, пока не ждём
// команды Host по этой стопке, — это пассажир взял/положил → TrunkTake/TrunkPut (со значениями). Host убирает/создаёт у себя;
// на «взял» больше, чем у него есть, отвечает TrunkDenied, и Client убирает эти предметы у пассажира (спека 3.4.3, без дюпа).
// Перенос из багажника в рюкзак переносит тот же экземпляр, Remove уничтожает и предмет в руке (RECON.md «6.12»).
namespace TrunkSync
{
    namespace
    {
        constexpr uint64_t PollMs = 400; // было 1000: пассажир ждал секунды; ItemProps::read на каждый предмет — следить, не раздувается ли сейв (DECISIONS 2026-10-30)
        constexpr uint64_t SettleMs = 5000; // после загрузки карты машина ещё собирается
        constexpr int MaxApplyAttempts = 3;

        struct Trunk
        {
            UObject* car = nullptr;
            UObject* component = nullptr; // BP_InventoryContainerComponent: AddItem сам выбирает ячейку
            UObject* container = nullptr;
            bool named = false; // шкаф деталей: UNamedItemContainer (Slots — TMap), работаем через его функции
        };

        // Стопка: архетип + значения свойств. Ключ — строка, по которой стопки сравниваются и хранятся
        struct Stack
        {
            std::string Path;
            std::vector<Coop::TrunkValue> Values;
        };
        using Key = std::string;
        struct Entry
        {
            Stack Stack;
            std::vector<UObject*> Items; // экземпляры в порядке ячеек
        };
        using ByKey = std::map<Key, Entry>;

        bool g_send_full = false;
        int8_t g_seen[4] = {-1, -1, -1, -1}; // контейнер найден (1) / нет (0) / ещё не проверяли (-1) — в лог пишем только смену
        uint64_t g_last_poll_ms = 0, g_map_loaded_ms = 0;
        // Host
        std::map<Key, std::pair<Stack, uint16_t>> g_sent; // что Client уже знает
        // Client
        std::map<Key, std::pair<Stack, uint16_t>> g_wanted; // от Host, ещё не совпало с нашим багажником
        std::map<Key, int> g_attempts;
        std::map<Key, uint16_t> g_known;                    // сколько должно быть по данным Host (после применения его команд)
        bool g_baseline = false;                            // g_known заполнен от нашего багажника после загрузки
        ByKey g_prev;                                       // наш багажник в прошлый опрос — чтобы знать, какие именно предметы взяли
        std::map<Key, std::vector<FWeakObjectPtr>> g_taken; // взятые пассажиром (на случай отказа Host)

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // Станция регенерации деталей в гараже — второй контейнер с теми же правилами: её стопки отличаются префиксом пути (пакеты те же)
        // Гоблер машины — третий: в нём лежат скормленные якоря (это и есть заряд kLIM; BP_InventoryManager_C.Inventory_Gobbler)
        // Хранилища гаража (шкафы, ящики) — места StoragePlace + i, префикс «@S<i>:», i — номер контейнера в списке
        // InventoryManager.GetGarageStationContainers. Порядок и размеры списка у Host и Client совпадают (тест 0.17.14, 2026-11-10)
        enum Place : int
        {
            TrunkPlace,
            IncubatorPlace,
            GobblerPlace,
            RouteTunerPlace, // настройщик маршрута (BP_Garage_RouteTuner_C): карты маршрута лежат в его InventoryContainer, не в списке GetGarageStationContainers (проба 2026-11-18)
            StoragePlace
        };
        // Шкафы деталей гаража (BP_Garage_InventoryParts_C, контейнер — актёра NamedPartsContainer) — места LockerBase + j, префикс «@L<j>:»,
        // j — номер шкафа по порядку (X, Y) позиции актёра: id и порядок массива игры между сессиями меняются (тест 2026-11-12)
        constexpr int MaxStorages = 256;
        constexpr int MaxLockers = 8;
        constexpr int LockerBase = StoragePlace + MaxStorages;
        constexpr std::string_view PlacePrefix[StoragePlace] = {"", "@I:", "@G:", "@R:"};
        std::string prefix_of(int place)
        {
            if (place < StoragePlace) return std::string(PlacePrefix[place]);
            return place >= LockerBase ? std::format("@L{}:", place - LockerBase) : std::format("@S{}:", place - StoragePlace);
        }
        int place_of(const std::string& path)
        {
            if (path.starts_with("@S") || path.starts_with("@L"))
            {
                // номер из сети: не число или вне диапазона — считаем багажником, такой архетип просто не найдётся
                const bool locker = path[1] == 'L';
                const int limit = locker ? MaxLockers : MaxStorages;
                int index = 0;
                size_t i = 2;
                for (; i < path.size() && path[i] >= '0' && path[i] <= '9' && index < limit; ++i) index = index * 10 + (path[i] - '0');
                return i > 2 && i < path.size() && path[i] == ':' && index < limit ? (locker ? LockerBase : StoragePlace) + index : TrunkPlace;
            }
            for (int place = TrunkPlace + 1; place < StoragePlace; ++place)
                if (path.starts_with(PlacePrefix[place])) return place;
            return TrunkPlace;
        }
        bool in_incubator(const std::string& path) { return place_of(path) == IncubatorPlace; }
        std::string archetype_of(const std::string& path)
        {
            int place = place_of(path);
            return path.substr(place < StoragePlace ? PlacePrefix[place].size() : path.find(':') + 1);
        }

        Key key_of(const Stack& stack)
        {
            Key key = stack.Path;
            // Станция регенерации лечит детали и у Host, и у Client одновременно: здоровье в ключе дало бы «взял старую — положил
            // новую» с обеих сторон (дубли). Здоровье станции передаётся (новые предметы создаются с ним), но стопки не различает
            const auto health = static_cast<uint8_t>(&ItemProps::Health - ItemProps::Specs);
            bool incubator = in_incubator(stack.Path);
            for (auto& v : stack.Values)
                if (!(incubator && v.Spec == health)) key += std::format("|{}:{}", v.Spec, v.Bits);
            return key;
        }

        // Значения whitelist-свойств предмета для пакета: целое как есть, float битами
        std::vector<Coop::TrunkValue> values_of(UObject* instance)
        {
            std::vector<Coop::TrunkValue> out;
            for (const auto& v : ItemProps::read(instance))
            {
                try
                {
                    int32_t bits;
                    if (v.Spec->Type == 'i') bits = std::stoi(v.Text);
                    else
                    {
                        float f = std::stof(v.Text);
                        std::memcpy(&bits, &f, sizeof(bits));
                    }
                    out.push_back({static_cast<uint8_t>(v.Spec - ItemProps::Specs), bits});
                }
                catch (const std::exception&) // своё же значение не разобралось — свойство пропускаем, не роняем игру
                {
                }
            }
            return out;
        }

        // Текст значения для ItemProps::write; пусто — индекс из сети вне списка (другая версия мода)
        std::string text_of(const Coop::TrunkValue& v)
        {
            if (v.Spec >= std::size(ItemProps::Specs)) return {};
            if (ItemProps::Specs[v.Spec].Type == 'i') return std::to_string(v.Bits);
            float f;
            std::memcpy(&f, &v.Bits, sizeof(f));
            return std::format("{}", f);
        }

        // Деталь своей машины с контейнером: класс детали (exact — точное имя, иначе начало имени) и имя компонента-контейнера.
        // container == nullptr — нет машины или такая деталь не установлена
        Trunk find_car_container(const TCHAR* part_class, bool exact, const TCHAR* component_name)
        {
            Trunk trunk{Cached::car()}; // как CarSync
            if (!trunk.car) return trunk;
            for (auto* comp : Cached::car_slot_components(trunk.car))
            {
                auto** actor = property<UObject*>(comp, STR("SpawnedActor"));
                if (!actor || !*actor) continue;
                auto name = (*actor)->GetClassPrivate()->GetName();
                if (exact ? name != part_class : !name.starts_with(part_class)) continue;
                auto** component = property<UObject*>(*actor, component_name);
                auto** container = component && *component ? property<UObject*>(*component, STR("Container")) : nullptr;
                if (container && *container)
                {
                    trunk.component = *component;
                    trunk.container = *container;
                }
                break;
            }
            return trunk;
        }

        Trunk find_trunk() { return find_car_container(STR("BP_Storage_Trunk"), false, STR("BP_InventoryContainerComponent")); }

        // Точное имя: BP_Carpart_Internal_GobblerScreen_C (экран) начинается так же
        Trunk find_gobbler() { return find_car_container(STR("BP_Carpart_Internal_Gobbler_C"), true, STR("BP_InventoryContainerComponent_ExternalInventory")); }

        // Станция регенерации (BP_Garage_PartIncubator_01_C) своего гаража; container == nullptr — не в гараже. car здесь — актор станции
        // (контекст мира для создания предметов)
        // У компонента станции поле Container пустое (RECON.md «6.20»): настоящий ItemContainer принадлежит предмету-улучшению
        // IA_Garage_PartIncubator_01 (его Outer). Ищем его среди всех ItemContainer; AddItem по-прежнему зовём у компонента актора
        // Найденный контейнер станции держим слабым указателем: перебор всех ItemContainer (обход всех объектов игры) — только когда
        // контейнер пропал (смена карты, станцию убрали)
        FWeakObjectPtr g_incubator_container;
        Trunk find_incubator()
        {
            static Cached::First station{STR("BP_Garage_PartIncubator_01_C")};
            Trunk incubator{station.get()};
            if (!incubator.car) return incubator;
            auto** component = property<UObject*>(incubator.car, STR("BP_InventoryContainerComponent"));
            if (!component || !*component) return incubator;
            if (UObject* cached = g_incubator_container.Get())
            {
                incubator.component = *component;
                incubator.container = cached;
                return incubator;
            }
            std::vector<UObject*> containers;
            UObjectGlobals::FindAllOf(STR("ItemContainer"), containers);
            for (auto* candidate : containers)
            {
                auto* owner = candidate->GetOuterPrivate();
                if (owner && Items::archetype_path(owner).find("IA_Garage_PartIncubator") != std::string::npos)
                {
                    incubator.component = *component;
                    incubator.container = candidate;
                    g_incubator_container = FWeakObjectPtr(candidate);
                    break;
                }
            }
            return incubator;
        }

        // Настройщик маршрута: в мире два экземпляра (уровень Garage_Design и копия в /Engine/Transient) — берём не из Transient.
        // Контейнер — свойство актёра InventoryContainer (Outer — BP_InventoryManager); компонент не нужен: кладём в сам контейнер, как в хранилища
        FWeakObjectPtr g_route_tuner;
        uint64_t g_route_tuner_try_ms = 0;
        Trunk find_route_tuner()
        {
            UObject* actor = g_route_tuner.Get();
            if (!actor && now_ms() - g_route_tuner_try_ms > 2000)
            {
                g_route_tuner_try_ms = now_ms();
                std::vector<UObject*> all;
                UObjectGlobals::FindAllOf(STR("BP_Garage_RouteTuner_C"), all);
                for (auto* candidate : all)
                    if (candidate->GetFullName().find(STR("/Engine/Transient")) == std::wstring::npos)
                    {
                        actor = candidate;
                        g_route_tuner = FWeakObjectPtr(candidate);
                        break;
                    }
            }
            Trunk tuner{actor};
            auto** container = actor ? property<UObject*>(actor, STR("InventoryContainer")) : nullptr;
            if (container && *container) tuner.container = *container;
            return tuner;
        }

        std::vector<FWeakObjectPtr> g_storages; // хранилища гаража в порядке списка игры
        bool g_storages_frozen = false;

        // Список хранилищ фиксируем на карту, когда два опроса подряд дали одно число: станции появляются не сразу, а новая
        // станция Host посреди сессии сдвинула бы номера (её контейнер начнёт синхронизироваться после смены карты).
        // true — список только что зафиксирован
        bool refresh_storages()
        {
            if (g_storages_frozen) return false;
            static Cached::First manager{STR("BP_InventoryManager_C")};
            std::vector<UObject*> list;
            if (!Items::call_array(manager.get(), STR("GetGarageStationContainers"), list) || list.empty() || list.size() > MaxStorages) return false;
            g_storages_frozen = list.size() == g_storages.size();
            g_storages.clear();
            for (auto* container : list) g_storages.emplace_back(container);
            if (g_storages_frozen) Output::send<LogLevel::Verbose>(STR("[TrunkSync] хранилища гаража: контейнеров {}\n"), list.size());
            return g_storages_frozen;
        }

        std::vector<FWeakObjectPtr> g_lockers; // контейнеры шкафов деталей в порядке (X, Y) актёров
        bool g_lockers_frozen = false;

        // Как refresh_storages: список фиксируем, когда два опроса подряд дали одно число шкафов. true — только что зафиксирован
        // Шкафов нет (не в гараже) — следующий поиск не раньше чем через LockerRetryMs: FindAllOf обходит все объекты игры
        constexpr uint64_t LockerRetryMs = 3000;
        uint64_t g_lockers_retry_ms = 0;
        bool refresh_lockers()
        {
            if (g_lockers_frozen || now_ms() < g_lockers_retry_ms) return false;
            std::vector<UObject*> actors;
            UObjectGlobals::FindAllOf(STR("BP_Garage_InventoryParts_C"), actors);
            struct Vec
            {
                float X, Y, Z;
            };
            std::vector<std::pair<std::pair<int, int>, UObject*>> found;
            for (auto* actor : actors)
            {
                auto** container = property<UObject*>(actor, STR("NamedPartsContainer"));
                UFunctionCall loc(actor, STR("K2_GetActorLocation"));
                auto* ret = loc ? loc.param<Vec>(STR("ReturnValue")) : nullptr;
                if (!container || !*container || !ret) continue;
                loc.call();
                found.push_back({{static_cast<int>(std::lround(ret->X)), static_cast<int>(std::lround(ret->Y))}, *container});
            }
            if (found.empty() || found.size() > MaxLockers)
            {
                g_lockers_retry_ms = now_ms() + LockerRetryMs;
                return false;
            }
            std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
            g_lockers_frozen = found.size() == g_lockers.size();
            g_lockers.clear();
            for (auto& [pos, container] : found) g_lockers.emplace_back(container);
            if (g_lockers_frozen) Output::send<LogLevel::Verbose>(STR("[TrunkSync] шкафы деталей: {}\n"), found.size());
            return g_lockers_frozen;
        }

        // Все места: багажник, станция, гоблер, дальше хранилища гаража по номерам, с LockerBase — шкафы деталей
        // (container == nullptr — места сейчас нет)
        std::vector<Trunk> find_places()
        {
            std::vector<Trunk> places{find_trunk(), find_incubator(), find_gobbler(), find_route_tuner()};
            UObject* context = Cached::car(); // контекст мира для создания предметов
            if (!context) context = places[IncubatorPlace].car;
            if (g_storages_frozen)
                for (auto& weak : g_storages)
                {
                    UObject* container = weak.Get();
                    // контейнер станции регенерации игра отдаёт и в этом списке — у него уже есть своё место; номера остальных не сдвигаем
                    bool own = std::any_of(places.begin(), places.begin() + StoragePlace, [&](const Trunk& t) { return t.container == container; });
                    places.push_back(Trunk{context, nullptr, own || !context ? nullptr : container});
                }
            if (g_lockers_frozen)
            {
                places.resize(LockerBase);
                for (auto& weak : g_lockers) places.push_back(Trunk{context, nullptr, context ? weak.Get() : nullptr, true});
            }
            return places;
        }

        // Шкаф: предметы функцией игры (Slots — TMap, raw-чтение роняло игру в 0.17.12/13)
        std::vector<UObject*> named_items(UObject* container)
        {
            std::vector<UObject*> list;
            Items::call_array(container, STR("GetAllItems"), list);
            return list;
        }

        int item_count(UObject* container)
        {
            int total = 0;
            auto* slots = property<Items::RawArray>(container, STR("Slots"));
            for (int32_t i = 0; slots && slots->Data && i < slots->Num; ++i) total += Items::items_of(slots->Data[i]).Num;
            return total;
        }

        // Игрок тащит предмет мышью: он во временной ячейке курсора, из багажника «пропал». UContainerCursor.GrabbedSlot
        // ставит GrabSlot и обнуляет PlacedGrabbed (exe+0x11aadc0, RECON.md «6.12») — пока не пусто, багажник не сравниваем
        bool dragging()
        {
            std::vector<UObject*> cursors;
            UObjectGlobals::FindAllOf(STR("ContainerCursor"), cursors);
            for (auto* cursor : cursors)
                if (auto** grabbed = property<UObject*>(cursor, STR("GrabbedSlot")); grabbed && *grabbed) return true;
            return false;
        }

        // Экземпляры по стопкам (порядок — как в ячейках)
        ByKey read_items(UObject* container, int place = TrunkPlace)
        {
            ByKey by_key;
            const std::string prefix = prefix_of(place);
            if (place >= LockerBase)
            {
                for (auto* item : named_items(container))
                {
                    Stack stack{Items::archetype_path(item), values_of(item)};
                    if (stack.Path.empty()) continue;
                    stack.Path.insert(0, prefix);
                    auto& entry = by_key[key_of(stack)];
                    if (entry.Items.empty()) entry.Stack = std::move(stack);
                    entry.Items.push_back(item);
                }
                return by_key;
            }
            auto* slots = property<Items::RawArray>(container, STR("Slots"));
            if (!slots || !slots->Data) return by_key;
            for (int32_t i = 0; i < slots->Num; ++i)
            {
                auto items = Items::items_of(slots->Data[i]);
                Entry* slot_entry = nullptr;
                for (int32_t k = 0; k < items.Num; ++k)
                {
                    if (!items.Data[k]) continue;
                    // shortcut: в хранилище гаража (тысячи предметов) стопку ячейки задаёт её первый предмет — в одной ячейке лежат
                    // одинаковые; читать каждый, если найдутся ячейки с разными значениями свойств
                    if (place >= StoragePlace && slot_entry)
                    {
                        slot_entry->Items.push_back(items.Data[k]);
                        continue;
                    }
                    Stack stack{Items::archetype_path(items.Data[k]), values_of(items.Data[k])};
                    if (stack.Path.empty()) continue;
                    stack.Path.insert(0, prefix);
                    auto& entry = by_key[key_of(stack)];
                    if (entry.Items.empty()) entry.Stack = std::move(stack);
                    entry.Items.push_back(items.Data[k]);
                    slot_entry = &entry;
                }
            }
            return by_key;
        }

        uint8_t create_context(const ByKey& items)
        {
            for (auto& [key, entry] : items)
                if (!entry.Items.empty()) return Items::create_context(entry.Items.front());
            return 0;
        }

        // Предмет в багажник через его компонент; false — игра не приняла (созданный предмет удаляем, чтобы не висел)
        bool add_to_trunk(const Trunk& trunk, UObject* instance)
        {
            if (trunk.named)
            {
                // Шкаф деталей: AddItem(Slot, Item, CreateSlot) в первую пустую ячейку (в какую именно — не важно, синхронизируем стопки)
                int before = static_cast<int>(named_items(trunk.container).size());
                UFunctionCall names(trunk.container, STR("GetSlotNames"));
                auto* p_names = names ? names.param<Items::RawArray>(STR("SlotNames")) : nullptr;
                UFunctionCall add(trunk.container, STR("AddItem"));
                auto* p_slot = add ? add.param<FName>(STR("Slot")) : nullptr;
                auto* p_item = add ? add.param<UObject*>(STR("Item")) : nullptr;
                auto* p_create = add ? add.param<bool>(STR("CreateSlot")) : nullptr;
                bool placed = false;
                if (p_names && p_slot && p_item && p_create)
                {
                    names.call();
                    auto* data = reinterpret_cast<FName*>(p_names->Data);
                    for (int32_t i = 0; data && i < p_names->Num && !placed; ++i)
                    {
                        UFunctionCall get(trunk.container, STR("GetSlot"));
                        auto* g_name = get ? get.param<FName>(STR("Slot")) : nullptr;
                        auto* g_ret = get ? get.param<UObject*>(STR("ReturnValue")) : nullptr;
                        if (!g_name || !g_ret) break;
                        *g_name = data[i];
                        get.call();
                        if (!*g_ret || Items::items_of(*g_ret).Num > 0) continue; // занята
                        *p_slot = data[i];
                        *p_item = instance;
                        *p_create = true;
                        add.call();
                        placed = static_cast<int>(named_items(trunk.container).size()) > before;
                        break;
                    }
                }
                if (!placed) Items::remove(instance);
                return placed;
            }
            if (!trunk.component)
            {
                // Хранилище гаража: компонента нет, кладём в сам UItemContainer (AddItem(Item, CreateSlotIfFull); шкаф растёт сам:
                // 145 → 148 ячеек в тесте 0.17.14). Тип результата в дампе не виден — успех проверяем по числу предметов
                UFunctionCall add(trunk.container, STR("AddItem"));
                auto* p_item = add ? add.param<UObject*>(STR("Item")) : nullptr;
                auto* p_create = add ? add.param<bool>(STR("CreateSlotIfFull")) : nullptr;
                int before = item_count(trunk.container);
                if (p_item && p_create)
                {
                    *p_item = instance;
                    *p_create = true;
                    add.call();
                }
                bool added = item_count(trunk.container) > before;
                if (!added) Items::remove(instance);
                return added;
            }
            UFunctionCall add(trunk.component, STR("AddItem"));
            auto* p_item = add ? add.param<UObject*>(STR("Item")) : nullptr;
            auto* p_added = add ? add.param<bool>(STR("Added")) : nullptr;
            if (!p_item || !p_added) return false;
            *p_item = instance;
            add.call();
            if (!*p_added) Items::remove(instance);
            return *p_added;
        }

        // Результат добавления: сколько предметов легло и под каким ключом они реально оказались (после записи значений
        // читаем обратно — если игра округлила или не приняла значение, ключ отличается от нужного)
        struct Added
        {
            size_t Count = 0;
            std::vector<Key> Actual;
        };

        // Добавить n новых предметов стопки (значения — штатным SetItemProperty после того, как предмет лёг в багажник:
        // так записывали в тесте F3/F2)
        Added add_new(const Trunk& trunk, const Stack& stack, size_t n, uint8_t context)
        {
            Added result;
            for (size_t i = 0; i < n; ++i)
            {
                auto* instance = Items::create(trunk.car, archetype_of(stack.Path), context);
                if (!instance || !add_to_trunk(trunk, instance))
                {
                    Output::send<LogLevel::Warning>(STR("[TrunkSync] {}: не положить в багажник ({} из {}) — {}\n"), Items::wide(stack.Path), i, n,
                                                    instance ? STR("игра не приняла") : STR("не создать предмет"));
                    return result;
                }
                for (auto& v : stack.Values)
                {
                    auto text = text_of(v);
                    if (text.empty() || !ItemProps::write(instance, ItemProps::Specs[v.Spec], text))
                        Output::send<LogLevel::Warning>(STR("[TrunkSync] {}: значение свойства {} не записалось — предмет с исходным\n"), Items::wide(stack.Path), v.Spec);
                }
                ++result.Count;
                result.Actual.push_back(key_of({stack.Path, values_of(instance)}));
            }
            return result;
        }

        // Убрать n предметов с конца списка (последние положенные); вернуть, сколько убрано
        size_t remove_last(std::vector<UObject*>& have, size_t n)
        {
            n = (std::min)(n, have.size());
            for (size_t i = 0; i < n; ++i)
            {
                Items::remove(have.back());
                have.pop_back();
            }
            return n;
        }

        std::wstring describe(const Stack& stack)
        {
            auto name = Items::wide(stack.Path.substr(stack.Path.rfind('.') + 1));
            // в логе видно, где лежит
            if (int place = place_of(stack.Path); place >= LockerBase) name.insert(0, std::format(STR("[шкаф {}] "), place - LockerBase));
            else if (place >= StoragePlace) name.insert(0, std::format(STR("[хранилище {}] "), place - StoragePlace));
            else if (place != TrunkPlace) name.insert(0, place == IncubatorPlace ? STR("[станция] ") : place == RouteTunerPlace ? STR("[настройщик маршрута] ") : STR("[гоблер] "));
            for (auto& v : stack.Values) name += Items::wide(" " + text_of(v));
            return name;
        }

        // Client: привести число предметов стопки к присланному Host-ом — не больше разницы. true — сделано целиком (повторять
        // нечего, даже если значения легли не такие: повтор только добавил бы лишнее); false — часть не легла, повторить
        bool apply(const Trunk& trunk, const Key& key, const Stack& stack, std::vector<UObject*>& have, uint16_t want, uint8_t context)
        {
            size_t now = have.size();
            if (now > want)
            {
                remove_last(have, now - want);
                Output::send<LogLevel::Verbose>(STR("[TrunkSync] {}: убрано {}, осталось {}\n"), describe(stack), now - want, want);
                return true;
            }
            auto added = add_new(trunk, stack, want - now, context);
            // Предметы, получившие другой ключ, засчитываем под их настоящим ключом: иначе следующий опрос принял бы расхождение
            // за действие пассажира и отправил бы Host-у лишнее «взял»/«положил»
            for (auto& actual : added.Actual)
            {
                if (actual == key) continue;
                Output::send<LogLevel::Warning>(STR("[TrunkSync] {}: значения легли не такие, как у Host — предмет засчитан отдельно\n"), describe(stack));
                if (g_known[key]) --g_known[key];
                ++g_known[actual];
            }
            if (added.Count) Output::send<LogLevel::Verbose>(STR("[TrunkSync] {}: добавлено {}, стало {}\n"), describe(stack), added.Count, now + added.Count);
            return added.Count == want - now;
        }

        // Host: пассажир взял/положил
        void on_request(Coop::PacketId id, uint16_t count, const Stack& stack)
        {
            int place = place_of(stack.Path);
            auto places = find_places();
            auto trunk = place < static_cast<int>(places.size()) ? places[place] : Trunk{};
            if (!trunk.container)
            {
                // нет багажника (станции) — «взял» отклоняем целиком (у пассажира не должно появиться ничего), «положил» теряется
                if (id == Coop::TrunkTake) Net::send_trunk(Coop::TrunkDenied, count, stack.Path, stack.Values);
                Output::send<LogLevel::Warning>(STR("[TrunkSync] пассажир: {} {} × {} — багажника нет\n"), id == Coop::TrunkTake ? STR("взял") : STR("положил"), describe(stack), count);
                return;
            }
            auto items = read_items(trunk.container, place);
            if (id == Coop::TrunkTake)
            {
                size_t taken = remove_last(items[key_of(stack)].Items, count);
                if (taken < count) Net::send_trunk(Coop::TrunkDenied, static_cast<uint16_t>(count - taken), stack.Path, stack.Values);
                Output::send<LogLevel::Verbose>(STR("[TrunkSync] пассажир взял {} × {}: убрал у себя {}, отказ {}\n"), describe(stack), count, taken, count - taken);
            }
            else
            {
                auto added = add_new(trunk, stack, count, create_context(items));
                Output::send<LogLevel::Verbose>(STR("[TrunkSync] пассажир положил {} × {}: добавил у себя {}\n"), describe(stack), count, added.Count);
                if (added.Count < count)
                {
                    // Host не принял (полон/не подходит) — сообщаем Client-у его настоящее число, иначе предмет остался бы только у пассажира
                    // shortcut: лишний предмет у Client убирается (не возвращается в рюкзак); возврат — когда понадобится
                    size_t have = items.contains(key_of(stack)) ? items[key_of(stack)].Items.size() : 0;
                    Net::send_trunk(Coop::TrunkItem, static_cast<uint16_t>(have), stack.Path, stack.Values);
                    Output::send<LogLevel::Warning>(STR("[TrunkSync] {}: у Host не легло {} из {} — у пассажира лишнее убрано\n"), describe(stack), count - added.Count, count);
                }
            }
            // Новое число Client узнает обычным TrunkItem из следующего опроса
        }

        // Client: Host отказал — эти предметы у Host-а уже забрали, убрать их у пассажира
        void on_denied(uint16_t count, const Stack& stack)
        {
            auto& taken = g_taken[key_of(stack)];
            size_t removed = 0;
            while (removed < count && !taken.empty())
            {
                if (UObject* instance = taken.back().Get())
                {
                    Items::remove(instance);
                    ++removed;
                }
                taken.pop_back();
            }
            Notify::push(Notify::Kind::Warn, place_of(stack.Path) >= StoragePlace ? "The driver already took that from storage" : "The driver already took that from the trunk");
            Output::send<LogLevel::Warning>(STR("[TrunkSync] Host уже забрал {} × {} — убрано у пассажира {}\n"), describe(stack), count, removed);
        }

        // Client: есть ли расхождение, которое detect_passenger принял бы за действие пассажира (без побочных эффектов)
        bool differs_from_known(const ByKey& items)
        {
            for (auto& [key, n] : g_known)
                if (!g_wanted.contains(key) && (items.contains(key) ? items.at(key).Items.size() : 0) != n) return true;
            for (auto& [key, entry] : items)
                if (!g_wanted.contains(key) && !g_known.contains(key) && !entry.Items.empty()) return true;
            return false;
        }

        // Client: расхождения нашего багажника с данными Host, которых мы не вызывали, — действия пассажира
        void detect_passenger(ByKey& items)
        {
            std::set<Key> keys;
            for (auto& [key, n] : g_known) keys.insert(key);
            for (auto& [key, entry] : items) keys.insert(key);
            for (auto& key : keys)
            {
                if (g_wanted.contains(key)) continue; // ждём применения команды Host — это не пассажир
                size_t local = items.contains(key) ? items[key].Items.size() : 0;
                uint16_t known = g_known.contains(key) ? g_known[key] : 0;
                if (local == known) continue;
                // Описание стопки — у локального списка или у прошлого опроса (если стопка исчезла целиком)
                const Stack* stack = items.contains(key) ? &items[key].Stack : g_prev.contains(key) ? &g_prev[key].Stack : nullptr;
                if (!stack)
                {
                    g_known[key] = static_cast<uint16_t>(local); // описания нет — сообщить Host-у нечего, принимаем как есть
                    continue;
                }
                if (local < known)
                {
                    // какие именно взяли: были в прошлый опрос, сейчас нет (на случай отказа Host)
                    auto& now_list = items[key].Items;
                    for (auto* instance : g_prev[key].Items)
                        if (std::find(now_list.begin(), now_list.end(), instance) == now_list.end()) g_taken[key].emplace_back(instance);
                    Net::send_trunk(Coop::TrunkTake, static_cast<uint16_t>(known - local), stack->Path, stack->Values);
                    Output::send<LogLevel::Verbose>(STR("[TrunkSync] пассажир взял {} × {} — сообщаю Host\n"), describe(*stack), known - local);
                }
                else
                {
                    Net::send_trunk(Coop::TrunkPut, static_cast<uint16_t>(local - known), stack->Path, stack->Values);
                    Output::send<LogLevel::Verbose>(STR("[TrunkSync] пассажир положил {} × {} — сообщаю Host\n"), describe(*stack), local - known);
                }
                g_known[key] = static_cast<uint16_t>(local);
            }
        }
    } // namespace

    void on_packet(Coop::PacketId id, uint16_t count, const std::string& path, const std::vector<Coop::TrunkValue>& values, bool is_host)
    {
        Stack stack{path, values};
        if (is_host)
        {
            if (id == Coop::TrunkTake || id == Coop::TrunkPut) on_request(id, count, stack);
            return;
        }
        if (id == Coop::TrunkDenied) on_denied(count, stack);
        if (id != Coop::TrunkItem) return;
        auto key = key_of(stack);
        g_wanted[key] = {stack, count};
        g_known[key] = count; // после применения так и должно быть
        g_attempts.erase(key);
    }

    void on_connected(bool is_host)
    {
        if (is_host) g_send_full = true;
    }

    void on_map_loaded()
    {
        g_map_loaded_ms = now_ms();
        g_sent.clear();
        g_send_full = true;
        std::fill(std::begin(g_seen), std::end(g_seen), int8_t{-1});
        g_known.clear(); // багажник новый — опорное состояние снимем заново
        g_baseline = false;
        g_prev.clear();
        g_taken.clear();
        g_storages.clear();
        g_storages_frozen = false;
        g_route_tuner = FWeakObjectPtr();
        g_route_tuner_try_ms = 0;
        g_lockers.clear();
        g_lockers_frozen = false;
        g_lockers_retry_ms = 0;
    }

    void tick(bool is_host)
    {
        if (!Net::connected()) return;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs || now - g_map_loaded_ms < SettleMs) return;
        g_last_poll_ms = now;
        // Перетаскивание (dragging — обход всех объектов игры) проверяем, только когда есть что отправить или применить: пока предмет
        // в курсоре, и Host не шлёт «−1», и Client не принимает перетаскивание за «взял»
        bool lockers_fresh = refresh_lockers(); // оба обновляем всегда (не через ||)
        if (refresh_storages() || lockers_fresh)
        {
            // Хранилища появились позже остальных мест: Host шлёт всё заново, Client добирает опору (иначе их содержимое
            // он принял бы за «пассажир положил» и задвоил у Host)
            g_send_full = true;
            g_baseline = false;
        }
        auto places = find_places();
        for (int place = TrunkPlace; place < StoragePlace; ++place)
        {
            int8_t found = places[place].container != nullptr;
            if (found == g_seen[place]) continue;
            g_seen[place] = found;
            static const TCHAR* names[StoragePlace] = {STR("багажник"), STR("станция регенерации"), STR("гоблер"), STR("настройщик маршрута")};
            auto* slots = found ? property<Items::RawArray>(places[place].container, STR("Slots")) : nullptr;
            Output::send<LogLevel::Verbose>(STR("[TrunkSync] {}: {}{}\n"), names[place], found ? STR("контейнер найден, ячеек ") : STR("контейнера нет (не в гараже / не установлена)"), found && slots ? std::to_wstring(slots->Num) : std::wstring{});
        }
        if (std::none_of(std::begin(places), std::end(places), [](const Trunk& t) { return t.container != nullptr; })) return;
        // Все контейнеры в одной карте: ключи станции и гоблера начинаются с префикса, с багажником не пересекаются
        auto read_all = [&] {
            ByKey all;
            for (int place = TrunkPlace; place < static_cast<int>(places.size()); ++place)
                if (places[place].container)
                    for (auto& [key, entry] : read_items(places[place].container, place)) all.emplace(key, std::move(entry));
            return all;
        };
        auto place_present = [&](const Key& key) {
            int place = place_of(key);
            return place < static_cast<int>(places.size()) && places[place].container != nullptr;
        };
        auto items = read_all();

        if (is_host)
        {
            bool full = g_send_full;
            std::map<Key, std::pair<Stack, uint16_t>> counts;
            for (auto& [key, entry] : items) counts[key] = {entry.Stack, static_cast<uint16_t>(entry.Items.size())};
            for (auto& [key, was] : g_sent)
                if (!counts.contains(key) && place_present(key)) counts[key] = {was.first, 0}; // стопка пропала — Client должен убрать
            auto unchanged = [&](const Key& key, uint16_t n) {
                auto it = full ? g_sent.end() : g_sent.find(key);
                return it != g_sent.end() ? it->second.second == n : n == 0;
            };
            bool any = std::any_of(counts.begin(), counts.end(), [&](auto& kv) { return !unchanged(kv.first, kv.second.second); });
            // Место пропало (багажник сняли) — его стопки уходят из g_sent ниже, чтобы при возврате багажника разослать их заново
            bool place_gone = std::any_of(g_sent.begin(), g_sent.end(), [&](auto& kv) { return !counts.contains(kv.first); });
            if (!any && !full && !place_gone) return;
            if (dragging()) return; // g_send_full остаётся — полное состояние уйдёт на следующем опросе
            if (full) g_sent.clear();
            g_send_full = false;
            for (auto& [key, now_stack] : counts)
            {
                if (unchanged(key, now_stack.second)) continue;
                if (!Net::send_trunk(Coop::TrunkItem, now_stack.second, now_stack.first.Path, now_stack.first.Values))
                {
                    Output::send<LogLevel::Warning>(STR("[TrunkSync] путь архетипа длиннее 255 — не отправлен\n"));
                    continue;
                }
                if (!full) Output::send<LogLevel::Verbose>(STR("[TrunkSync] отправлено: {} × {}\n"), describe(now_stack.first), now_stack.second);
            }
            if (full) Output::send<LogLevel::Verbose>(STR("[TrunkSync] багажник отправлен: стопок {}\n"), items.size());
            std::erase_if(counts, [](auto& kv) { return kv.second.second == 0; });
            g_sent = std::move(counts);
            return;
        }

        if ((!g_baseline || !g_wanted.empty() || differs_from_known(items)) && dragging()) return; // опору не снимаем с предметом в курсоре
        if (!g_baseline)
        {
            // Опора — наш багажник (тот же сейв, что у Host); стопки, о которых Host уже сообщил, не трогаем
            for (auto& [key, entry] : items)
                if (!g_known.contains(key)) g_known[key] = static_cast<uint16_t>(entry.Items.size());
            g_baseline = true;
        }
        else detect_passenger(items);

        uint8_t context = create_context(items);
        bool applied = false; // меняли багажник — g_prev перечитать; нет — он такой же, как items (второе чтение тысяч предметов не нужно)
        for (auto it = g_wanted.begin(); it != g_wanted.end();)
        {
            if (!place_present(it->first)) // нет багажника/станции — ждём, пока появятся
            {
                ++it;
                continue;
            }
            auto& have = items[it->first].Items;
            applied |= have.size() != it->second.second;
            bool done = have.size() == it->second.second || apply(places[place_of(it->first)], it->first, it->second.first, have, it->second.second, context);
            if (!done && ++g_attempts[it->first] > MaxApplyAttempts)
            {
                Output::send<LogLevel::Warning>(STR("[TrunkSync] {}: не получилось за {} попыток — оставляю как есть\n"), describe(it->second.first), MaxApplyAttempts);
                g_known[it->first] = static_cast<uint16_t>(have.size()); // иначе расхождение приняли бы за действие пассажира
                done = true;
            }
            if (done)
            {
                g_attempts.erase(it->first);
                it = g_wanted.erase(it);
            }
            else ++it; // часть не легла — повторим на следующем опросе
        }
        if (applied) g_prev = read_all(); // после применения: следующий опрос сравнит с этим
        else
        {
            std::erase_if(items, [](auto& kv) { return kv.second.Items.empty(); }); // пустые записи добавил operator[] выше — в чтении их нет
            g_prev = std::move(items);
        }
    }

    // Ctrl+F6 (Diag): что лежит в багажнике, станции и гоблере — число предметов и стопок
    void describe()
    {
        static const TCHAR* names[StoragePlace] = {STR("багажник"), STR("станция"), STR("гоблер"), STR("настройщик маршрута")};
        auto places = find_places();
        for (int place = 0; place < static_cast<int>(places.size()); ++place)
        {
            auto& trunk = places[place];
            auto name = place < StoragePlace ? std::wstring(names[place]) : place >= LockerBase ? std::format(STR("шкаф {}"), place - LockerBase) : std::format(STR("хранилище {}"), place - StoragePlace);
            if (!trunk.container)
            {
                if (place < StoragePlace) Output::send<LogLevel::Verbose>(STR("[Diag] {}: контейнера нет\n"), name);
                continue;
            }
            int total = 0;
            std::string list;
            auto items = read_items(trunk.container, place);
            for (auto& [key, entry] : items)
            {
                total += static_cast<int>(entry.Items.size());
                list += std::format(" {}×{}", entry.Items.size(), entry.Stack.Path.substr(entry.Stack.Path.rfind('/') + 1));
            }
            if (place < StoragePlace) Output::send<LogLevel::Verbose>(STR("[Diag] {}: предметов {};{}\n"), name, total, Items::wide(list));
            else if (place >= LockerBase) Output::send<LogLevel::Verbose>(STR("[Diag] {}: предметов {};{}\n"), name, total, Items::wide(list));
            else if (total) Output::send<LogLevel::Verbose>(STR("[Diag] {}: предметов {}, стопок {}\n"), name, total, items.size()); // список длинный — только числа
        }
    }
} // namespace TrunkSync
