#include "TrunkSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UClass.hpp>

#include "Game/ItemProps.h"
#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <algorithm>
#include <chrono>
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
        constexpr uint64_t PollMs = 1000;
        constexpr uint64_t SettleMs = 5000; // после загрузки карты машина ещё собирается
        constexpr int MaxApplyAttempts = 3;

        struct Trunk
        {
            UObject* car = nullptr;
            UObject* component = nullptr; // BP_InventoryContainerComponent: AddItem сам выбирает ячейку
            UObject* container = nullptr;
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
        int8_t g_seen[3] = {-1, -1, -1}; // контейнер найден (1) / нет (0) / ещё не проверяли (-1) — в лог пишем только смену
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
        enum Place : int
        {
            TrunkPlace,
            IncubatorPlace,
            GobblerPlace,
            PlaceCount
        };
        constexpr std::string_view PlacePrefix[PlaceCount] = {"", "@I:", "@G:"};
        int place_of(const std::string& path)
        {
            for (int place = TrunkPlace + 1; place < PlaceCount; ++place)
                if (path.starts_with(PlacePrefix[place])) return place;
            return TrunkPlace;
        }
        bool in_incubator(const std::string& path) { return place_of(path) == IncubatorPlace; }
        std::string archetype_of(const std::string& path) { return path.substr(PlacePrefix[place_of(path)].size()); }

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
            Trunk trunk{UObjectGlobals::FindFirstOf(STR("NewVehiclePawn"))}; // как CarSync
            if (!trunk.car) return trunk;
            std::vector<UObject*> comps;
            UObjectGlobals::FindAllOf(STR("SlotSceneComponent"), comps);
            for (auto* comp : comps)
            {
                if (comp->GetOuterPrivate() != trunk.car) continue;
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
        // shortcut: перебор всех ItemContainer раз в секунду (~100 объектов), кэша нет — завести кэш, если станет заметно в кадре
        Trunk find_incubator()
        {
            Trunk incubator{UObjectGlobals::FindFirstOf(STR("BP_Garage_PartIncubator_01_C"))};
            if (!incubator.car) return incubator;
            auto** component = property<UObject*>(incubator.car, STR("BP_InventoryContainerComponent"));
            if (!component || !*component) return incubator;
            std::vector<UObject*> containers;
            UObjectGlobals::FindAllOf(STR("ItemContainer"), containers);
            for (auto* candidate : containers)
            {
                auto* owner = candidate->GetOuterPrivate();
                if (owner && Items::archetype_path(owner).find("IA_Garage_PartIncubator") != std::string::npos)
                {
                    incubator.component = *component;
                    incubator.container = candidate;
                    break;
                }
            }
            return incubator;
        }

        Trunk find_place(int place) { return place == IncubatorPlace ? find_incubator() : place == GobblerPlace ? find_gobbler() : find_trunk(); }

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
            auto* slots = property<Items::RawArray>(container, STR("Slots"));
            if (!slots || !slots->Data) return by_key;
            for (int32_t i = 0; i < slots->Num; ++i)
            {
                auto items = Items::items_of(slots->Data[i]);
                for (int32_t k = 0; k < items.Num; ++k)
                {
                    if (!items.Data[k]) continue;
                    Stack stack{Items::archetype_path(items.Data[k]), values_of(items.Data[k])};
                    if (stack.Path.empty()) continue;
                    stack.Path.insert(0, PlacePrefix[place]);
                    auto& entry = by_key[key_of(stack)];
                    if (entry.Items.empty()) entry.Stack = std::move(stack);
                    entry.Items.push_back(items.Data[k]);
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
        bool add_to_trunk(UObject* component, UObject* instance)
        {
            UFunctionCall add(component, STR("AddItem"));
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
                if (!instance || !add_to_trunk(trunk.component, instance))
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
            if (int place = place_of(stack.Path); place != TrunkPlace) name.insert(0, place == IncubatorPlace ? STR("[станция] ") : STR("[гоблер] ")); // в логе видно, где лежит
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
            auto trunk = find_place(place);
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
            Output::send<LogLevel::Warning>(STR("[TrunkSync] Host уже забрал {} × {} — убрано у пассажира {}\n"), describe(stack), count, removed);
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
    }

    void tick(bool is_host)
    {
        if (!Net::connected()) return;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs || now - g_map_loaded_ms < SettleMs) return;
        g_last_poll_ms = now;
        if (dragging()) return; // и Host не шлёт «−1», и Client не принимает перетаскивание за «взял»
        Trunk places[PlaceCount] = {find_trunk(), find_incubator(), find_gobbler()};
        for (int place = TrunkPlace; place < PlaceCount; ++place)
        {
            int8_t found = places[place].container != nullptr;
            if (found == g_seen[place]) continue;
            g_seen[place] = found;
            static const TCHAR* names[PlaceCount] = {STR("багажник"), STR("станция регенерации"), STR("гоблер")};
            auto* slots = found ? property<Items::RawArray>(places[place].container, STR("Slots")) : nullptr;
            Output::send<LogLevel::Verbose>(STR("[TrunkSync] {}: {}{}\n"), names[place], found ? STR("контейнер найден, ячеек ") : STR("контейнера нет (не в гараже / не установлена)"), found && slots ? std::to_wstring(slots->Num) : std::wstring{});
        }
        if (std::none_of(std::begin(places), std::end(places), [](const Trunk& t) { return t.container != nullptr; })) return;
        // Все контейнеры в одной карте: ключи станции и гоблера начинаются с префикса, с багажником не пересекаются
        auto read_all = [&] {
            ByKey all;
            for (int place = TrunkPlace; place < PlaceCount; ++place)
                if (places[place].container)
                    for (auto& [key, entry] : read_items(places[place].container, place)) all.emplace(key, std::move(entry));
            return all;
        };
        auto place_present = [&](const Key& key) { return places[place_of(key)].container != nullptr; };
        auto items = read_all();

        if (is_host)
        {
            if (g_send_full) g_sent.clear();
            bool full = g_send_full;
            g_send_full = false;
            std::map<Key, std::pair<Stack, uint16_t>> counts;
            for (auto& [key, entry] : items) counts[key] = {entry.Stack, static_cast<uint16_t>(entry.Items.size())};
            for (auto& [key, was] : g_sent)
                if (!counts.contains(key) && place_present(key)) counts[key] = {was.first, 0}; // стопка пропала — Client должен убрать
            for (auto& [key, now_stack] : counts)
            {
                auto it = g_sent.find(key);
                if (it != g_sent.end() ? it->second.second == now_stack.second : now_stack.second == 0) continue;
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

        if (!g_baseline)
        {
            // Опора — наш багажник (тот же сейв, что у Host); стопки, о которых Host уже сообщил, не трогаем
            for (auto& [key, entry] : items)
                if (!g_known.contains(key)) g_known[key] = static_cast<uint16_t>(entry.Items.size());
            g_baseline = true;
        }
        else detect_passenger(items);

        uint8_t context = create_context(items);
        for (auto it = g_wanted.begin(); it != g_wanted.end();)
        {
            if (!place_present(it->first)) // нет багажника/станции — ждём, пока появятся
            {
                ++it;
                continue;
            }
            auto& have = items[it->first].Items;
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
        g_prev = read_all(); // после применения: следующий опрос сравнит с этим
    }
} // namespace TrunkSync
