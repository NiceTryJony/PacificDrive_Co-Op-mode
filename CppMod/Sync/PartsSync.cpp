#include "PartsSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/ItemProps.h"
#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Host раз в секунду читает слоты своей машины и шлёт изменившиеся (один слот — один пакет). Опрос, а не хук OnSwapPartEnd:
// так ловятся и снятие руками, и детали, сбитые ударом (PopOffPartFromCar), и колёса (PopWheelOff).
// Client держит присланное и приводит свою машину к нему: убирает лишнее (ItemInstance.Remove) и ставит нужное
// (CreateItemInstance + ItemSlot.AddItem) — дальше игра сама меняет видимую деталь.
namespace PartsSync
{
    namespace
    {
        constexpr uint64_t PollMs = 300; // цепочка Client → Host → Client = опросы × дребезг (2 опроса) + сеть; при 1000 была ~4 с
        constexpr uint64_t SettleMs = 5000; // после загрузки карты слоты ещё заполняются — пустые не рассылать и не применять
        constexpr int MaxApplyAttempts = 3;
        // Внутренние слоты (хранилище, крафт, «душа», гоблер) не трогаем: там инвентарь и прогресс, ошибка стоит дорого
        const char* SyncedPrefixes[] = {"Car.Equip.Body.", "Car.Equip.Essentials.", "Car.Equip.Attachments.", "Car.Equip.Performance.", "Car.Equip.Cosmetics."};

        using Items::narrow;
        using Items::wide;

        struct Slot
        {
            UObject* item_slot = nullptr;
            UObject* instance = nullptr; // первая деталь в слоте (у слотов машины ёмкость 1)
            std::string path;            // значение слота: путь архетипа[|эффект|эффект…]; пусто — слот пуст
            std::string health;          // IP_Health.Health текстом; пусто — у детали нет здоровья
            UObject* actor = nullptr;    // видимая деталь (SpawnedActor); на ней компонент краски
            std::string tint;            // «путь класса краски~путь класса оклейки»; пусто — нет компонента/ещё не готов
            std::map<int, std::string> levels; // индекс ItemProps::Specs → текст: топливо и заряд детали (баки, аккумуляторы)
        };

        // Здоровье в сравнении «что изменилось» не участвует (меняется от каждого удара), но едет в пакете после '#':
        // «значение#здоровье». '#' в путях объектов UE не бывает
        constexpr char HealthMark = '#';
        constexpr float NoHealth = std::numeric_limits<float>::quiet_NaN();

        // Краска и оклейка едут после '~': «значение#здоровье~краска~оклейка». '~' в путях объектов UE не бывает
        constexpr char TintMark = '~';
        // Уровни топлива и заряда — после '^': «…^1=65.6,2=17.5» (номер ItemProps::Specs=значение)
        constexpr char LevelsMark = '^';
        constexpr int LevelSpecs[] = {1, 2}; // IP_Fuelable.Fuel, IP_Battery.Battery

        std::string levels_text(const std::map<int, std::string>& levels)
        {
            std::string out;
            for (auto& [spec, value] : levels) out += (out.empty() ? "" : ",") + std::to_string(spec) + '=' + value;
            return out;
        }

        std::map<int, float> parse_levels(const std::string& text)
        {
            std::map<int, float> out;
            for (size_t pos = 0; pos < text.size();)
            {
                size_t end = text.find(',', pos);
                if (end == std::string::npos) end = text.size();
                auto part = text.substr(pos, end - pos);
                pos = end + 1;
                auto eq = part.find('=');
                if (eq == std::string::npos) continue;
                try
                {
                    int spec = std::stoi(part.substr(0, eq));
                    if (std::find(std::begin(LevelSpecs), std::end(LevelSpecs), spec) != std::end(LevelSpecs)) out[spec] = std::stof(part.substr(eq + 1));
                }
                catch (const std::exception&) // мусор из сети не роняет игру
                {
                }
            }
            return out;
        }

        // Порог «уровень изменился»: 2 % (не меньше 0.5) — расход на ходу не шлём каждый опрос
        float level_step(float value) { return (std::max)(0.5f, 0.02f * std::fabs(value)); }

        bool levels_differ(const std::map<int, float>& a, const std::map<int, float>& b)
        {
            if (a.size() != b.size()) return true;
            for (auto& [spec, value] : a)
            {
                auto it = b.find(spec);
                if (it == b.end() || std::fabs(it->second - value) >= level_step(value)) return true;
            }
            return false;
        }

        std::string wire_of(const Slot& slot)
        {
            if (slot.path.empty()) return slot.path;
            std::string wire = slot.path;
            if (!slot.health.empty()) wire += HealthMark + slot.health;
            if (!slot.tint.empty()) wire += TintMark + slot.tint;
            if (!slot.levels.empty()) wire += LevelsMark + levels_text(slot.levels);
            return wire;
        }

        struct Wire
        {
            std::string path, health, tint, levels;
        };

