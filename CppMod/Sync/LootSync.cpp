#include "LootSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Предмет мира = UItemInstance с проявлением AItemWorldManifestation. Подобрали = слот экземпляра (GetOuterSlot) стал другим,
// чем при первом появлении (в мире он может быть и пустым, и «мировым» — 0.7.0 ждал пустой и не учёл ничего), уничтожили =
// слабый указатель пуст. Игра показывает предметы только рядом с игроком, поэтому учёт
// пополняется по мере появления, а снятое собеседником помнится списком и снимается, когда появится и у нас.
// shortcut: только предметы, лежащие в мире; содержимое контейнеров (багажники, ящики) — следующим шагом.
namespace LootSync
{
    std::atomic<bool> g_simulate_peer_pick{false};

    namespace
    {
        struct Vec
        {
            float X, Y, Z;
        };
        struct Tracked
        {
            FWeakObjectPtr Instance;
            uint32_t Archetype;
            Vec Position;         // при первом появлении — у собеседника в этот момент то же место
            UObject* InitialSlot; // слот при первом появлении — только для сравнения, не разыменовывается
            bool Done;            // подобран/убран — больше не следим
            // Актёр в мире, чья гибель = «забрали»: деталь донора или ресурс, распиленные пилой (тест 2026-11-10: актёр пропал,
            // а экземпляр жив и слот тот же — собеседник ничего не узнал). У предметов на земле пусто: их проявление игра выгружает сама
            FWeakObjectPtr Actor{};
            bool HasActor = false, HasInstance = true; // ресурс под распил (BP_Spawn_ResourceBase_C) — без экземпляра, только актёр
        };

        constexpr uint64_t ScanIntervalMs = 4000, PollIntervalMs = 500, AfterLoadMs = 5000;
        constexpr float OriginEpsilon = 100;         // см: ближе к (0,0,0) — не объект мира
        constexpr float DestroyedPickRadius = 1500;  // см: уничтожение дальше от игрока — не подбор

        std::atomic<bool> g_map_loaded{false};
        std::vector<Tracked> g_items;
        std::vector<Coop::LootBody> g_removed_by_peer; // снять, как только появится у нас
        uint64_t g_map_loaded_ms = 0, g_last_scan_ms = 0, g_last_poll_ms = 0;
        std::unordered_set<UObject*> g_seen; // экземпляры, уже рассмотренные на этой карте

        struct RawArray // TArray<UObject*>
        {
            UObject** Data;
            int32_t Num;
            int32_t Max;
        };

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        uint32_t fnv1a(const StringType& s)
        {
            uint32_t h = 2166136261u;
            for (wchar_t c : s)
            {
                h ^= static_cast<uint32_t>(c);
                h *= 16777619u;
            }
            return h;
        }

        bool matches(const Coop::LootBody& loot, uint32_t archetype, const Vec& p)
        {
            float dx = loot.X - p.X, dy = loot.Y - p.Y, dz = loot.Z - p.Z;
            return loot.Archetype == archetype && dx * dx + dy * dy + dz * dz <= Coop::LootMatchRadius * Coop::LootMatchRadius;
        }

        // Слот экземпляра; nullopt — функции нет (тогда подбор не отследить)
        std::optional<UObject*> outer_slot(UObject* instance)
        {
            UFunctionCall fn(instance, STR("GetOuterSlot"));
            auto* ret = fn ? fn.param<UObject*>(STR("ReturnValue")) : nullptr;
            if (!ret) return std::nullopt;
            fn.call();
            return *ret;
        }

        void remove_instance(UObject* instance)
        {
            UFunctionCall fn(instance, STR("Remove"));
            if (auto* p = fn ? fn.param<bool>(STR("bRemoveActors")) : nullptr)
            {
                *p = true; // вместе с проявлением в мире
                fn.call();
            }
        }

