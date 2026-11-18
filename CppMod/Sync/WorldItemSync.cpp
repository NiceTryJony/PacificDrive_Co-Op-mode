#include "WorldItemSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunction.hpp>

#include "Game/ItemProps.h"
#include "Game/Items.h"
#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_set>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace WorldItemSync
{
    namespace
    {
        constexpr int64_t SettleMs = 15'000; // после загрузки карты подгрузка уровня создаёт предметы сама
        constexpr int64_t DelayMs = 300;     // дать проявлению дособраться, потом читать свойства
        constexpr float NearPlayerCm = 450;  // положил игрок — предмет возникает рядом с ним
        constexpr size_t MaxPending = 32;
        // Выброшенный предмет летит и падает по физике (лог 2026-11-07: брошен на высоте 186, поднят на 32 в 80 см от точки
        // броска). Создание по сети физику не запускает — копия висела бы в точке броска, а подбор (LootPicked — место, где
        // лежит) не совпадал бы с ней. Поэтому шлём не точку броска, а место, где предмет остановился
        constexpr int64_t SampleMs = 100;     // как часто смотрим, движется ли
        constexpr float StillCm = 1.0f;       // сдвиг за SampleMs меньше — стоит
        constexpr int64_t StillForMs = 500;   // стоит столько — лёг
        constexpr int64_t GiveUpMs = 8'000;   // катится дольше — шлём где есть

        struct Pending
        {
            FWeakObjectPtr Instance;
            float Transform[12]; // точка появления (по ней — «рядом с игроком»)
            int64_t Ms;
            bool Near = false;   // проверка «из рук игрока» пройдена, дальше ждём, пока ляжет
            float Last[3]{};
            int64_t SampleAt = 0, StillSince = 0;
        };

        struct Check
        {
            FWeakObjectPtr Instance;
            int64_t Ms;
        };
        std::vector<Check> g_checks; // созданное по сети: через 2 с смотрим, где предмет

        std::vector<Pending> g_pending;
        std::unordered_set<UObject*> g_sent; // экземпляры, уже отправленные на этой карте (хук иногда срабатывает дважды)
        bool g_hooked = false, g_applying = false;
        int64_t g_loaded_ms = 0;
        size_t g_off_transform = SIZE_MAX, g_off_instance = SIZE_MAX, g_off_context = SIZE_MAX;

        struct Vec
        {
            float X, Y, Z;
        };
        struct alignas(16) Transform
        {
            float Data[12];
        };

        int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        UObject* character() { return Cached::character(); }

        Vec actor_location(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {NAN, NAN, NAN};
            fn.call();
            return *ret;
        }

        // Трансформ актёра в мире (AActor::GetTransform, в Blueprint — GetActorTransform); false — не вышло
        bool actor_transform(UObject* actor, float out[12])
        {
            UFunctionCall fn(actor, STR("GetTransform"));
            auto* ret = fn ? fn.param<Transform>(STR("ReturnValue")) : nullptr;
            if (!ret) return false;
            fn.call();
            std::memcpy(out, ret->Data, sizeof(ret->Data));
            return true;
        }

        // Проявление предмета в мире (UItemInstance.GetActorManifestation); nullptr — актёра нет
        UObject* manifestation_of(UObject* instance)
        {
            UFunctionCall fn(instance, STR("GetActorManifestation"));
            auto* ret = fn ? fn.param<UObject*>(STR("ReturnValue")) : nullptr;
            if (!ret) return nullptr;
            fn.call();
            return *ret;
        }

        // Живой InventorySystem игры (UDrivingGameInstance.InventorySystem). Не CDO: SpawnItemInstanceActor выбирает класс
        // проявления по списку правил самого объекта (this+0x60, не рефлектируется) — у CDO он пуст, и актёр не создаётся
        // (exe: exec-thunk 0x1411D3C60 → 0x1411BA8A0 → SelectActorManifestationClass 0x1411B3820, разбор 2026-11-07)
        UObject* inventory_system()
        {
            std::vector<UObject*> instances; // среди найденных может быть CDO с пустым полем — берём заполненное
            UObjectGlobals::FindAllOf(STR("DrivingGameInstance"), instances);
            for (UObject* gi : instances)
                if (auto** sys = property<UObject*>(gi, STR("InventorySystem")); sys && *sys) return *sys;
            return nullptr;
        }

        // Класс-переопределение проявления у архетипа (WorldManifestationOverride) — мягкая ссылка: игра берёт его только
        // уже загруженным (TSoftObjectPtr::Get). У отправителя он загружен (предмет был в руках), у получателя может не быть —
        // грузим сами штатным LoadClassAsset_Blocking. Пустая ссылка — ничего не делает
        void preload_world_class(UObject* instance)
        {
            auto** archetype = property<UObject*>(instance, STR("Archetype"));
            auto* soft = archetype && *archetype ? property<uint8_t>(*archetype, STR("WorldManifestationOverride")) : nullptr;
            static auto* lib = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
            if (!soft || !lib) return;
            UFunctionCall load(lib, STR("LoadClassAsset_Blocking"));
            auto* p_soft = load ? load.param<uint8_t>(STR("AssetClass")) : nullptr;
            if (!p_soft) return;
            std::memcpy(p_soft, soft, 0x28); // TSoftClassPtr = FSoftObjectPtr (0x28); буфер параметров не разрушается — копия без владения безопасна
            load.call();
        }

        // Значения свойств предмета в сетевой вид (как в TrunkSync); своё же нечитаемое значение пропускаем
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
                catch (const std::exception&)
                {
                }
            }
            return out;
        }

        std::string text_of(const Coop::TrunkValue& v)
        {
            if (v.Spec >= std::size(ItemProps::Specs)) return {};
            if (ItemProps::Specs[v.Spec].Type == 'i') return std::to_string(v.Bits);
            float f;
            std::memcpy(&f, &v.Bits, sizeof(f));
            return std::format("{}", f);
        }

        void install_hook()
        {
            if (g_hooked) return;
            auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/PenDriverInventory.InventorySystem:SpawnItemInstanceActor"));
            if (!fn) return; // модуль инвентаря ещё не загружен — попробуем на следующем тике
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (p->GetName() == STR("Transform")) g_off_transform = static_cast<size_t>(p->GetOffset_ForInternal());
                else if (p->GetName() == STR("Instance")) g_off_instance = static_cast<size_t>(p->GetOffset_ForInternal());
                else if (p->GetName() == STR("ActorContext")) g_off_context = static_cast<size_t>(p->GetOffset_ForInternal());
            }
            g_hooked = true; // дальше не повторяем при любом исходе
            if (g_off_transform == SIZE_MAX || g_off_instance == SIZE_MAX || g_off_context == SIZE_MAX)
            {
                Output::send<LogLevel::Error>(STR("[WorldItem] у SpawnItemInstanceActor нет ожидаемых параметров — предметы на земле не синхронизируются\n"));
                return;
            }
            UObjectGlobals::RegisterHook(
                    STR("/Script/PenDriverInventory.InventorySystem:SpawnItemInstanceActor"),
                    [](UnrealScriptFunctionCallableContext& context, void*) {
                        auto* locals = static_cast<uint8_t*>(context.TheStack.Locals());
                        if (!locals || g_applying || !Net::connected() || now_ms() - g_loaded_ms < SettleMs || g_pending.size() >= MaxPending) return;
                        if (*(locals + g_off_context) != 0) return; // только EItemActorContext::World
                        UObject* instance = *reinterpret_cast<UObject**>(locals + g_off_instance);
                        if (!instance || g_sent.count(instance)) return;
                        Pending p{FWeakObjectPtr(instance), {}, now_ms()};
                        std::memcpy(p.Transform, locals + g_off_transform, sizeof(p.Transform));
                        g_pending.push_back(p);
                    },
                    [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
            Output::send<LogLevel::Verbose>(STR("[WorldItem] перехват появления предметов в мире включён\n"));
        }
    } // namespace

    void on_map_loaded()
    {
        g_loaded_ms = now_ms();
        g_pending.clear();
        g_sent.clear();
    }

    void tick(bool)
    {
        if (!g_hooked) install_hook();
        for (size_t i = 0; i < g_checks.size();) // проверка созданного по сети предмета: актёр жив, где он, упал ли
        {
            if (now_ms() - g_checks[i].Ms < 2000)
            {
                ++i;
                continue;
            }
            UObject* instance = g_checks[i].Instance.Get();
            g_checks.erase(g_checks.begin() + static_cast<std::ptrdiff_t>(i));
            UObject* actor = instance ? manifestation_of(instance) : nullptr;
            if (!actor)
            {
                Output::send<LogLevel::Warning>(STR("[WorldItem] через 2 с проявления нет (предмет {})\n"), instance ? STR("жив") : STR("уничтожен"));
                continue;
            }
            Vec at = actor_location(actor);
            Output::send<LogLevel::Verbose>(STR("[WorldItem] через 2 с: {} в ({:.0f}, {:.0f}, {:.0f})\n"), actor->GetClassPrivate()->GetName(), at.X, at.Y, at.Z);
        }
        if (g_pending.empty()) return;
        int64_t now = now_ms();
        UObject* player = character();
        for (size_t i = 0; i < g_pending.size();)
        {
            Pending& p = g_pending[i];
            if (now - p.Ms < DelayMs || now - p.SampleAt < SampleMs)
            {
                ++i;
                continue;
            }
            p.SampleAt = now;
            UObject* instance = p.Instance.Get();
            UObject* actor = instance ? manifestation_of(instance) : nullptr;
            bool drop = !instance || !player || !Net::connected() || !ItemProps::available();
            if (!drop && !p.Near)
            {
                Vec me = actor_location(player);
                float dx = p.Transform[4] - me.X, dy = p.Transform[5] - me.Y, dz = p.Transform[6] - me.Z; // FTransform: позиция [4..6]
                drop = std::sqrt(dx * dx + dy * dy + dz * dz) > NearPlayerCm; // не из рук игрока — подгрузка уровня
                p.Near = !drop;
            }
            if (!drop && !actor) drop = now - p.Ms > GiveUpMs; // подняли раньше, чем лёг, — собеседнику и не нужен
            if (drop)
            {
                g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (!actor)
            {
                ++i;
                continue;
            }
            Vec at = actor_location(actor);
            float mx = at.X - p.Last[0], my = at.Y - p.Last[1], mz = at.Z - p.Last[2];
            if (std::sqrt(mx * mx + my * my + mz * mz) >= StillCm) p.StillSince = now;
            p.Last[0] = at.X, p.Last[1] = at.Y, p.Last[2] = at.Z;
            if (now - p.StillSince < StillForMs && now - p.Ms < GiveUpMs)
            {
                ++i;
                continue;
            }
            Coop::WorldItemBody body{};
            if (!actor_transform(actor, body.Transform)) std::memcpy(body.Transform, p.Transform, sizeof(body.Transform));
            body.CreateContext = Items::create_context(instance);
            std::string path = Items::archetype_path(instance);
            g_sent.insert(instance);
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            if (path.find("IA_Hazard_Leech") != std::string::npos) continue; // пиявки ведёт LeechSync (рождаются тем же SpawnItemInstanceActor)
            if (!path.empty() && Net::send_world_item(body, path, values_of(instance)))
                Output::send<LogLevel::Verbose>(STR("[WorldItem] отправлен: {} — лёг в ({:.0f}, {:.0f}, {:.0f})\n"), Items::wide(path), body.Transform[4], body.Transform[5],
                                                body.Transform[6]);
        }
    }

    void* spawn_in_world(const std::string& path, const float transform[12], uint8_t create_context)
    {
        UObject* player = character();
        UObject* inventory = inventory_system();
        if (!player || !inventory)
        {
            Output::send<LogLevel::Warning>(STR("[WorldItem] {}: нет игрока/InventorySystem — не создан\n"), Items::wide(path));
            return nullptr;
        }
        g_applying = true; // создание от сети хук не пересылает обратно
        UObject* instance = Items::create(player, path, create_context);
        if (instance)
        {
            preload_world_class(instance);
            UFunctionCall spawn(inventory, STR("SpawnItemInstanceActor"));
            auto* p_world = spawn ? spawn.param<UObject*>(STR("WorldContextObject")) : nullptr;
            auto* p_tf = spawn ? spawn.param<Transform>(STR("Transform")) : nullptr;
            auto* p_instance = spawn ? spawn.param<UObject*>(STR("Instance")) : nullptr;
            auto* p_context = spawn ? spawn.param<uint8_t>(STR("ActorContext")) : nullptr;
            if (p_world && p_tf && p_instance && p_context)
            {
                *p_world = player;
                std::memcpy(p_tf->Data, transform, sizeof(p_tf->Data));
                *p_instance = instance;
                *p_context = 0; // World
                spawn.call();
                g_sent.insert(instance);
            }
            else
            {
                Output::send<LogLevel::Error>(STR("[WorldItem] нет параметров SpawnItemInstanceActor\n"));
                instance = nullptr;
            }
        }
        else Output::send<LogLevel::Warning>(STR("[WorldItem] не создать {} (архетип не загружен)\n"), Items::wide(path));
        g_applying = false;
        return instance;
    }

    void on_world_item(const Coop::WorldItemBody& body, const std::string& path, const std::vector<Coop::TrunkValue>& values)
    {
        auto* instance = static_cast<UObject*>(spawn_in_world(path, body.Transform, body.CreateContext));
        if (!instance) return;
        // Есть ли у экземпляра проявление (актёр) в мире? Без него предмета не видно (жалоба пользователя 2026-11-07)
        UObject* actor = manifestation_of(instance);
        g_checks.push_back({FWeakObjectPtr(instance), now_ms()});
        Output::send<LogLevel::Verbose>(STR("[WorldItem] проявление: {}\n"), actor ? actor->GetFullName() : StringType(STR("НЕТ")));
        for (const auto& v : values)
        {
            auto text = text_of(v);
            if (text.empty() || !ItemProps::write(instance, ItemProps::Specs[v.Spec], text))
                Output::send<LogLevel::Warning>(STR("[WorldItem] {}: значение свойства {} не записалось\n"), Items::wide(path), v.Spec);
        }
        Output::send<LogLevel::Verbose>(STR("[WorldItem] создан: {} в ({:.0f}, {:.0f}, {:.0f})\n"), Items::wide(path), body.Transform[4], body.Transform[5], body.Transform[6]);
    }
} // namespace WorldItemSync