        Wire split_wire(const std::string& wire)
        {
            Wire out;
            std::string rest = wire;
            if (auto mark = rest.find(LevelsMark); mark != std::string::npos)
            {
                out.levels = rest.substr(mark + 1);
                rest.resize(mark);
            }
            if (auto mark = rest.find(TintMark); mark != std::string::npos)
            {
                out.tint = rest.substr(mark + 1);
                rest.resize(mark);
            }
            if (auto mark = rest.find(HealthMark); mark != std::string::npos)
            {
                out.health = rest.substr(mark + 1);
                rest.resize(mark);
            }
            out.path = rest;
            return out;
        }

        float to_float(const std::string& text)
        {
            if (text.empty()) return NoHealth;
            try
            {
                return std::stof(text);
            }
            catch (const std::exception&) // мусор из сети не роняет игру
            {
                return NoHealth;
            }
        }

        // Порог «здоровье изменилось»: 2 % (не меньше 1 единицы) — мелкий износ на ходу не шлём каждый опрос
        float health_step(float value) { return (std::max)(1.0f, 0.02f * std::fabs(value)); }

        // Значение слота = архетип + через '|' классы эффектов состояния (протечка и т.п.), отсортированные: так сравнение строк
        // замечает и смену детали, и появление/пропажу эффекта
        std::string archetype_of(const std::string& value) { return value.substr(0, value.find('|')); }

        std::vector<std::string> effects_in(const std::string& value)
        {
            std::vector<std::string> out;
            for (size_t pos = value.find('|'); pos != std::string::npos;)
            {
                size_t next = value.find('|', pos + 1);
                if (auto effect = value.substr(pos + 1, next == std::string::npos ? next : next - pos - 1); !effect.empty()) out.push_back(effect);
                pos = next;
            }
            return out;
        }

        // У нас эффекты — часть известных Host-у и чего-то не хватает: игрок починил
        bool effects_repaired(const std::string& mine, const std::string& known)
        {
            auto have = effects_in(mine), host = effects_in(known);
            if (have.size() >= host.size()) return false;
            return std::includes(host.begin(), host.end(), have.begin(), have.end()); // оба списка отсортированы, повторы считаются
        }

        // Привести эффекты предмета к списку. false — что-то не удалось (менеджера нет, класс не загружен)
        bool sync_effects(UObject* instance, const std::vector<std::string>& wanted)
        {
            bool ok = true;
            std::set<std::string> classes(wanted.begin(), wanted.end());
            for (auto& effect : Items::effects_of(instance)) classes.insert(effect);
            for (auto& effect : classes)
            {
                auto count_of = [&](const std::vector<std::string>& list) { return static_cast<int>(std::count(list.begin(), list.end(), effect)); };
                int want = count_of(wanted);
                // по одному, перечитывая: неизвестно, снимает ли Remove один экземпляр или все — если счётчик не сдвинулся, дальше не долбим
                for (int guard = 0; guard < 16; ++guard)
                {
                    int have = count_of(Items::effects_of(instance));
                    if (have == want) break;
                    if (!Items::change_effect(instance, effect, have < want)) { ok = false; break; }
                    if (count_of(Items::effects_of(instance)) == have) { ok = false; break; }
                }
            }
            return ok;
        }

        bool g_send_full = false;
        uint64_t g_last_poll_ms = 0, g_map_loaded_ms = 0;
        std::map<std::string, std::string> g_sent;   // Host: что Client уже знает
        // Приводим машину к желаемому: Client — к присланному Host-ом, Host — к запросу Client-а
        std::map<std::string, std::string> g_wanted;
        std::map<std::string, int> g_attempts;
        std::map<std::string, std::string> g_known;    // Client: последнее, что Host сообщил о слоте
        std::map<std::string, std::string> g_seen;     // Client: расхождение с g_known, замеченное прошлым опросом (дребезг при замене)
        std::map<std::string, std::string> g_reported; // Client: что уже отправлено Host-у, повторно не шлём
        bool g_client_parts = true;
        std::map<std::string, std::string> g_diag; // Client: последнее залогированное расхождение по слоту

        // Краска и оклейка (RECON.md «6.24»): Host шлёт вместе со слотом, Client применяет штатным SetPaint/SetDecal компонента
        // BP_TintableComponent_C на видимой детали; перекрасил Client — просит Host (как со здоровьем)
        struct TintWant
        {
            std::string archetype; // для какой детали краска: стоит другая — не применяем
            std::string tint;
            bool log;              // писать в лог (по запросу Client-а)
        };
        std::map<std::string, std::string> g_sent_tint;    // Host: что Client уже знает
        std::map<std::string, std::string> g_known_tint;   // Client: последняя краска от Host-а (переживает загрузку карты)
        std::map<std::string, TintWant> g_tint_wanted;
        std::map<std::string, int> g_tint_attempts;
        std::map<std::string, std::string> g_tint_seen;    // Client: расхождение замечено прошлым опросом (дребезг)
        std::map<std::string, std::string> g_tint_reported; // Client: какая краска уже отправлена Host-у