        // Убрать у себя учтённое: экземпляр (с проявлением) и актёр, если тот остался (распиленная деталь, ресурс)
        void remove_tracked(Tracked& item)
        {
            if (UObject* instance = item.HasInstance ? item.Instance.Get() : nullptr) remove_instance(instance);
            if (UObject* actor = item.HasActor ? item.Actor.Get() : nullptr)
            {
                UFunctionCall destroy(actor, STR("K2_DestroyActor"));
                if (destroy) destroy.call();
            }
        }

        StringType archetype_name(UObject* instance)
        {
            auto** archetype = property<UObject*>(instance, STR("Archetype"));
            return archetype && *archetype ? (*archetype)->GetName() : StringType{};
        }

        // Ищет учтённый предмет и помечает снятым; nullptr — такого в учёте нет. Одинаковые предметы одного контейнера
        // (3 скотча в багажнике) делят ключ архетип + якорь — берём ещё не снятый, иначе второй скотч получал отказ (тест 2026-11-17 21:23)
        Tracked* take_tracked(const Coop::LootBody& loot, bool* was_done = nullptr)
        {
            Tracked* done = nullptr;
            for (auto& item : g_items)
            {
                if (!matches(loot, item.Archetype, item.Position)) continue;
                if (item.Done)
                {
                    if (!done) done = &item;
                    continue;
                }
                if (was_done) *was_done = false;
                item.Done = true;
                return &item;
            }
            if (done && was_done) *was_done = true;
            return done;
        }

        Vec actor_location(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {};
            fn.call();
            return *ret;
        }

