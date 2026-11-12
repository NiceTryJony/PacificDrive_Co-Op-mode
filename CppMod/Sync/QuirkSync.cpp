#include "QuirkSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Items.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace QuirkSync
{
    namespace
    {
        constexpr uint64_t PollMs = 1000, ResendMs = 10'000, SettleMs = 8000; // после загрузки карты причуды восстанавливаются из сейва

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        uint64_t g_loaded_ms = 0, g_last_poll_ms = 0, g_last_sent_ms = 0;
        std::map<uint32_t, int> g_sent;                // Host: что последним отправлено
        std::optional<std::map<uint32_t, int>> g_host; // Client: набор Host (нет — ещё не пришёл)
        std::map<uint32_t, int> g_mine;                // Client: свой набор на прошлом опросе
        std::map<uint32_t, std::string> g_names;       // ключ → "QC|QB" для лога

        UObject* manager() { return UObjectGlobals::FindFirstOf(STR("BP_StatusEffectManager_C")); }

        std::string class_of(UObject* quirk, const TCHAR* prop)
        {
            auto** part = property<UObject*>(quirk, prop);
            return part && *part ? Items::narrow((*part)->GetClassPrivate()->GetName()) : "?";
        }

        uint32_t fnv1a(const std::string& s)
        {
            uint32_t h = 2166136261u;
            for (unsigned char c : s) h = (h ^ c) * 16777619u;
            return h;
        }

        uint32_t key_of(UObject* quirk)
        {
            auto name = class_of(quirk, STR("QuirkCondition")) + "|" + class_of(quirk, STR("QuirkBehavior"));
            uint32_t key = fnv1a(name);
            g_names[key] = name;
            return key;
        }

        std::vector<UObject*> quirks()
        {
            std::vector<UObject*> out;
            auto* m = manager();
            auto* arr = m ? property<Items::RawArray>(m, STR("CarQuirks")) : nullptr;
            for (int32_t i = 0; arr && arr->Data && i < arr->Num; ++i)
                if (arr->Data[i]) out.push_back(arr->Data[i]);
            return out;
        }

        std::map<uint32_t, int> current()
        {
            std::map<uint32_t, int> out;
            for (auto* q : quirks()) ++out[key_of(q)];
            return out;
        }

        std::wstring name_of(uint32_t key)
        {
            auto it = g_names.find(key);
            return Items::wide(it != g_names.end() ? it->second : std::format("{:08x}", key));
        }

        // Что о причуде выяснено: биты Coop::QuirkKnown* (диагноз; часть/тип условия и поведения)
        struct Flag
        {
            const TCHAR* element; // nullptr — у самой причуды
            const TCHAR* name;
            uint8_t bit;
        };
        constexpr Flag Flags[] = {{nullptr, STR("Diagnosed"), Coop::QuirkDiagnosed},
                                  {STR("QuirkCondition"), STR("PartKnown"), Coop::QuirkConditionPart},
                                  {STR("QuirkCondition"), STR("TypeKnown"), Coop::QuirkConditionType},
                                  {STR("QuirkBehavior"), STR("PartKnown"), Coop::QuirkBehaviorPart},
                                  {STR("QuirkBehavior"), STR("TypeKnown"), Coop::QuirkBehaviorType}};

        bool* flag_of(UObject* quirk, const Flag& f)
        {
            UObject* owner = quirk;
            if (f.element)
            {
                auto** element = property<UObject*>(quirk, f.element);
                owner = element ? *element : nullptr;
            }
            return owner ? property<bool>(owner, f.name) : nullptr;
        }

        uint8_t known_of(UObject* quirk)
        {
            uint8_t bits = 0;
            for (auto& f : Flags)
                if (auto* v = flag_of(quirk, f); v && *v) bits |= f.bit;
            for (uint8_t c = 0; c < Coop::QuirkCategories; ++c)
            {
                UFunctionCall fn(quirk, STR("IsQuirkKnown"));
                auto* category = fn ? fn.param<uint8_t>(STR("Detective Category")) : nullptr;
                auto* full = fn ? fn.param<bool>(STR("FullyKnown")) : nullptr;
                if (!category || !full) break;
                *category = c;
                fn.call();
                if (*full) bits |= static_cast<uint8_t>(Coop::QuirkFullyKnown0 << c);
            }
            return bits;
        }

        // Штатное «решение найдено» в категории (то, что игра делает после диагностики); false — функции нет
        bool fully_learn(UObject* quirk, uint8_t category_index, bool via_diagnosis)
        {
            UFunctionCall fn(quirk, STR("FullyLearnQuirk"));
            auto* category = fn ? fn.param<uint8_t>(STR("Detective Category")) : nullptr;
            auto* via = fn ? fn.param<bool>(STR("ViaDiagnosis")) : nullptr;
            if (!category || !via) return false;
            *category = category_index;
            *via = via_diagnosis;
            fn.call();
            return true;
        }

        // Добавить знание (только поднимаем флаги — забыть найденное нельзя). Возвращает, что реально добавилось
        uint8_t learn(uint32_t key, uint8_t bits)
        {
            uint8_t added = 0;
            for (auto* q : quirks())
            {
                if (key_of(q) != key) continue;
                uint8_t have = known_of(q);
                // Диагноз собеседника = решение найдено целиком: открываем все категории (сам флаг Diagnosed панель не читает)
                uint8_t want = bits;
                if (bits & Coop::QuirkDiagnosed)
                    for (uint8_t c = 0; c < Coop::QuirkCategories; ++c) want |= static_cast<uint8_t>(Coop::QuirkFullyKnown0 << c);
                for (uint8_t c = 0; c < Coop::QuirkCategories; ++c)
                {
                    auto bit = static_cast<uint8_t>(Coop::QuirkFullyKnown0 << c);
                    if ((want & bit) && !(have & bit) && fully_learn(q, c, bits & Coop::QuirkDiagnosed)) added |= bit;
                }
                for (auto& f : Flags)
                    if (auto* v = flag_of(q, f); v && (bits & f.bit) && !*v)
                    {
                        *v = true;
                        added |= f.bit;
                    }
            }
            return added;
        }

        std::map<uint32_t, uint8_t> known_now()
        {
            std::map<uint32_t, uint8_t> out;
            for (auto* q : quirks()) out[key_of(q)] |= known_of(q);
            return out;
        }

        std::map<uint32_t, uint8_t> g_sent_known, g_host_known;

        // Вылечить одну причуду с этим ключом; false — такой нет или функция не сработала
        bool cure(uint32_t key)
        {
            for (auto* q : quirks())
            {
                if (key_of(q) != key) continue;
                // CureCarQuirk — лечение «от игрока»: без расходников в багажнике отказывает, а с ними тратит их второй раз
                // (тесты 2026-11-09 08:09/08:12 и 2026-11-10 07:18: «НЕ вышло» / «готово» + лишний расход Fix*). Собеседник уже
                // заплатил — снимаем сам эффект родным UBaseStatusEffectManager.RemoveStatusEffect
                size_t before = quirks().size();
                UFunctionCall remove(manager(), STR("RemoveStatusEffect"));
                if (auto* effect = remove ? remove.param<UObject*>(STR("StatusEffect")) : nullptr)
                {
                    *effect = q;
                    remove.call();
                }
                if (quirks().size() == before)
                {
                    UFunctionCall clean(manager(), STR("CleanCachedQuirks")); // эффект снят, а в CarQuirks остался указатель
                    if (clean) clean.call();
                }
                if (quirks().size() < before) return true;
                Output::send<LogLevel::Warning>(STR("[Quirk] RemoveStatusEffect причуду не снял — пробую CureCarQuirk\n"));
                UFunctionCall fn(manager(), STR("CureCarQuirk"));
                auto* target = fn ? fn.param<UObject*>(STR("QuirkToCure")) : nullptr;
                auto* removed = fn ? fn.param<bool>(STR("Removed")) : nullptr;
                if (!target) return false;
                *target = q;
                fn.call();
                return !removed || *removed;
            }
            return false;
        }
    } // namespace

    void on_map_loaded()
    {
        g_loaded_ms = now_ms();
        g_sent.clear();
        g_mine.clear();
        g_host.reset();
        g_sent_known.clear();
        g_host_known.clear();
    }

    void on_quirk_set(const Coop::QuirkSetBody& body)
    {
        std::map<uint32_t, int> host;
        g_host_known.clear();
        for (int i = 0; i < body.Count && i < Coop::MaxQuirks; ++i)
        {
            ++host[body.Keys[i]];
            g_host_known[body.Keys[i]] |= body.Known[i];
        }
        g_host = host;
        if (now_ms() - g_loaded_ms < SettleMs) return; // своё ещё грузится — применим на ближайшем опросе
        // Что выяснил Host — видно и мне (панель «исправления»)
        for (auto& [key, bits] : g_host_known)
            if (uint8_t added = learn(key, bits))
                Output::send<LogLevel::Verbose>(STR("[Quirk] Host выяснил о «{}» (биты {}) — показываю у себя\n"), name_of(key), added);
        // Лишние у меня — Host их вылечил: лечу у себя. Новых у Host не создаём (их пару игра разыгрывает случайно)
        for (auto& [key, mine] : current())
        {
            int extra = mine - (host.contains(key) ? host[key] : 0);
            for (int i = 0; i < extra; ++i)
            {
                bool ok = cure(key);
                Output::send<LogLevel::Verbose>(STR("[Quirk] Host вылечил «{}» — лечу у себя: {}\n"), name_of(key), ok ? STR("готово") : STR("НЕ вышло"));
                // Это лечение — не моё действие: вычитаем из своего счёта, Host-у не сообщаем. Весь счёт здесь не обновляем:
                // набор Host приходит каждые 10 с и затирал моё лечение, которого опрос ещё не видел (тест 2026-11-10 07:18:23)
                if (auto it = g_mine.find(key); ok && it != g_mine.end() && it->second > 0) --it->second;
            }
        }
    }

    void on_quirk_cure(const Coop::QuirkBody& body)
    {
        bool ok = cure(body.Key);
        Output::send<LogLevel::Verbose>(STR("[Quirk] пассажир вылечил «{}» — у меня: {}\n"), name_of(body.Key), ok ? STR("вылечено") : STR("такой уже нет"));
        g_last_sent_ms = 0; // разослать новый набор сразу
    }

    void on_quirk_know(const Coop::QuirkKnowBody& body)
    {
        uint8_t added = learn(body.Key, body.Known);
        Output::send<LogLevel::Verbose>(STR("[Quirk] пассажир выяснил о «{}» (биты {}) — добавлено у меня: {}\n"), name_of(body.Key), body.Known, added);
        g_last_sent_ms = 0;
    }

    void tick(bool is_host)
    {
        uint64_t now = now_ms();
        if (!Net::connected() || now - g_loaded_ms < SettleMs || now - g_last_poll_ms < PollMs) return;
        g_last_poll_ms = now;
        if (!manager()) return;
        auto set = current();
        auto known = known_now();
        if (is_host)
        {
            if (set == g_sent && known == g_sent_known && now - g_last_sent_ms < ResendMs) return;
            Coop::QuirkSetBody body{};
            for (auto& [key, n] : set)
                for (int i = 0; i < n && body.Count < Coop::MaxQuirks; ++i)
                {
                    body.Known[body.Count] = known[key];
                    body.Keys[body.Count++] = key;
                }
            Net::send_quirk_set(body);
            if (set != g_sent || known != g_sent_known) Output::send<LogLevel::Verbose>(STR("[Quirk] мои причуды → пассажиру: {}\n"), body.Count);
            g_sent = set;
            g_sent_known = known;
            g_last_sent_ms = now;
            return;
        }
        // Client: выяснил то, чего Host ещё не знает — сообщаю (Host добавит и разошлёт)
        if (g_host)
            for (auto& [key, bits] : known)
                if (uint8_t extra = bits & ~(g_host_known.contains(key) ? g_host_known[key] : 0); extra && g_host->contains(key))
                {
                    Net::send_quirk_know({key, bits});
                    g_host_known[key] |= bits; // не повторять каждую секунду, пока не придёт новый набор
                    Output::send<LogLevel::Verbose>(STR("[Quirk] я выяснил о «{}» (биты {}) → сообщаю Host\n"), name_of(key), extra);
                }
        // Client: какой причуды у меня стало меньше, а у Host она ещё есть — это я вылечил
        if (g_host && !g_mine.empty())
            for (auto& [key, was] : g_mine)
            {
                int left = set.contains(key) ? set[key] : 0;
                int host = g_host->contains(key) ? (*g_host)[key] : 0;
                for (int i = 0; i < (std::min)(was - left, host - left); ++i)
                {
                    Net::send_quirk_cure({key});
                    Output::send<LogLevel::Verbose>(STR("[Quirk] я вылечил «{}» → прошу Host\n"), name_of(key));
                }
            }
        g_mine = set;
    }
} // namespace QuirkSync