        // Топливо и заряд деталей (баки, аккумуляторы; RECON.md «6.25»): как здоровье — Host шлёт, Client подтягивается, а если у Client
        // стало больше (заправил, зарядил) — просит Host. SystemsSync уровни Client-у не применяет (только проценты для окна статуса)
        struct LevelsWant
        {
            std::string archetype;
            std::map<int, float> values;
            bool log;      // писать в лог (по запросу Client-а)
            bool only_up;  // Host по запросу Client-а: принимать только рост
        };
        std::map<std::string, std::map<int, float>> g_sent_levels;  // Host: что Client уже знает
        std::map<std::string, std::map<int, float>> g_known_levels; // Client: последнее слово Host-а
        std::map<std::string, LevelsWant> g_levels_wanted;
        std::map<std::string, int> g_levels_attempts;
        std::set<std::string> g_levels_seen;                   // Client: «больше, чем у Host» замечено прошлым опросом
        std::map<std::string, std::string> g_levels_reported;  // Client: какие уровни уже отправлены Host-у

        // Здоровье деталей (docs/SYNC_PLAN.md, правило 3). Host шлёт при смене слота и при изменении ≥ health_step; Client подтягивается
        // к Host-у, а если у него стало больше (Repair Putty, установка починенной детали) — просит Host. Урон Client не получает:
        // физика куклы выключена, поэтому «больше» = действие игрока
        struct HealthWant
        {
            std::string archetype; // для какой детали значение: стоит другая — не пишем
            float value;
            bool log;              // писать в лог (по запросу Client-а); подтягивание за износом Host-а — без лога
            bool only_up;          // Host по запросу Client-а: принимать только рост (ремонт); у Client значение могло отстать.
                                   // Сбрасывается, если по тому же запросу Host ставит новую деталь — тогда здоровье как у Client
        };
        std::map<std::string, float> g_sent_health;       // Host: что Client уже знает
        std::map<std::string, float> g_known_health;      // Client: последнее здоровье от Host-а (переживает загрузку карты)
        std::map<std::string, HealthWant> g_health_wanted; // привести здоровье детали к этому
        std::map<std::string, int> g_health_attempts;
        std::set<std::string> g_health_seen;              // Client: «больше, чем у Host» замечено прошлым опросом (дребезг)
        std::map<std::string, std::string> g_health_reported; // Client: какое здоровье уже отправлено Host-у

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        bool synced(const std::string& tag)
        {
            for (auto* prefix : SyncedPrefixes)
                if (tag.starts_with(prefix)) return true;
            return false;
        }

        // Компоненты краски по актёрам-владельцам: на видимой детали он один
        std::map<UObject*, UObject*> tint_components_by_actor()
        {
            std::vector<UObject*> comps;
            UObjectGlobals::FindAllOf(STR("BP_TintableComponent_C"), comps);
            std::map<UObject*, UObject*> out;
            for (auto* comp : comps)
                if (comp) out[comp->GetOuterPrivate()] = comp;
            return out;
        }

        // «краска~оклейка» пути классов палитр; пусто — компонент не готов или данных нет
        std::string read_tint(UObject* component)
        {
            UFunctionCall get(component, STR("GetTintableProperties"));
            auto* paint = get ? get.param<UObject*>(STR("Paint")) : nullptr;
            auto* decal = get ? get.param<UObject*>(STR("DecalPalette")) : nullptr;
            auto* valid = get ? get.param<bool>(STR("Valid")) : nullptr;
            if (!paint || !decal || !valid) return {};
            get.call();
            if (!*valid || !*paint || !*decal) return {};
            return narrow((*paint)->GetPathName()) + TintMark + narrow((*decal)->GetPathName());
        }

        bool set_palette(UObject* component, const TCHAR* function, const TCHAR* param_name, const std::string& class_path)
        {
            auto* palette = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, wide(class_path).c_str());
            if (!palette) return false; // класс палитры у нас не загружен
            UFunctionCall set(component, function);
            auto* p_palette = set ? set.param<UObject*>(param_name) : nullptr;
            if (!p_palette) return false;
            *p_palette = palette; // ColorTag остаётся пустым: игра его принимает (RECON.md «6.24»)
            set.call();
            return true;
        }

        UObject* find_car() { return UObjectGlobals::FindFirstOf(STR("NewVehiclePawn")); } // как CarSync

        // Слоты машины по тегу. Теги, встречающиеся дважды (у машины есть такие), пропускаем: порядок компонентов у Host
        // и Client может разниться, и слот попал бы не в тот
        std::map<std::string, Slot> read_slots(UObject* car)
        {
            std::vector<UObject*> comps;
            UObjectGlobals::FindAllOf(STR("SlotSceneComponent"), comps);
            std::map<std::string, Slot> slots;
            std::set<std::string> twice;
            auto tint_components = tint_components_by_actor();
            for (auto* comp : comps)
            {
                if (comp->GetOuterPrivate() != car) continue;
                auto* tag_name = property<FName>(comp, STR("SlotName")); // FGameplayTag = одно FName TagName
                auto** item_slot = property<UObject*>(comp, STR("ItemSlot"));
                if (!tag_name || !item_slot || !*item_slot) continue;
                std::string tag = narrow(tag_name->ToString());
                if (!synced(tag)) continue;
                if (slots.contains(tag)) twice.insert(tag);
                Slot slot{*item_slot};
                if (auto** actor = property<UObject*>(comp, STR("SpawnedActor")); actor && *actor)
                {
                    slot.actor = *actor;
                    if (auto it = tint_components.find(*actor); it != tint_components.end()) slot.tint = read_tint(it->second);
                }
                if (auto items = Items::items_of(*item_slot); items.Num > 0 && items.Data[0])
                {
                    slot.instance = items.Data[0];
                    slot.path = Items::archetype_path(slot.instance);
                    if (!slot.path.empty())
                    {
                        for (auto& effect : Items::effects_of(slot.instance)) slot.path += '|' + effect;
                        slot.health = ItemProps::read(slot.instance, ItemProps::Health);
                        for (int spec : LevelSpecs)
                            if (auto text = ItemProps::read(slot.instance, ItemProps::Specs[spec]); !text.empty()) slot.levels[spec] = text;
                    }
                }
                slots[tag] = slot;
            }
            for (auto& tag : twice) slots.erase(tag);
            return slots;
        }