        float distance_to_player(const Vec& p)
        {
            Vec me = actor_location(Cached::character());
            float dx = p.X - me.X, dy = p.Y - me.Y, dz = p.Z - me.Z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // Взять экземпляр в учёт. anchor — где он у собеседника тоже окажется: сам предмет, деталь машины или ящик
        bool track(Tracked item)
        {
            // Собеседник уже забрал этот предмет, пока он был далеко от нас
            for (auto it = g_removed_by_peer.begin(); it != g_removed_by_peer.end(); ++it)
            {
                if (!matches(*it, item.Archetype, item.Position)) continue;
                remove_tracked(item);
                g_removed_by_peer.erase(it);
                item.Done = true;
                break;
            }
            g_items.push_back(item);
            return true;
        }

        // actor — актёр, гибель которого тоже значит «забрали» (деталь донора); у остального nullptr
        bool consider(UObject* instance, const Vec& anchor, UObject* actor = nullptr)
        {
            if (!instance || !g_seen.insert(instance).second) return false;
            // В начале координат — служебные объекты не из мира (шаблоны машин в гараже, тест 2026-10-27)
            if (std::fabs(anchor.X) + std::fabs(anchor.Y) + std::fabs(anchor.Z) < OriginEpsilon) return false;
            auto slot = outer_slot(instance);
            if (!slot) return false; // GetOuterSlot нет — подбор не отследить
            auto name = archetype_name(instance);
            if (name.starts_with(STR("IA_Hazard_Leech"))) return false; // пиявки ползают и их ведёт LeechSync — не лут
            Tracked item{FWeakObjectPtr(instance), fnv1a(name), anchor, *slot, false};
            if (actor) item.Actor = FWeakObjectPtr(actor), item.HasActor = true;
            return track(item);
        }

        // Ресурс под распил пилой (свинцовая плита, компьютер, радио…): экземпляра нет, опознаём по классу и месту
        bool consider_resource(UObject* actor)
        {
            if (!actor || !g_seen.insert(actor).second) return false;
            Vec at = actor_location(actor);
            if (std::fabs(at.X) + std::fabs(at.Y) + std::fabs(at.Z) < OriginEpsilon) return false;
            Tracked item{FWeakObjectPtr(), fnv1a(actor->GetClassPrivate()->GetName()), at, nullptr, false};
            item.Actor = FWeakObjectPtr(actor), item.HasActor = true, item.HasInstance = false;
            return track(item);
        }

        void scan(bool is_host)
        {
            int ground = 0, parts = 0, in_crates = 0, resources = 0;
            std::vector<UObject*> actors;
            // 1. Предметы на земле (у них bWorldOwned = 0 и слота нет — тест 2026-10-27)
            UObjectGlobals::FindAllOf(STR("ItemWorldManifestation"), actors);
            for (UObject* actor : actors)
            {
                auto** instance = property<UObject*>(actor, STR("Instance"));
                if (instance && *instance && !g_seen.contains(*instance)) ground += consider(*instance, actor_location(actor));
            }
            // 2. Детали машин-доноров (двери, колёса, капоты…): разбор = экземпляр в инвентарь или уничтожен
            actors.clear();
            UObjectGlobals::FindAllOf(STR("LootableTemplatePart"), actors);
            for (UObject* actor : actors)
            {
                auto** instance = property<UObject*>(actor, STR("ItemInstance"));
                if (instance && *instance && !g_seen.contains(*instance)) parts += consider(*instance, actor_location(actor), actor);
            }
            // 2а. Ресурсы под распил (тест 2026-11-10 07:33: Host распилил свинцовую плиту — у Client осталась)
            actors.clear();
            UObjectGlobals::FindAllOf(STR("BP_Spawn_ResourceBase_C"), actors);
            for (UObject* actor : actors)
                if (!g_seen.contains(actor)) resources += consider_resource(actor);
            // 3. Содержимое ящиков/шкафчиков/рюкзаков: экземпляры в слотах InventoryContainer, якорь — сам ящик
            actors.clear();
            UObjectGlobals::FindAllOf(STR("BP_Lootable_Base_C"), actors);
            for (UObject* crate : actors)
            {
                // Флаг «предметы при открытии уже разыграны» сейв не сохраняет (тест 2026-10-27): у Client ставим сами,
                // иначе при открытии он доразыграет своё. Содержимое, разыгранное Host заранее, уже пришло в сейве
                if (!is_host)
                    if (auto* rolled = property<bool>(crate, STR("RolledOnOpenItems"))) *rolled = true;
                auto** container = property<UObject*>(crate, STR("InventoryContainer"));
                auto* slots = container && *container ? property<RawArray>(*container, STR("Slots")) : nullptr;
                if (!slots || !slots->Data) continue;
                Vec anchor{};
                bool anchored = false;
                for (int32_t s = 0; s < slots->Num; ++s)
                {
                    auto* items = slots->Data[s] ? property<RawArray>(slots->Data[s], STR("Items")) : nullptr;
                    for (int32_t i = 0; items && items->Data && i < items->Num; ++i)
                    {
                        if (!items->Data[i] || g_seen.contains(items->Data[i])) continue;
                        if (!anchored) anchor = actor_location(crate), anchored = true;
                        in_crates += consider(items->Data[i], anchor);
                    }
                }
            }
            // 4. Содержимое багажников машин-доноров: ItemContainer с Outer = ItemInstance актёра BP_TemplatePart_TrunkContents_C
            //    (RECON.md 6.27; тест 2026-11-06: Client взял 5 и 8 предметов, у Host не убавилось). Якорь — сам багажник.
            //    Кузова грузовиков (BP_TemplatePart_TruckBedContainer_C, тест 2026-11-17 21:22) устроены так же
            int in_trunks = 0;
            actors.clear();
            UObjectGlobals::FindAllOf(STR("BP_TemplatePart_TrunkContents_C"), actors);
            UObjectGlobals::FindAllOf(STR("BP_TemplatePart_TruckBedContainer_C"), actors);
            if (!actors.empty())
            {
                std::vector<UObject*> containers;
                UObjectGlobals::FindAllOf(STR("ItemContainer"), containers);
                std::unordered_multimap<UObject*, UObject*> by_outer; // Outer → контейнер: вместо перебора «багажники × контейнеры»
                for (UObject* container : containers) by_outer.emplace(container->GetOuterPrivate(), container);
                for (UObject* trunk : actors)
                {
                    auto** owner = property<UObject*>(trunk, STR("ItemInstance"));
                    if (!owner || !*owner) continue;
                    auto range = by_outer.equal_range(*owner);
                    for (auto it = range.first; it != range.second; ++it)
                    {
                        UObject* container = it->second;
                        auto* slots = property<RawArray>(container, STR("Slots"));
                        Vec anchor{};
                        bool anchored = false;
                        for (int32_t s = 0; slots && slots->Data && s < slots->Num; ++s)
                        {
                            auto* items = slots->Data[s] ? property<RawArray>(slots->Data[s], STR("Items")) : nullptr;
                            for (int32_t i = 0; items && items->Data && i < items->Num; ++i)
                            {
                                if (!items->Data[i] || g_seen.contains(items->Data[i])) continue;
                                if (!anchored) anchor = actor_location(trunk), anchored = true;
                                in_trunks += consider(items->Data[i], anchor);
                            }
                        }
                    }
                }
            }
            if (ground + parts + in_crates + in_trunks + resources)
                Output::send<LogLevel::Verbose>(STR("[Loot] в учёт: +{} на земле, +{} деталей машин, +{} в ящиках, +{} в багажниках доноров, +{} ресурсов под распил; всего {}\n"),
                                                ground, parts, in_crates, in_trunks, resources, g_items.size());
        }

        void poll(bool is_host)
        {
            for (auto& item : g_items)
            {
                if (item.Done) continue;
                UObject* instance = item.HasInstance ? item.Instance.Get() : nullptr;
                bool picked = false; // слот сменился — предмет в чьём-то инвентаре
                if (instance)
                {
                    auto slot = outer_slot(instance);
                    picked = slot && *slot != item.InitialSlot;
                }
                bool sawn = item.HasActor && !item.Actor.Get(); // актёр детали/ресурса пропал — распилили или разобрали
                if (!picked && !sawn && (instance || !item.HasInstance)) continue;
                item.Done = true;
                // Уничтожен вдали от игрока — это игра выгрузила/почистила, а не подбор: собеседнику не сообщаем
                if (!picked && distance_to_player(item.Position) > DestroyedPickRadius) continue;
                Coop::LootBody loot{item.Archetype, item.Position.X, item.Position.Y, item.Position.Z};
                Net::send_loot(is_host ? Coop::LootPicked : Coop::InventoryAction, loot);
                Output::send<LogLevel::Verbose>(STR("[Loot] {}: {} у ({:.0f}, {:.0f}, {:.0f})\n"), is_host ? STR("забрал Host") : STR("забрал я, сообщаю Host"),
                                                picked ? archetype_name(instance) : sawn ? STR("<распилен/разобран>") : STR("<уничтожен>"), loot.X, loot.Y, loot.Z);
            }
        }

        // Убрать у себя предмет, который забрал собеседник (в мире он или уже у нас в инвентаре — неважно: прав Host)
        void remove_here(const Coop::LootBody& loot, const wchar_t* why)
        {
            if (Tracked* item = take_tracked(loot))
            {
                remove_tracked(*item);
                Output::send<LogLevel::Verbose>(STR("[Loot] убран предмет ({}) у ({:.0f}, {:.0f}, {:.0f})\n"), why, loot.X, loot.Y, loot.Z);
            }
            else
            {
                g_removed_by_peer.push_back(loot); // у нас ещё не показан — снимем при появлении
            }
        }
    } // namespace

