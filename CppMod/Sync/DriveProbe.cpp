#include "DriveProbe.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunction.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/PassengerSync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace DriveProbe
{
    namespace
    {
        constexpr uint64_t RemoteTimeoutMs = 250; // Client молчит дольше — ввод Client = нули (DECISIONS 2026-11-13)

        std::atomic<float> g_value[2]; // что подставляем: 0 — газ (AccelerateBrake), 1 — руль (SteerLeftRight)
        std::atomic<int> g_calls[2];
        bool g_hooked = false;
        uint64_t g_log_ms = 0;

        // Host: последний ввод Client
        float g_remote[2] = {0, 0};
        uint64_t g_remote_ms = 0;
        bool g_steer_sent = false; // последний наш вызов SteerLeftRight был не нулевой — нужен один нулевой, иначе руль «залипнет» (событие руля пешком не приходит)

        // Client
        uint64_t g_send_ms = 0;
        bool g_sent_nonzero = false;
        uint64_t g_client_log_ms = 0;

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        void hook_axis(UObject* car, const TCHAR* fn_name, int idx)
        {
            UFunction* fn = car->GetFunctionByNameInChain(fn_name);
            if (!fn) { Output::send<LogLevel::Warning>(STR("[DriveProbe] нет функции {}\n"), fn_name); return; }
            // у входа оси один параметр float (в блюпринте «Axis Value»); имя не важно — берём первый FFloatProperty
            static size_t offsets[2];
            bool found = false;
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (p->GetClass().GetFName() != FName(STR("FloatProperty"), FNAME_Add)) continue;
                offsets[idx] = static_cast<size_t>(p->GetOffset_ForInternal()), found = true;
                break;
            }
            if (!found) { Output::send<LogLevel::Warning>(STR("[DriveProbe] у {} нет float-параметра\n"), fn_name); return; }
            // индекс в замыкание не нужен: два отдельных лямбда-хука
            auto path = car->GetClassPrivate()->GetPathName() + STR(":") + fn_name;
            if (idx == 0)
                UObjectGlobals::RegisterHook(path, [](UnrealScriptFunctionCallableContext& ctx, void*) {
                    ++g_calls[0];
                    float* axis = reinterpret_cast<float*>(static_cast<uint8_t*>(ctx.TheStack.Locals()) + offsets[0]);
                    float v = g_value[0];
                    if (v != 0.f) *axis = v;
                }, [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
            else
                UObjectGlobals::RegisterHook(path, [](UnrealScriptFunctionCallableContext& ctx, void*) {
                    ++g_calls[1];
                    float* axis = reinterpret_cast<float*>(static_cast<uint8_t*>(ctx.TheStack.Locals()) + offsets[1]);
                    float v = g_value[1];
                    if (v != 0.f) *axis = v;
                }, [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
            Output::send<LogLevel::Verbose>(STR("[DriveProbe] перехват {} установлен (смещение параметра {})\n"), fn_name, offsets[idx]);
        }

        UObject* find_car()
        {
            UObject* car = Cached::car(); // BP_PlayerCarNew_C : ANewVehiclePawn, как в CarSync
            if (!car) return nullptr;
            if (!g_hooked) g_hooked = true, hook_axis(car, STR("AccelerateBrake"), 0), hook_axis(car, STR("SteerLeftRight"), 1);
            return car;
        }

        // Ввод Client читаем с клавиш и геймпада у его контроллера: машина-кукла не его пешка, событий осей у неё может не быть.
        // FKey в параметрах — имя клавиши в первых 8 байтах, остальное (KeyDetails) нули: движок найдёт по имени
        struct FKeyName
        {
            FName KeyName;
        };

        bool key_down(UObject* pc, const TCHAR* name)
        {
            UFunctionCall call(pc, STR("IsInputKeyDown"));
            auto* key = call ? call.param<FKeyName>(STR("Key")) : nullptr;
            auto* result = call ? call.param<bool>(STR("ReturnValue")) : nullptr;
            if (!key || !result) return false;
            key->KeyName = FName(name, FNAME_Add);
            call.call();
            return *result;
        }

        float key_analog(UObject* pc, const TCHAR* name)
        {
            UFunctionCall call(pc, STR("GetInputAnalogKeyState"));
            auto* key = call ? call.param<FKeyName>(STR("Key")) : nullptr;
            auto* result = call ? call.param<float>(STR("ReturnValue")) : nullptr;
            if (!key || !result) return 0.f;
            key->KeyName = FName(name, FNAME_Add);
            call.call();
            return *result;
        }

        bool read_keys(float& throttle, float& steer)
        {
            static Cached::First pc_cache{STR("PlayerController")};
            UObject* pc = pc_cache.get();
            if (!pc) return false;
            throttle = (key_down(pc, STR("W")) ? 1.f : 0.f) - (key_down(pc, STR("S")) ? 1.f : 0.f) + key_analog(pc, STR("Gamepad_RightTriggerAxis")) - key_analog(pc, STR("Gamepad_LeftTriggerAxis"));
            steer = (key_down(pc, STR("D")) ? 1.f : 0.f) - (key_down(pc, STR("A")) ? 1.f : 0.f) + key_analog(pc, STR("Gamepad_LeftX"));
            throttle = std::clamp(throttle, -1.f, 1.f), steer = std::clamp(steer, -1.f, 1.f);
            return true;
        }

        void send_input(float throttle, float steer)
        {
            auto to_i8 = [](float v) { return static_cast<int8_t>(std::lround(std::clamp(v, -1.f, 1.f) * 127.f)); };
            Coop::DriveInputBody body{to_i8(steer), to_i8(throttle), 0};
            std::vector<uint8_t> payload(sizeof(body));
            std::memcpy(payload.data(), &body, sizeof(body));
            Net::send(Coop::DriveInput, payload);
        }

        // ---- Действия в салоне (DriveAction) ----
        bool g_suppress = false;       // SystemsSync применяет состояние Host — это не действие игрока
        bool g_actions_hooked = false; // Client: перехваты поставлены

        size_t param_offset(UFunction* fn, const TCHAR* name)
        {
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
                if (p->GetName() == name) return static_cast<size_t>(p->GetOffset_ForInternal());
            return SIZE_MAX;
        }

        void forward_action(uint8_t kind, uint8_t value, uint8_t arg)
        {
            if (g_suppress || !PassengerSync::driving()) return;
            // булевы действия приводим к 0/1; зажигание и коробка — как есть (Host их зажмёт)
            if (kind != Coop::DriveIgnition && kind != Coop::DriveGear) value = value != 0;
            Coop::DriveActionBody body{kind, value, arg};
            std::vector<uint8_t> payload(sizeof(body));
            std::memcpy(payload.data(), &body, sizeof(body));
            Net::send(Coop::DriveAction, payload);
            Output::send<LogLevel::Verbose>(STR("[DriveAction] Client → Host: вид {} значение {} арг {}\n"), kind, value, arg);
        }

        // Перехват вызова у куклы Client: значение (и необязательный слот) берём из параметров функции по имени
        template <uint8_t K>
        void hook_action(UObject* object, const TCHAR* fn_name, const TCHAR* value_param, const TCHAR* arg_param = nullptr)
        {
            static size_t value_off = SIZE_MAX, arg_off = SIZE_MAX;
            UFunction* fn = object ? object->GetFunctionByNameInChain(fn_name) : nullptr;
            if (!fn) { Output::send<LogLevel::Warning>(STR("[DriveAction] нет функции {}\n"), fn_name); return; }
            value_off = param_offset(fn, value_param);
            if (arg_param) arg_off = param_offset(fn, arg_param);
            if (value_off == SIZE_MAX || (arg_param && arg_off == SIZE_MAX))
            {
                Output::send<LogLevel::Warning>(STR("[DriveAction] у {} нет параметра «{}»\n"), fn_name, value_param);
                return;
            }
            UObjectGlobals::RegisterHook(object->GetClassPrivate()->GetPathName() + STR(":") + fn_name, [](UnrealScriptFunctionCallableContext& ctx, void*) {
                auto* locals = static_cast<uint8_t*>(ctx.TheStack.Locals());
                if (!locals) return;
                forward_action(K, locals[value_off], arg_off != SIZE_MAX ? locals[arg_off] : 0); // слот — int32, нужен младший байт
            }, [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
            Output::send<LogLevel::Verbose>(STR("[DriveAction] перехват {} установлен\n"), fn_name);
        }

        void install_action_hooks()
        {
            static Cached::First manager_cache{STR("BP_CarManager_C")};
            UObject* car = Cached::car();
            UObject* manager = manager_cache.get();
            if (!car || !manager) return; // повторим на следующем тике
            g_actions_hooked = true;
            hook_action<Coop::DriveHorn>(car, STR("ToggleCarHorn"), STR("Active"));
            hook_action<Coop::DriveWipers>(manager, STR("SetWipersOnOffVariable"), STR("WipersOn"));
            hook_action<Coop::DriveLights>(manager, STR("SetCarLightsOn"), STR("Set Car Lights On"));
            hook_action<Coop::DriveIgnition>(manager, STR("SetCarIgnitionState"), STR("NewState"));
            hook_action<Coop::DriveGear>(manager, STR("SetCarGearBoxState"), STR("Updated"));
            hook_action<Coop::DriveAbility>(car, STR("SetAbilityPressed"), STR("Pressed"), STR("Slot"));
        }
    } // namespace

    void suppress_actions(bool on) { g_suppress = on; }

    void on_action(const uint8_t* body, size_t size)
    {
        if (size < sizeof(Coop::DriveActionBody)) return;
        Coop::DriveActionBody a;
        std::memcpy(&a, body, sizeof(a));
        static Cached::First manager_cache{STR("BP_CarManager_C")};
        UObject* car = Cached::car();
        UObject* manager = manager_cache.get();
        if (!car || !manager) return;
        bool done = false;
        switch (a.Kind)
        {
        case Coop::DriveHorn:
            if (UFunctionCall c(car, STR("ToggleCarHorn")); c)
                if (auto* p = c.param<bool>(STR("Active"))) *p = a.Value != 0, c.call(), done = true;
            break;
        case Coop::DriveWipers:
            if (UFunctionCall c(manager, STR("SetWipersOnOffVariable")); c)
                if (auto* p = c.param<bool>(STR("WipersOn"))) *p = a.Value != 0, c.call(), done = true;
            break;
        case Coop::DriveLights: // на машине, не на менеджере: она ведёт анимацию фар и сама зовёт менеджера
            if (UFunctionCall c(car, STR("Set Headlights On")); c)
                if (auto* p = c.param<bool>(STR("On"))) *p = a.Value != 0, c.call(), done = true;
            break;
        case Coop::DriveIgnition:
            if (UFunctionCall c(manager, STR("SetCarIgnitionState")); c)
                if (auto* p = c.param<uint8_t>(STR("NewState"))) *p = (std::min)(a.Value, Coop::IgnitionMax), c.call(), done = true;
            break;
        case Coop::DriveGear:
            if (UFunctionCall c(car, STR("ToggleGearBoxState")); c)
                if (auto* p = c.param<uint8_t>(STR("New State"))) *p = (std::min<uint8_t>)(a.Value, 1), c.call(), done = true;
            break;
        case Coop::DriveAbility:
            if (UFunctionCall c(car, STR("SetAbilityPressed")); c)
            {
                auto* slot = c.param<int32_t>(STR("Slot"));
                auto* pressed = c.param<bool>(STR("Pressed"));
                if (slot && pressed) *slot = (std::min<uint8_t>)(a.Arg, 7), *pressed = a.Value != 0, c.call(), done = true;
            }
            break;
        default:
            return;
        }
        Output::send<LogLevel::Verbose>(STR("[DriveAction] Host: вид {} значение {} арг {} — {}\n"), a.Kind, a.Value, a.Arg, done ? STR("выполнено") : STR("НЕ вышло"));
    }

    void on_input(const uint8_t* body, size_t size)
    {
        if (size < sizeof(Coop::DriveInputBody)) return;
        Coop::DriveInputBody in;
        std::memcpy(&in, body, sizeof(in));
        g_remote[1] = std::clamp(in.Steer / 127.f, -1.f, 1.f);
        g_remote[0] = std::clamp(in.Throttle / 127.f, -1.f, 1.f);
        g_remote_ms = now_ms();
    }

    void tick(float throttle, float steer)
    {
        uint64_t now = now_ms();
        if (g_remote_ms && now - g_remote_ms < RemoteTimeoutMs) throttle = g_remote[0], steer = g_remote[1];
        g_value[0] = throttle, g_value[1] = steer;
        if (throttle == 0.f && steer == 0.f && !g_steer_sent) return;
        UObject* car = find_car();
        if (!car) return;
        // движок шлёт событие руля только водителю (пешком 0 вызовов/с), событие газа — всегда; поэтому руль вызываем сами,
        // и один раз нулём после отпускания (иначе осталась бы последняя ось)
        if (steer != 0.f || g_steer_sent)
        {
            UFunctionCall call(car, STR("SteerLeftRight"));
            if (auto* axis = call ? call.param<float>(STR("Axis Value")) : nullptr) *axis = steer, call.call();
            g_steer_sent = steer != 0.f;
        }
        if (now - g_log_ms < 1000) return;
        g_log_ms = now;
        auto** vc = property<UObject*>(car, STR("VehicleController"));
        auto f = [&](UObject* o, const TCHAR* n) { auto* p = o ? property<float>(o, n) : nullptr; return p ? *p : -999.f; };
        Output::send<LogLevel::Verbose>(
                STR("[DriveProbe] вызовов газ/руль {}/{}; Raw {:.2f} Cur {:.2f} UserSteer {:.2f}; VC газ {:.2f} руль {:.2f} тормоз {:.2f}\n"),
                g_calls[0].exchange(0), g_calls[1].exchange(0), f(car, STR("RawThrottle")), f(car, STR("ThrottleCurrent")), f(car, STR("UserSteerInput")),
                f(vc ? *vc : nullptr, STR("ThrottleInput")), f(vc ? *vc : nullptr, STR("SteerInput")), f(vc ? *vc : nullptr, STR("BrakeInput")));
    }

    void client_tick(bool enabled)
    {
        uint64_t now = now_ms();
        if (!enabled)
        {
            if (g_sent_nonzero) send_input(0, 0), g_sent_nonzero = false; // высадка: Host сразу получает нули (иначе — по таймауту 250 мс)
            return;
        }
        if (!g_actions_hooked) install_action_hooks();
        if (now - g_send_ms < Net::adaptive_interval_ms(Coop::DriveInputIntervalMs)) return;
        g_send_ms = now;
        float throttle = 0, steer = 0;
        if (!read_keys(throttle, steer)) return;
        send_input(throttle, steer);
        g_sent_nonzero = throttle != 0.f || steer != 0.f;
        if (now - g_client_log_ms < 2000) return;
        g_client_log_ms = now;
        Output::send<LogLevel::Verbose>(STR("[DriveInput] Client: газ {:.2f} руль {:.2f}\n"), throttle, steer);
    }
} // namespace DriveProbe