        bool any_installed(const std::map<std::string, Slot>& slots)
        {
            for (auto& [tag, slot] : slots)
                if (!slot.path.empty()) return true;
            return false;
        }

        // Привести слот к нужной детали. false — не вышло (лог уже написан)
        bool apply(UObject* car, const std::string& tag, const Slot& slot, const std::string& wanted_value, uint8_t context)
        {
            std::string wanted = archetype_of(wanted_value);
            auto wanted_effects = effects_in(wanted_value);
            if (slot.instance && !wanted.empty() && Items::archetype_path(slot.instance) == wanted)
            {
                // Деталь та же, разошлись только эффекты — деталь не пересоздаём
                bool ok = sync_effects(slot.instance, wanted_effects);
                Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: эффекты → {}{}\n"), wide(tag), wanted_effects.size(), ok ? STR("") : STR(" (не все применились)"));
                return ok;
            }
            if (slot.instance) Items::remove(slot.instance);
            if (wanted.empty())
            {
                Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: деталь снята\n"), wide(tag));
                return true;
            }
            auto* instance = Items::create(car, wanted, context);
            if (!instance)
            {
                Output::send<LogLevel::Warning>(STR("[PartsSync] {}: не создать {} (архетип не загружен или CreateItemInstance не сработал) — слот пуст\n"), wide(tag), wide(wanted));
                return false;
            }
            if (!Items::add_to_slot(slot.item_slot, instance))
            {
                Output::send<LogLevel::Error>(STR("[PartsSync] нет ItemSlot.AddItem/Item\n"));
                return false;
            }
            Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: поставлена {}\n"), wide(tag), wide(wanted));
            if (!wanted_effects.empty() && !sync_effects(instance, wanted_effects)) Output::send<LogLevel::Warning>(STR("[PartsSync] {}: не все эффекты применились\n"), wide(tag));
            return true;
        }

        uint8_t create_context(const std::map<std::string, Slot>& slots)
        {
            for (auto& [tag, slot] : slots)
                if (slot.instance) return Items::create_context(slot.instance);
            return 0;
        }

        // Записать нужное здоровье деталям, которые уже стоят (пока слот ещё меняется — ждём). Совпадение проверяет следующий опрос
        void apply_health(const std::map<std::string, Slot>& slots)
        {
            for (auto it = g_health_wanted.begin(); it != g_health_wanted.end();)
            {
                const auto& [tag, want] = *it;
                if (g_wanted.contains(tag)) // деталь ещё ставится
                {
                    ++it;
                    continue;
                }
                auto slot = slots.find(tag);
                float mine = slot != slots.end() ? to_float(slot->second.health) : NoHealth;
                bool same_part = slot != slots.end() && slot->second.instance && archetype_of(slot->second.path) == want.archetype;
                if (!same_part || std::isnan(mine) || std::fabs(mine - want.value) < 0.01f) // другая деталь / нет здоровья / уже совпало
                {
                    g_health_attempts.erase(tag);
                    it = g_health_wanted.erase(it);
                    continue;
                }
                if (want.only_up && want.value < mine)
                {
                    // У Client отставшее значение — не «чиним вниз»; перешлём своё, Client подтянется (правило 4)
                    g_sent_health.erase(tag);
                    g_health_attempts.erase(tag);
                    it = g_health_wanted.erase(it);
                    continue;
                }
                if (++g_health_attempts[tag] > MaxApplyAttempts)
                {
                    Output::send<LogLevel::Warning>(STR("[PartsSync] {}: здоровье {} не записалось за {} попыток (стоит {})\n"), wide(tag), want.value, MaxApplyAttempts, mine);
                    g_sent_health.erase(tag); // Host: Client ждёт ответа — перешлём настоящее значение
                    g_health_attempts.erase(tag);
                    it = g_health_wanted.erase(it);
                    continue;
                }
                bool ok = ItemProps::write(slot->second.instance, ItemProps::Health, std::format("{}", want.value));
                if (want.log) Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: здоровье {} → {}{}\n"), wide(tag), mine, want.value, ok ? STR("") : STR(" (игра не приняла)"));
                ++it;
            }
        }