    void on_loot(Coop::PacketId id, const Coop::LootBody& loot, bool is_host)
    {
        if (is_host && id == Coop::InventoryAction)
        {
            // Спека 3.4.2: Host проверяет, что предмет ещё не забран. Забран им самим — отказ, Client уберёт у себя
            bool host_took = false;
            Tracked* item = take_tracked(loot, &host_took);
            if (host_took)
            {
                Net::send_loot(Coop::LootDenied, loot);
                Output::send<LogLevel::Verbose>(STR("[Loot] пассажир взял то, что уже у меня — отказ\n"));
                return;
            }
            if (item) remove_tracked(*item);
            else g_removed_by_peer.push_back(loot);
            Output::send<LogLevel::Verbose>(STR("[Loot] пассажир забрал предмет у ({:.0f}, {:.0f}, {:.0f}) — убран у меня\n"), loot.X, loot.Y, loot.Z);
            return;
        }
        if (!is_host && id == Coop::LootPicked) remove_here(loot, L"забрал Host");
        if (!is_host && id == Coop::LootDenied)
        {
            // Наш экземпляр уже у нас в инвентаре — учёт помечен снятым, но слабый указатель жив
            for (auto& item : g_items)
            {
                if (!item.Done || !matches(loot, item.Archetype, item.Position)) continue; // не снятый — ещё лежит в контейнере, не наш
                if (UObject* instance = item.Instance.Get()) remove_instance(instance);
                Output::send<LogLevel::Verbose>(STR("[Loot] этот предмет уже забрал Host — убран из моего инвентаря\n"));
                break;
            }
        }
    }