        // Применить присланную краску деталям, которые уже стоят (пока слот ещё меняется — ждём). Совпадение проверяет следующий опрос
        void apply_tint(const std::map<std::string, Slot>& slots, bool is_host)
        {
            auto tint_components = tint_components_by_actor();
            for (auto it = g_tint_wanted.begin(); it != g_tint_wanted.end();)
            {
                const auto& [tag, want] = *it;
                if (g_wanted.contains(tag))
                {
                    ++it;
                    continue;
                }
                auto slot = slots.find(tag);
                bool same_part = slot != slots.end() && slot->second.instance && archetype_of(slot->second.path) == want.archetype;
                if (!same_part || slot->second.tint.empty() || slot->second.tint == want.tint) // другая деталь / компонент не готов / уже совпало
                {
                    g_tint_attempts.erase(tag);
                    it = g_tint_wanted.erase(it);
                    continue;
                }
                if (++g_tint_attempts[tag] > MaxApplyAttempts)
                {
                    Output::send<LogLevel::Warning>(STR("[PartsSync] {}: краска не применилась за {} попыток — оставляю как есть\n"), wide(tag), MaxApplyAttempts);
                    if (is_host) g_sent_tint.erase(tag); // Client ждёт ответа — перешлём настоящую
                    else g_known_tint[tag] = slot->second.tint;
                    g_tint_attempts.erase(tag);
                    it = g_tint_wanted.erase(it);
                    continue;
                }
                auto component = tint_components.find(slot->second.actor);
                if (component == tint_components.end())
                {
                    ++it;
                    continue;
                }
                auto split = [](const std::string& tint) { auto mark = tint.find(TintMark); return std::pair{tint.substr(0, mark), mark == std::string::npos ? std::string{} : tint.substr(mark + 1)}; };
                auto [have_paint, have_decal] = split(slot->second.tint);
                auto [want_paint, want_decal] = split(want.tint);
                bool ok = true;
                if (have_paint != want_paint) ok = set_palette(component->second, STR("SetPaint"), STR("Paint"), want_paint) && ok;
                if (have_decal != want_decal) ok = set_palette(component->second, STR("SetDecal"), STR("Decal"), want_decal) && ok;
                if (want.log || !ok)
                    Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: краска {}{}\n"), wide(tag), wide(want_paint.substr(want_paint.rfind('.') + 1)), ok ? STR("") : STR(" (палитра не загружена или игра не приняла)"));
                ++it;
            }
        }

        // Записать присланные уровни деталям, которые уже стоят (пока слот ещё меняется — ждём). Совпадение проверяет следующий опрос
        void apply_levels(const std::map<std::string, Slot>& slots, bool is_host)
        {
            for (auto it = g_levels_wanted.begin(); it != g_levels_wanted.end();)
            {
                const auto& [tag, want] = *it;
                if (g_wanted.contains(tag))
                {
                    ++it;
                    continue;
                }
                auto slot = slots.find(tag);
                bool same_part = slot != slots.end() && slot->second.instance && archetype_of(slot->second.path) == want.archetype;
                std::map<int, float> todo;
                if (same_part)
                    for (auto& [spec, value] : want.values)
                    {
                        auto mine = slot->second.levels.find(spec);
                        if (mine == slot->second.levels.end()) continue; // у детали нет такого уровня
                        float have = to_float(mine->second);
                        if (std::isnan(have) || std::fabs(have - value) < 0.01f) continue;
                        if (want.only_up && value < have) continue; // Host по запросу Client-а: вниз не «чиним»
                        todo[spec] = value;
                    }
                if (todo.empty())
                {
                    g_levels_attempts.erase(tag);
                    if (want.only_up && is_host) g_sent_levels.erase(tag); // не приняли «вниз» — перешлём Client-у настоящее
                    it = g_levels_wanted.erase(it);
                    continue;
                }
                if (++g_levels_attempts[tag] > MaxApplyAttempts)
                {
                    Output::send<LogLevel::Warning>(STR("[PartsSync] {}: уровни не записались за {} попыток — оставляю как есть\n"), wide(tag), MaxApplyAttempts);
                    if (is_host) g_sent_levels.erase(tag);
                    else g_known_levels.erase(tag);
                    g_levels_attempts.erase(tag);
                    it = g_levels_wanted.erase(it);
                    continue;
                }
                for (auto& [spec, value] : todo)
                {
                    bool ok = ItemProps::write(slot->second.instance, ItemProps::Specs[spec], std::format("{}", value));
                    if (want.log || !ok)
                        Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: {} → {}{}\n"), wide(tag), wide(ItemProps::Specs[spec].Field), value, ok ? STR("") : STR(" (игра не приняла)"));
                }
                ++it;
            }
        }
    } // namespace

    void on_part(const std::string& tag, const std::string& wire)
    {
        if (!synced(tag)) return; // мусор/чужая версия — внутренние слоты не трогаем
        auto [path, health_text, tint, levels_str] = split_wire(wire);
        g_wanted[tag] = path;
        g_known[tag] = path;
        g_levels_seen.erase(tag);
        g_levels_reported.erase(tag);
        g_levels_attempts.erase(tag);
        if (auto levels = parse_levels(levels_str); !levels.empty())
        {
            g_known_levels[tag] = levels;
            g_levels_wanted[tag] = {archetype_of(path), levels, false, false};
        }
        else
        {
            g_known_levels.erase(tag);
            g_levels_wanted.erase(tag);
        }
        g_tint_seen.erase(tag);
        g_tint_reported.erase(tag);
        g_tint_attempts.erase(tag);
        if (!tint.empty())
        {
            g_known_tint[tag] = tint;
            g_tint_wanted[tag] = {archetype_of(path), tint, true}; // краска меняется редко — пишем в лог, чтобы видеть применение у Client
        }
        else
        {
            g_known_tint.erase(tag);
            g_tint_wanted.erase(tag);
        }
        g_seen.erase(tag);
        g_reported.erase(tag);
        g_attempts.erase(tag);
        g_health_seen.erase(tag);
        g_health_reported.erase(tag);
        g_health_attempts.erase(tag);
        if (float health = to_float(health_text); !std::isnan(health))
        {
            g_known_health[tag] = health;
            g_health_wanted[tag] = {archetype_of(path), health, false, false};
        }
        else
        {
            g_known_health.erase(tag);
            g_health_wanted.erase(tag);
        }
    }

    void on_part_request(const std::string& tag, const std::string& wire)
    {
        if (!g_client_parts || !synced(tag)) return;
        auto [path, health_text, tint, levels_str] = split_wire(wire);
        Output::send<LogLevel::Verbose>(STR("[PartsSync] Client просит {} → {}{}{}\n"), wide(tag), path.empty() ? STR("(пусто)") : wide(path),
                                        health_text.empty() ? STR("") : STR(", здоровье "), wide(health_text));
        g_wanted[tag] = path;
        g_attempts.erase(tag);
        g_health_attempts.erase(tag);
        g_tint_attempts.erase(tag);
        g_levels_attempts.erase(tag);
        if (auto levels = parse_levels(levels_str); !levels.empty()) g_levels_wanted[tag] = {archetype_of(path), levels, true, true};
        if (!tint.empty()) g_tint_wanted[tag] = {archetype_of(path), tint, true};
        if (float health = to_float(health_text); !std::isnan(health)) g_health_wanted[tag] = {archetype_of(path), health, true, true};
    }

    void set_client_parts(bool on) { g_client_parts = on; }

    void on_connected(bool is_host)
    {
        if (is_host) g_send_full = true;
    }

    void on_map_loaded()
    {
        g_map_loaded_ms = now_ms();
        g_sent.clear(); // новая карта — Host шлёт все слоты заново
        g_sent_health.clear();
        g_sent_tint.clear();
        g_sent_levels.clear();
        g_send_full = true;
        // Здоровье в свежем сейве зоны уже то, что у Host; g_known_health оставляем (правило 1), очереди записи — сбрасываем
        g_health_wanted.clear();
        g_health_attempts.clear();
        g_health_seen.clear();
        g_health_reported.clear();
        // Client: слово Host-а не теряем (он слоты заново не пришлёт — сейв зоны приходит уже после первой присылки), а привести к нему
        // новые слоты — обычная работа g_wanted; сравнивать начнём после SettleMs, когда слоты заполнятся
        for (auto& [tag, path] : g_known) g_wanted[tag] = path;
        g_attempts.clear();
        g_seen.clear();
        g_reported.clear();
        g_levels_wanted.clear();
        g_levels_attempts.clear();
        g_levels_seen.clear();
        g_levels_reported.clear();
        for (auto& [tag, levels] : g_known_levels)
            if (auto known = g_known.find(tag); known != g_known.end()) g_levels_wanted[tag] = {archetype_of(known->second), levels, false, false};
        // Краска: у Host в новой зоне уже то, что в сейве; Client приводим к последнему слову Host-а
        g_tint_wanted.clear();
        g_tint_attempts.clear();
        g_tint_seen.clear();
        g_tint_reported.clear();
        for (auto& [tag, tint] : g_known_tint)
            if (auto known = g_known.find(tag); known != g_known.end()) g_tint_wanted[tag] = {archetype_of(known->second), tint, false};
    }

    void tick(bool is_host)
    {
        if (!Net::connected()) return;
        uint64_t now = now_ms();
        if (now - g_last_poll_ms < PollMs || now - g_map_loaded_ms < SettleMs) return;
        g_last_poll_ms = now;
        auto* car = find_car();
        if (!car) return;
        auto slots = read_slots(car);
        if (!any_installed(slots)) return; // машина ещё не собрана (загрузка) — пустые слоты не рассылать и не «чинить»

        uint8_t context = create_context(slots);
        for (auto it = g_wanted.begin(); it != g_wanted.end();)
        {
            auto slot = slots.find(it->first);
            if (slot == slots.end() || slot->second.path == it->second) // такого слота у нас нет или уже совпало
            {
                g_attempts.erase(it->first);
                it = g_wanted.erase(it);
                continue;
            }
            if (++g_attempts[it->first] > MaxApplyAttempts)
            {
                Output::send<LogLevel::Warning>(STR("[PartsSync] {}: не совпало за {} попыток — оставляю как есть\n"), wide(it->first), MaxApplyAttempts);
                // Принимаем реальность без отправки: Host заново сообщит свой слот, Client не станет «исправлять» Host-а своим запросом
                if (is_host) g_sent.erase(it->first);
                else g_known[it->first] = slot->second.path;
                g_attempts.erase(it->first);
                it = g_wanted.erase(it);
                continue;
            }
            // Ставится другая деталь (или та же заново) — новая создаётся с исходным здоровьем, присланное значение пишем как есть
            if (auto health = g_health_wanted.find(it->first); health != g_health_wanted.end() && archetype_of(slot->second.path) != archetype_of(it->second))
                health->second.only_up = false;
            apply(car, it->first, slot->second, it->second, context);
            ++it; // совпадение проверит следующий опрос
        }
        apply_health(slots);
        apply_tint(slots, is_host);
        apply_levels(slots, is_host);

        if (is_host)
        {
            if (g_send_full)
            {
                g_sent.clear();
                g_sent_health.clear();
            }
            g_send_full = false;
            int sent = 0;
            for (auto& [tag, slot] : slots)
            {
                auto it = g_sent.find(tag);
                bool path_changed = it == g_sent.end() || it->second != slot.path;
                float health = to_float(slot.health);
                auto sent_health = g_sent_health.find(tag);
                bool health_changed = !std::isnan(health) && (sent_health == g_sent_health.end() || std::fabs(health - sent_health->second) >= health_step(sent_health->second));
                auto sent_tint = g_sent_tint.find(tag);
                bool tint_changed = !slot.tint.empty() && (sent_tint == g_sent_tint.end() || sent_tint->second != slot.tint);
                std::map<int, float> levels;
                for (auto& [spec, text] : slot.levels)
                    if (float value = to_float(text); !std::isnan(value)) levels[spec] = value;
                auto sent_levels = g_sent_levels.find(tag);
                bool levels_changed = !levels.empty() && (sent_levels == g_sent_levels.end() || levels_differ(sent_levels->second, levels));
                if (!path_changed && !health_changed && !tint_changed && !levels_changed) continue;
                if (!Net::send_part(tag, wire_of(slot)))
                {
                    Output::send<LogLevel::Warning>(STR("[PartsSync] {}: значение слота длиннее 65535 — не отправлено\n"), wide(tag));
                    continue;
                }
                if (std::isnan(health)) g_sent_health.erase(tag);
                else g_sent_health[tag] = health;
                if (slot.tint.empty()) g_sent_tint.erase(tag);
                else g_sent_tint[tag] = slot.tint;
                if (levels.empty()) g_sent_levels.erase(tag);
                else g_sent_levels[tag] = levels;
                if (tint_changed && !path_changed && !health_changed) Output::send<LogLevel::Verbose>(STR("[PartsSync] отправлена краска: {}\n"), wide(tag));
                if (!path_changed) continue; // только здоровье (износ на ходу) и краска — без лога слота
                bool first = it == g_sent.end();
                g_sent[tag] = slot.path;
                ++sent;
                if (!first) Output::send<LogLevel::Verbose>(STR("[PartsSync] отправлено: {} → {}\n"), wide(tag), slot.path.empty() ? STR("(пусто)") : wide(slot.path));
            }
            if (sent > 1) Output::send<LogLevel::Verbose>(STR("[PartsSync] отправлено слотов: {}\n"), sent);
            return;
        }

        // Client: слот разошёлся с тем, что сообщил Host, и мы сами его не меняли — деталь поставил/снял игрок
        for (auto& [tag, slot] : slots)
        {
            auto known = g_known.find(tag);
            if (known == g_known.end() || g_wanted.contains(tag)) continue;
            if (slot.path != known->second && g_diag[tag] != slot.path) // диагностика: каждое новое расхождение слота с словом Host-а — в лог один раз
            {
                g_diag[tag] = slot.path;
                Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: расхождение с Host — у меня {} эффектов, у Host {}, деталь {}\n"), wide(tag), effects_in(slot.path).size(), effects_in(known->second).size(),
                                                archetype_of(slot.path) == archetype_of(known->second) ? STR("та же") : STR("другая"));
            }
            // Деталь та же: эффект, появившийся на кукле сам, не действие игрока (Host вернёт своё), а вот пропавший — ремонт, о нём говорим Host-у
            if (archetype_of(slot.path) == archetype_of(known->second) && !effects_repaired(slot.path, known->second))
            {
                g_seen.erase(tag);
                g_reported.erase(tag);
                continue;
            }
            if (!g_client_parts)
            {
                Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: изменять детали Client-у запрещено (client_parts=0) — откатываю\n"), wide(tag));
                g_wanted[tag] = known->second;
                g_attempts.erase(tag);
                continue;
            }
            auto seen = g_seen.find(tag);
            if (seen == g_seen.end() || seen->second != slot.path) // при замене деталь на миг исчезает — ждём второго опроса
            {
                g_seen[tag] = slot.path;
                continue;
            }
            auto reported = g_reported.find(tag);
            if (reported != g_reported.end() && reported->second == slot.path) continue;
            if (!Net::send_part(tag, wire_of(slot), Coop::CarPartRequest)) continue; // со здоровьем: снятую и починенную деталь Host поставит такой же
            g_reported[tag] = slot.path;
            Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: деталь изменена игроком → прошу Host: {}\n"), wide(tag), slot.path.empty() ? STR("(пусто)") : wide(slot.path));
        }

        // Client: здоровье той же детали разошлось с Host-ом
        for (auto& [tag, slot] : slots)
        {
            if (g_wanted.contains(tag) || g_health_wanted.contains(tag)) continue; // ещё применяем слово Host-а
            auto known = g_known.find(tag);
            auto known_health = g_known_health.find(tag);
            float mine = to_float(slot.health);
            if (known == g_known.end() || known_health == g_known_health.end() || std::isnan(mine) || archetype_of(slot.path) != archetype_of(known->second)) continue;
            float host = known_health->second, step = health_step(host);
            if (mine < host - step || (mine > host + step && !g_client_parts)) // просело само (урон кукле) или Client-у менять нельзя — прав Host
            {
                g_health_wanted[tag] = {archetype_of(slot.path), host, false, false};
                continue;
            }
            if (mine <= host + step)
            {
                g_health_seen.erase(tag);
                continue;
            }
            if (g_health_seen.insert(tag).second) continue; // стало больше — подтверждаем вторым опросом
            if (auto reported = g_health_reported.find(tag); reported != g_health_reported.end() && reported->second == slot.health) continue;
            if (!Net::send_part(tag, wire_of(slot), Coop::CarPartRequest)) continue;
            g_health_reported[tag] = slot.health;
            Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: деталь починена игроком (здоровье {} → {}) → прошу Host\n"), wide(tag), host, mine);
        }

        // Client: топливо/заряд той же детали стали больше, чем сообщил Host — заправил или зарядил игрок
        for (auto& [tag, slot] : slots)
        {
            if (g_wanted.contains(tag) || g_levels_wanted.contains(tag)) continue; // ещё применяем слово Host-а
            auto known = g_known.find(tag);
            auto known_levels = g_known_levels.find(tag);
            if (known == g_known.end() || known_levels == g_known_levels.end() || archetype_of(slot.path) != archetype_of(known->second)) continue;
            std::map<int, float> pull; // просело само (расход куклы) или Client-у менять нельзя — прав Host
            bool grew = false;
            for (auto& [spec, host] : known_levels->second)
            {
                auto mine_text = slot.levels.find(spec);
                float mine = mine_text != slot.levels.end() ? to_float(mine_text->second) : NoHealth;
                if (std::isnan(mine)) continue;
                float step = level_step(host);
                if (mine < host - step || (mine > host + step && !g_client_parts)) pull[spec] = host;
                else if (mine > host + step) grew = true;
            }
            if (!pull.empty())
            {
                g_levels_wanted[tag] = {archetype_of(slot.path), known_levels->second, false, false};
                continue;
            }
            if (!grew)
            {
                g_levels_seen.erase(tag);
                continue;
            }
            if (g_levels_seen.insert(tag).second) continue; // подтверждаем вторым опросом
            auto text = levels_text(slot.levels);
            if (auto reported = g_levels_reported.find(tag); reported != g_levels_reported.end() && reported->second == text) continue;
            if (!Net::send_part(tag, wire_of(slot), Coop::CarPartRequest)) continue;
            g_levels_reported[tag] = text;
            Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: топливо/заряд выросли у игрока ({}) → прошу Host\n"), wide(tag), wide(text));
        }

        // Client: краска или оклейка той же детали разошлась с Host-ом — перекрасил игрок
        for (auto& [tag, slot] : slots)
        {
            if (g_wanted.contains(tag) || g_tint_wanted.contains(tag)) continue; // ещё применяем слово Host-а
            auto known = g_known.find(tag);
            auto known_tint = g_known_tint.find(tag);
            if (known == g_known.end() || known_tint == g_known_tint.end() || slot.tint.empty() || archetype_of(slot.path) != archetype_of(known->second)) continue;
            if (slot.tint == known_tint->second)
            {
                g_tint_seen.erase(tag);
                g_tint_reported.erase(tag);
                continue;
            }
            if (!g_client_parts)
            {
                g_tint_wanted[tag] = {archetype_of(slot.path), known_tint->second, false};
                continue;
            }
            auto seen = g_tint_seen.find(tag);
            if (seen == g_tint_seen.end() || seen->second != slot.tint) // при смене детали краска на миг дефолтная — ждём второго опроса
            {
                g_tint_seen[tag] = slot.tint;
                continue;
            }
            if (auto reported = g_tint_reported.find(tag); reported != g_tint_reported.end() && reported->second == slot.tint) continue;
            if (!Net::send_part(tag, wire_of(slot), Coop::CarPartRequest)) continue;
            g_tint_reported[tag] = slot.tint;
            Output::send<LogLevel::Verbose>(STR("[PartsSync] {}: деталь перекрашена игроком → прошу Host\n"), wide(tag));
        }
    }
} // namespace PartsSync