    void on_map_loaded() { g_map_loaded = true; }

    void preroll_containers()
    {
        std::vector<UObject*> crates;
        UObjectGlobals::FindAllOf(STR("BP_Lootable_Base_C"), crates);
        int rolled = 0, already = 0;
        for (UObject* crate : crates)
        {
            auto* done = property<bool>(crate, STR("RolledOnOpenItems"));
            if (!done) continue;
            if (*done)
            {
                ++already;
                continue;
            }
            UFunctionCall roll(crate, STR("OnRollForOpenItems"));
            if (!roll) continue;
            roll.call();
            ++rolled;
        }
        Output::send<LogLevel::Verbose>(STR("[Loot] ящиков {}: разыграл «при открытии» заранее {}, уже были разыграны {}\n"), crates.size(), rolled, already);
        // Розыгрыш перекладывает содержимое ящиков по слотам — учёт по старым слотам дал бы ложные «забрал» (тест 2026-10-27)
        if (rolled)
        {
            g_items.clear();
            g_seen.clear();
        }
    }

    namespace
    {
        void simulate_peer_pick()
        {
            UObject* character = Cached::character();
            UFunctionCall get_loc(character, STR("K2_GetActorLocation"));
            auto* me = get_loc ? get_loc.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!me) return;
            get_loc.call();
            const Tracked* nearest = nullptr;
            float best = 500 * 500; // 5 м
            for (const auto& item : g_items)
            {
                float dx = item.Position.X - me->X, dy = item.Position.Y - me->Y, dz = item.Position.Z - me->Z;
                if (!item.Done && dx * dx + dy * dy + dz * dz < best) best = dx * dx + dy * dy + dz * dz, nearest = &item;
            }
            if (!nearest)
            {
                Output::send<LogLevel::Verbose>(STR("[Loot] Ctrl+F12: рядом (5 м) нет учтённых предметов\n"));
                return;
            }
            Output::send<LogLevel::Verbose>(STR("[Loot] Ctrl+F12: имитирую — собеседник забрал ближайший предмет\n"));
            remove_here({nearest->Archetype, nearest->Position.X, nearest->Position.Y, nearest->Position.Z}, L"имитация Ctrl+F12");
        }
    } // namespace

    void tick(bool is_host)
    {
        uint64_t now = now_ms();
        if (g_map_loaded.exchange(false))
        {
            g_items.clear(); // старые экземпляры ушли с миром
            g_seen.clear();
            g_removed_by_peer.clear();
            g_map_loaded_ms = now;
        }
        // До связи учёт не ведём: подобранное в одиночку собеседнику не важно (он загрузит наш сейв)
        if (!Net::connected() || now - g_map_loaded_ms < AfterLoadMs) return;
        if (!Cached::character()) return; // меню/загрузка
        if (now - g_last_scan_ms >= ScanIntervalMs)
        {
            g_last_scan_ms = now;
            scan(is_host);
        }
        if (g_simulate_peer_pick.exchange(false)) simulate_peer_pick();
        if (now - g_last_poll_ms >= PollIntervalMs)
        {
            g_last_poll_ms = now;
            poll(is_host);
        }
    }
} // namespace LootSync
