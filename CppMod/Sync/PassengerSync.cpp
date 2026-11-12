#include "PassengerSync.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "Game/Items.h"
#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/CarSync.h"
#include "Sync/DoorSync.h"
#include "Sync/Notify.h"
#include "Sync/Presence.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using namespace RC;
using namespace RC::Unreal;

// Host решает (свободен ли слот заднего ряда, спека 3.3.3), Client только просит и садится после подтверждения (3.3.2 п.3).
// Посадка — подход 3.3.1-B: каждый кадр ставим персонажа на место сиденья вслед за машиной-куклой. Аттач (3.3.1-A) не годится:
// игра сама переставляет прикреплённого к машине персонажа на место водителя (тест 2026-10-26), а штатный вход в куклу
// роняет под карту.
namespace PassengerSync
{
    std::atomic<bool> g_key_pressed{false};

    namespace
    {
        struct Vec
        {
            float X, Y, Z;
        };
        struct Rot
        {
            float Pitch, Yaw, Roll;
        };
        struct alignas(16) Transform
        {
            float Rotation[4];
            float Translation[4];
            float Scale[4];
        };
        struct RawArray
        {
            void* Data;
            int32_t Num;
            int32_t Max;
        };
        struct Axes // оси повёрнутой системы (FRotationMatrix UE)
        {
            Vec X, Y, Z;
        };

        constexpr float MaxSeatDistance = 600;     // см от машины, с которого можно просить посадку
        const TCHAR* SeatSlot[2] = {STR("SLOT_BackSeatLeft"), STR("SLOT_BackSeatRight")};
        constexpr uint8_t MoveWalking = 1; // EMovementMode::MOVE_Walking
        constexpr uint64_t GuardIntervalMs = 200;

        Vec g_seat_offset{-95, 0, 40};       // левое место, из coop.ini (set_seat_offsets); 40 — подобрано в тесте 2026-10-26
        float g_exit_side_cm = 230;          // coop.ini exit_side_cm: на сколько см от оси машины в сторону высаживаем; 0 — штатная точка
        constexpr float ExitHeightCm = 120;  // над началом координат машины: капсула игрока, дальше падает на землю
        Vec g_seat_offset_right{-95, 0, 40}; // правое место: те же числа от отражённой точки водителя (тест 2026-10-30: симметрия сошлась)
        int g_seat = -1;              // Client: где сидим
        int g_resume_seat = -1;       // Client: место, с которого нас унесла смена зоны, — вернуться на него, когда машина Host появится
        uint64_t g_last_resume_ms = 0;
        constexpr uint64_t ResumeRetryMs = 1500;
        Vec g_seat_local{};           // место сиденья в осях машины (от её начала координат)
        int g_host_passenger = -1;    // Host: где сидит пассажир
        uint64_t g_last_guard_ms = 0, g_last_exit_ms = 0;
        int g_guard_attempts = 0;

        uint64_t now_ms()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        Axes axes(float pitch, float yaw, float roll)
        {
            constexpr float ToRad = 3.14159265f / 180.0f;
            float sp = std::sin(pitch * ToRad), cp = std::cos(pitch * ToRad);
            float sy = std::sin(yaw * ToRad), cy = std::cos(yaw * ToRad);
            float sr = std::sin(roll * ToRad), cr = std::cos(roll * ToRad);
            return {{cp * cy, cp * sy, sp}, {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp}, {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
        }

        float dot(const Vec& a, const Vec& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }

        // Место храним на диске: после перезапуска игры Client сам вернётся на то же место (папка coop/ создаётся модом)
        const std::filesystem::path SeatFile = L"ue4ss/Mods/PacificDriveCoop/coop/last_seat.txt";

        void save_seat(int seat)
        {
            std::ofstream f(SeatFile, std::ios::trunc);
            if (f) f << seat;
        }

        void clear_saved_seat()
        {
            std::error_code ec;
            std::filesystem::remove(SeatFile, ec);
        }

        int load_saved_seat()
        {
            std::ifstream f(SeatFile);
            int seat = -1;
            if (f && (f >> seat) && seat >= 0 && seat <= 1) return seat;
            return -1;
        }

        // Затемнение камеры «из чёрного» при посадке/высадке: у игры вход — тоже камера + затемнение (EnterCarDipToBlack), скелетной анимации нет
        void camera_fade_in(UObject* character, float seconds)
        {
            auto** controller = property<UObject*>(character, STR("Controller"));
            if (!controller || !*controller) return;
            auto** cam = property<UObject*>(*controller, STR("PlayerCameraManager"));
            if (!cam || !*cam) return;
            struct Color
            {
                float R, G, B, A;
            };
            UFunctionCall fade(*cam, STR("StartCameraFade"));
            auto* from = fade ? fade.param<float>(STR("FromAlpha")) : nullptr;
            auto* to = fade ? fade.param<float>(STR("ToAlpha")) : nullptr;
            auto* dur = fade ? fade.param<float>(STR("Duration")) : nullptr;
            auto* color = fade ? fade.param<Color>(STR("Color")) : nullptr;
            if (!from || !to || !dur || !color) return;
            *from = 1.0f;
            *to = 0.0f;
            *dur = seconds;
            *color = {0, 0, 0, 1};
            fade.call();
        }

        UObject* own_character() { return Cached::character(); }

        // Персонаж «в машине» по штатной логике игры: прикреплён (IsCharacterAttached) или PercentInCar > 0 — второе нужно
        // потому, что вход через дверь идёт таймлайном, и прикрепление может быть позже (тест 2026-10-30: Client сел за руль
        // куклы, защита по одному IsCharacterAttached не сработала)
        struct CarState
        {
            bool Attached = false;
            float Percent = 0;
            bool inside() const { return Attached || Percent > 0.02f; }
        };

        CarState character_car_state(UObject* character)
        {
            CarState state;
            UFunctionCall attached(character, STR("IsCharacterAttached"));
            if (auto* p = attached ? attached.param<bool>(STR("Attached")) : nullptr)
            {
                attached.call();
                state.Attached = *p;
            }
            if (auto* percent = property<float>(character, STR("PercentInCar"))) state.Percent = *percent;
            return state;
        }

        // Панель быстрого выбора прячется, пока HUD считает игрока «в машине» (PDP_HotbarHUD.PlayerEnterCar/PlayerExitCar); наша
        // высадка и выдёргивание из машины Host-а этого не сообщают — у Client в зоне панель пропадала (ячейки при этом работали).
        // Вызов только показывает панель: если причина в другом, вреда нет
        void show_hotbar()
        {
            std::vector<UObject*> huds;
            UObjectGlobals::FindAllOf(STR("PDP_HotbarHUD_C"), huds);
            for (auto* hud : huds)
                if (UFunctionCall fn(hud, STR("PlayerExitCar")); fn) fn.call();
        }

        bool force_exit_car(UObject* character)
        {
            UFunctionCall exit(character, STR("Exit Car"));
            auto* p_dip = exit ? exit.param<bool>(STR("DipToBlack")) : nullptr;
            auto* p_teleport = exit ? exit.param<bool>(STR("Teleport")) : nullptr;
            if (!p_dip || !p_teleport) return false;
            *p_dip = false;
            *p_teleport = true;
            exit.call();
            show_hotbar();
            return true;
        }

        uint64_t g_describe_at = 0; // когда записать состояние персонажа после «Exit Car» от сторожа

        // Состояние персонажа для разбора «Client прикован к месту, меню Esc сломано» (2026-11-12): таймлайны входа/выхода, режим движения,
        // игнор ввода, к чему прикреплён корень. Ничего не меняет
        void log_character_state(const TCHAR* tag)
        {
            UObject* character = own_character();
            if (!character)
            {
                Output::send<LogLevel::Verbose>(STR("[Passenger] состояние ({}): персонажа нет\n"), tag);
                return;
            }
            auto timeline = [&](const TCHAR* name) {
                auto** t = property<UObject*>(character, name);
                UFunctionCall fn(t ? *t : nullptr, STR("IsPlaying"));
                auto* r = fn ? fn.param<bool>(STR("ReturnValue")) : nullptr;
                if (!r) return -1;
                fn.call();
                return *r ? 1 : 0;
            };
            auto flag = [&](UObject* object, const TCHAR* function) {
                UFunctionCall fn(object, function);
                auto* r = fn ? fn.param<bool>(STR("ReturnValue")) : nullptr;
                if (!r) return -1;
                fn.call();
                return *r ? 1 : 0;
            };
            auto** movement = property<UObject*>(character, STR("CharacterMovement"));
            auto* mode = movement && *movement ? property<uint8_t>(*movement, STR("MovementMode")) : nullptr;
            auto** controller = property<UObject*>(character, STR("Controller"));
            UObject* ctrl = controller ? *controller : nullptr;
            auto** root = property<UObject*>(character, STR("RootComponent"));
            auto** parent = root && *root ? property<UObject*>(*root, STR("AttachParent")) : nullptr;
            CarState car = character_car_state(character);
            Output::send<LogLevel::Verbose>(STR("[Passenger] состояние ({}): прикреплён={} в машине {:.2f}, к {}; таймлайны (1 идёт): вход {} выход {} затемнение выхода {} затемнение входа {}; "
                                                "режим движения {}, игнор движения {} взгляда {}; g_seat={}\n"),
                                            tag, car.Attached, car.Percent, parent && *parent ? (*parent)->GetFullName() : StringType(STR("ничего")), timeline(STR("EnterCarTimeline")),
                                            timeline(STR("ExitCarTimeline")), timeline(STR("ExitCar_FadeToBlackTimeline")), timeline(STR("EnterCarDipToBlack")),
                                            mode ? static_cast<int>(*mode) : -1, flag(ctrl, STR("IsMoveInputIgnored")), flag(ctrl, STR("IsLookInputIgnored")), g_seat);
        }

        template <typename T>
        bool call1(UObject* object, const TCHAR* function, const TCHAR* param, T value)
        {
            UFunctionCall fn(object, function);
            auto* p = fn ? fn.param<T>(param) : nullptr;
            if (!p) return false;
            *p = value;
            fn.call();
            return true;
        }

        // Звук салона: мы телепортируем игрока на место, а не сажаем штатно, и машина-кукла не знает, что он внутри
        // (AC_CarAudio.PlayerInsideCar = false) — музыка звучит «из закрытой машины». Те же вызовы делает игра при посадке
        // (Lua F1/Shift+F1, тест 2026-10-30: звук становится салонным и возвращается, RECON.md «6.14»)
        void car_audio_inside(UObject* car, bool inside)
        {
            if (!car) return; // машины уже нет (карта сменилась, связь пропала) — переключать нечего
            auto** audio = property<UObject*>(car, STR("AC_CarAudio"));
            if (!audio || !*audio || !call1<bool>(*audio, inside ? STR("OnPlayerStartEnterCar") : STR("OnPlayerStartExitCar"), STR("Teleport"), true))
                Output::send<LogLevel::Warning>(STR("[Passenger] не удалось переключить звук салона (нет AC_CarAudio или функции)\n"));
        }

        // Ограничение взаимодействий пассажира: пока он сидит, подсвечиваться (а значит, и нажиматься) могут только двери и карта.
        // Единая точка — BP_HighlightManager_C."Should Allow Highlight"(Highlight, out Allow, out FailReason): после неё ставим Allow=false.
        // Белый список — по классам владельца подсветки (Lua F9, RECON.md «6.14»):
        //  - BP_CarPart_Interactive_C — шесть точек дверей/капота/багажника;
        //  - BP_PlayerCarNew_C с компонентами Highlight_LeftHandle / Highlight_RightHandle — ручки дверей изнутри (Highlight_Key, ключ зажигания — нет);
        //  - BP_Carpart_Internal_GobblerScreen_C — «карта» (экран Гобблера) — shortcut: по догадке, подтвердить тестом
        // Остальное (руль, рычаг, радио, ручник, детали…) недоступно. Отключается coop.ini restrict_interactions=0.
        bool g_restrict = true;
        bool g_hook_tried = false;
        size_t g_highlight_offset = SIZE_MAX, g_allow_offset = SIZE_MAX;
        FProperty* g_highlight_prop = nullptr;
        FProperty* g_allow_prop = nullptr;
        size_t g_inside_offset = SIZE_MAX;
        FProperty* g_inside_prop = nullptr;
        std::set<StringType> g_blocked_logged_names;

        bool highlight_allowed(UObject* highlight)
        {
            UObject* owner = highlight->GetOuterPrivate();
            if (!owner) return true; // не знаем, чей — не мешаем
            auto owner_class = owner->GetClassPrivate()->GetName();
            if (owner_class == STR("BP_Carpart_Internal_GobblerScreen_C")) return true;
            // BP_CarPart_Interactive_C — точка снятия/установки детали у КАЖДОГО слота машины (тест 2026-11-06: десятки разрешённых),
            // а не только двери. Двери — слоты Car.Equip.Body.DoorsFL/DoorsFR
            if (owner_class == STR("BP_CarPart_Interactive_C"))
            {
                auto** slot = property<UObject*>(owner, STR("SlotSceneComponent"));
                auto* tag = slot && *slot ? property<FName>(*slot, STR("SlotName")) : nullptr;
                return tag && tag->ToString().starts_with(STR("Car.Equip.Body.Doors"));
            }
            if (owner_class == STR("BP_PlayerCarNew_C"))
            {
                auto name = highlight->GetName();
                return name == STR("Highlight_LeftHandle") || name == STR("Highlight_RightHandle");
            }
            return false;
        }

        // Защита от радиации: игра даёт её игроку «в машине» по доле GetPlayerInCarPercent (тест 0.17.4 2026-11-07: водитель —
        // 1.00 и playerProtected=1, сидящий пассажир — 0.00 и 0, радиация приходит обоим). Пока пассажир сидит, отвечаем 1.0
        struct InCarHook
        {
            const TCHAR* cls;
            const TCHAR* param;
            FProperty* prop = nullptr;
            size_t offset = SIZE_MAX;
            bool done = false;
        };
        InCarHook g_in_car[] = {{STR("PDP_MainCharacter_C"), STR("In Car percent")}, {STR("BP_MainCharacterManager_C"), STR("Percent")}};

        template <size_t I>
        void install_in_car_percent()
        {
            auto& h = g_in_car[I];
            if (h.done) return;
            auto* object = UObjectGlobals::FindFirstOf(h.cls);
            UFunction* fn = object ? object->GetFunctionByNameInChain(STR("GetPlayerInCarPercent")) : nullptr;
            if (!fn) return; // ещё не появился — попробуем при следующей посадке
            h.done = true;
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
                if (p->GetName() == h.param) h.prop = p, h.offset = static_cast<size_t>(p->GetOffset_ForInternal());
            if (h.offset == SIZE_MAX)
            {
                Output::send<LogLevel::Warning>(STR("[Passenger] у {}.GetPlayerInCarPercent нет параметра «{}» — защита пассажира от радиации не включена\n"), h.cls, h.param);
                return;
            }
            UObjectGlobals::RegisterHook(
                    object->GetClassPrivate()->GetPathName() + STR(":GetPlayerInCarPercent"), [](UnrealScriptFunctionCallableContext&, void*) {},
                    [](UnrealScriptFunctionCallableContext& context, void*) {
                        if (g_seat < 0) return;
                        auto& h = g_in_car[I];
                        auto* locals = static_cast<uint8_t*>(context.TheStack.Locals());
                        auto* percent = static_cast<float*>(FindOutParamValueAddress(context.TheStack, h.prop));
                        if (!percent && locals) percent = reinterpret_cast<float*>(locals + h.offset);
                        if (percent) *percent = 1.0f;
                    },
                    nullptr);
            Output::send<LogLevel::Verbose>(STR("[Passenger] сидящий пассажир «в машине» для {}.GetPlayerInCarPercent (защита от радиации)\n"), h.cls);
        }

        void install_highlight_filter()
        {
            if (g_hook_tried || !g_restrict) return;
            auto* manager = UObjectGlobals::FindFirstOf(STR("BP_HighlightManager_C"));
            if (!manager) return; // нет менеджера (меню) — попробуем при следующей посадке
            g_hook_tried = true;
            UFunction* fn = manager->GetFunctionByNameInChain(STR("Should Allow Highlight"));
            if (!fn)
            {
                Output::send<LogLevel::Warning>(STR("[Passenger] нет BP_HighlightManager_C.Should Allow Highlight — ограничение взаимодействий выключено\n"));
                return;
            }
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (p->GetName() == STR("Highlight")) g_highlight_prop = p, g_highlight_offset = static_cast<size_t>(p->GetOffset_ForInternal());
                else if (p->GetName() == STR("Allow")) g_allow_prop = p, g_allow_offset = static_cast<size_t>(p->GetOffset_ForInternal());
            }
            if (g_highlight_offset == SIZE_MAX || g_allow_offset == SIZE_MAX)
            {
                Output::send<LogLevel::Warning>(STR("[Passenger] у Should Allow Highlight нет параметров Highlight/Allow — ограничение выключено\n"));
                return;
            }
            auto name = manager->GetClassPrivate()->GetPathName() + STR(":Should Allow Highlight");
            UObjectGlobals::RegisterHook(
                    name, [](UnrealScriptFunctionCallableContext&, void*) {},
                    [](UnrealScriptFunctionCallableContext& context, void*) {
                        if (g_seat < 0) return;
                        auto* locals = static_cast<uint8_t*>(context.TheStack.Locals());
                        auto* highlight = locals ? *reinterpret_cast<UObject**>(locals + g_highlight_offset) : nullptr;
                        // Allow — выходной параметр: при вызове из Blueprint его адрес лежит в OutParms вызывающего; при вызове
                        // через ProcessEvent (OutParms нет) — в буфере параметров, то есть в Locals
                        auto* allow = static_cast<bool*>(FindOutParamValueAddress(context.TheStack, g_allow_prop));
                        if (!allow && locals) allow = reinterpret_cast<bool*>(locals + g_allow_offset);
                        if (!highlight || !allow || !*allow) return;
                        bool pass = highlight_allowed(highlight);
                        if (!pass) *allow = false;
                        // Подсветка перебирается каждый кадр — в лог каждая пара «класс / компонент» один раз за запуск игры
                        auto* owner = highlight->GetOuterPrivate();
                        // имя самого владельца — чтобы отличить дверь от других BP_CarPart_Interactive_C (пункт «лишние подсветки»)
                        auto what = (owner ? owner->GetClassPrivate()->GetName() + STR(" ") + owner->GetName() : StringType(STR("?"))) + STR(" / ") +
                                    highlight->GetName();
                        if (g_blocked_logged_names.insert((pass ? STR("+") : STR("-")) + what).second)
                            Output::send<LogLevel::Verbose>(STR("[Passenger] подсветка {} пассажиру: {}\n"), pass ? STR("разрешена") : STR("запрещена"), what);
                    },
                    nullptr);
            Output::send<LogLevel::Verbose>(STR("[Passenger] ограничение взаимодействий пассажира включено ({})\n"), name);

            // Менеджер подсветки считает, что игрок «в машине», по штатному входу (IsPlayerInCar) — у сидящего пассажира он снаружи:
            // берётся «уличный» радиус/дистанция выбора, и салонные объекты (двери изнутри, экран Гобблера) могут быть недосягаемы
            // (тест 2026-10-30: «ничего не подсвечивается»). Пока пассажир сидит, отвечаем «внутри»
            if (UFunction* in_car = manager->GetFunctionByNameInChain(STR("IsPlayerInCar")))
            {
                for (FProperty* p : TFieldRange<FProperty>(in_car, EFieldIterationFlags::IncludeDeprecated))
                    if (p->GetName() == STR("Inside")) g_inside_prop = p, g_inside_offset = static_cast<size_t>(p->GetOffset_ForInternal());
                if (g_inside_offset != SIZE_MAX)
                {
                    UObjectGlobals::RegisterHook(
                            manager->GetClassPrivate()->GetPathName() + STR(":IsPlayerInCar"), [](UnrealScriptFunctionCallableContext&, void*) {},
                            [](UnrealScriptFunctionCallableContext& context, void*) {
                                if (g_seat < 0) return;
                                auto* locals = static_cast<uint8_t*>(context.TheStack.Locals());
                                auto* inside = static_cast<bool*>(FindOutParamValueAddress(context.TheStack, g_inside_prop));
                                if (!inside && locals) inside = reinterpret_cast<bool*>(locals + g_inside_offset);
                                if (inside) *inside = true;
                            },
                            nullptr);
                    Output::send<LogLevel::Verbose>(STR("[Passenger] сидящий пассажир считается «в машине» для выбора подсветки (IsPlayerInCar)\n"));
                }
            }
        }

        Vec actor_location(UObject* actor)
        {
            UFunctionCall fn(actor, STR("K2_GetActorLocation"));
            auto* ret = fn ? fn.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!ret) return {};
            fn.call();
            return *ret;
        }

        // Слот заднего ряда пуст = место свободно (спека 3.3.3, проверено в игре: снятие батареи 1→0)
        // Пассажир заходит через заднюю дверь своей стороны: она должна быть открыта или снята (слот двери пуст)
        bool rear_door_passable(UObject* car, int seat)
        {
            auto** slot = property<UObject*>(car, seat == 1 ? STR("SLOT_RearRightDoor") : STR("SLOT_RearLeftDoor"));
            if (!slot || !*slot) return true; // слота нет в этой машине — не мешаем
            auto** item_slot = property<UObject*>(*slot, STR("ItemSlot"));
            auto* items = item_slot && *item_slot ? property<RawArray>(*item_slot, STR("Items")) : nullptr;
            if (!items || items->Num == 0) return true; // дверь снята
            // Состояние дверей, которое Host отдаёт Client-у (DoorStates менеджера машины): 2 — заднее левое, 3 — заднее правое
            // (лог 2026-11-06 23:10). Флаг DoorOpen актёра двери отстаёт на 8+ с — по нему открытую дверь считали закрытой
            if (int mask = DoorSync::known_mask(); mask >= 0 && (mask & (1 << (seat == 1 ? 3 : 2)))) return true;
            UFunctionCall where(*slot, STR("K2_GetComponentLocation"));
            auto* at = where ? where.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!at) return true;
            where.call();
            // Актёр двери стоит в точке слота (замер 2026-11-06: шарнир = SLOT_RearLeft/RightDoor)
            std::vector<UObject*> doors;
            UObjectGlobals::FindAllOf(STR("BP_CarPart_Door_Base_C"), doors);
            for (UObject* door : doors)
            {
                Vec d = actor_location(door);
                if (std::fabs(d.X - at->X) + std::fabs(d.Y - at->Y) + std::fabs(d.Z - at->Z) > 60) continue;
                auto* open = property<bool>(door, STR("DoorOpen"));
                return !open || *open;
            }
            return true; // актёр не найден — не блокируем
        }

        bool seat_free(UObject* car, int seat)
        {
            auto** slot = property<UObject*>(car, SeatSlot[seat]);
            auto** item_slot = slot && *slot ? property<UObject*>(*slot, STR("ItemSlot")) : nullptr;
            auto* items = item_slot && *item_slot ? property<RawArray>(*item_slot, STR("Items")) : nullptr;
            return items && items->Num == 0;
        }

        void place(UObject* actor, const Vec& at, float yaw)
        {
            UFunctionCall fn(actor, STR("K2_SetActorLocationAndRotation"));
            auto* p_loc = fn ? fn.param<Vec>(STR("NewLocation")) : nullptr;
            auto* p_rot = fn ? fn.param<Rot>(STR("NewRotation")) : nullptr;
            auto* p_teleport = fn ? fn.param<bool>(STR("bTeleport")) : nullptr;
            if (!p_loc || !p_rot || !p_teleport) return;
            *p_loc = at;
            *p_rot = {0, yaw, 0};
            *p_teleport = true;
            fn.call();
        }

        void set_walking(UObject* character, bool walking)
        {
            auto** movement = property<UObject*>(character, STR("CharacterMovement"));
            if (!movement || !*movement) return;
            if (walking) call1<uint8_t>(*movement, STR("SetMovementMode"), STR("NewMovementMode"), MoveWalking);
            else
            {
                UFunctionCall disable(*movement, STR("DisableMovement"));
                if (disable) disable.call();
            }
            // Сидя — без коллизии: капсула внутри машины не должна толкаться (спека 3.3.2 п.4)
            call1(character, STR("SetActorEnableCollision"), STR("bNewActorEnableCollision"), walking);
        }

        // Каждый кадр после CarSync::tick: место сиденья = поза куклы + повёрнутое смещение
        void follow_car(UObject* character)
        {
            Coop::CarStateBody car;
            if (!CarSync::puppet_pose(car)) return;
            Axes a = axes(car.Pitch, car.Yaw, car.Roll);
            const Vec& l = g_seat_local;
            Vec at{car.X + a.X.X * l.X + a.Y.X * l.Y + a.Z.X * l.Z, car.Y + a.X.Y * l.X + a.Y.Y * l.Y + a.Z.Y * l.Z,
                   car.Z + a.X.Z * l.X + a.Y.Z * l.Y + a.Z.Z * l.Z};
            place(character, at, car.Yaw);
        }

        // Посадка/высадка без телепорта: путь от места игрока к порогу двери своей стороны и к сиденью (RECON 6.38). Игра ведёт
        // игрока по таймлайну 2 с от точки двери; её точки у задних дверей все совпадают с шарниром, поэтому путь строим сами.
        // Левое место — левая задняя дверь (y<0), правое — правая (y>0). Камера = капсула игрока, едет вместе с ней
        struct Anim
        {
            bool active = false, enter = true;
            uint64_t t0 = 0;
            Vec start{}, door{}, finish{}; // enter: все в осях машины; exit: start и door в осях машины, finish — мир
        } g_anim;
        constexpr uint64_t AnimMs = 900;
        constexpr float DoorSideCm = 95;     // шарниры задних дверей (замер 2026-11-06: y = −95 / +94)
        constexpr float AnimMaxStartCm = 450; // дальше — не ходим, как раньше: телепорт с затемнением
        constexpr float DoorLegShare = 0.55f; // доля времени на путь до порога

        Vec lerp(const Vec& a, const Vec& b, float t) { return {a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t}; }

        Vec car_local(const Coop::CarStateBody& car, const Vec& world)
        {
            Axes a = axes(car.Pitch, car.Yaw, car.Roll);
            Vec d{world.X - car.X, world.Y - car.Y, world.Z - car.Z};
            return {dot(d, a.X), dot(d, a.Y), dot(d, a.Z)};
        }

        Vec car_world(const Coop::CarStateBody& car, const Vec& l)
        {
            Axes a = axes(car.Pitch, car.Yaw, car.Roll);
            return {car.X + a.X.X * l.X + a.Y.X * l.Y + a.Z.X * l.Z, car.Y + a.X.Y * l.X + a.Y.Y * l.Y + a.Z.Y * l.Z,
                    car.Z + a.X.Z * l.X + a.Y.Z * l.Y + a.Z.Z * l.Z};
        }

        // Порог двери той стороны, где сидим: тот же x, что у сиденья, снаружи кузова
        Vec door_threshold(int seat, const Vec& seat_local) { return {seat_local.X, (seat == 1 ? 1.0f : -1.0f) * DoorSideCm, seat_local.Z}; }

        // Один шаг анимации; false — анимации нет или она закончилась
        void step_anim(UObject* character)
        {
            Coop::CarStateBody pose;
            if (!g_anim.active || !CarSync::puppet_pose(pose))
            {
                if (g_anim.active) g_anim.active = false, set_walking(character, !g_anim.enter);
                return;
            }
            float t = (std::min)(1.0f, static_cast<float>(now_ms() - g_anim.t0) / AnimMs);
            float e = t * t * (3 - 2 * t); // плавный разгон и остановка
            Vec at;
            if (e < DoorLegShare) at = car_world(pose, lerp(g_anim.start, g_anim.door, e / DoorLegShare));
            else if (g_anim.enter) at = car_world(pose, lerp(g_anim.door, g_anim.finish, (e - DoorLegShare) / (1 - DoorLegShare)));
            else at = lerp(car_world(pose, g_anim.door), g_anim.finish, (e - DoorLegShare) / (1 - DoorLegShare));
            place(character, at, pose.Yaw);
            if (t >= 1.0f)
            {
                g_anim.active = false;
                if (!g_anim.enter) set_walking(character, true); // дошли до земли — снова ходим
            }
        }

        void sit(int seat)
        {
            UObject* car = CarSync::puppet_car();
            UObject* character = own_character();
            auto** socket = car ? property<UObject*>(car, STR("TempSocket_Seat")) : nullptr;
            Coop::CarStateBody pose;
            if (!character || !socket || !*socket || !CarSync::puppet_pose(pose))
            {
                Output::send<LogLevel::Error>(STR("[Passenger] сесть не удалось: нет машины/персонажа/TempSocket_Seat\n"));
                return;
            }
            // Точка водителя в осях машины — из мировых координат (TempSocket_Seat может быть вложен не в корень)
            UFunctionCall socket_loc(*socket, STR("K2_GetComponentLocation"));
            auto* world = socket_loc ? socket_loc.param<Vec>(STR("ReturnValue")) : nullptr;
            if (!world) return;
            socket_loc.call();
            Axes a = axes(pose.Pitch, pose.Yaw, pose.Roll);
            Vec d{world->X - pose.X, world->Y - pose.Y, world->Z - pose.Z};
            Vec driver{dot(d, a.X), dot(d, a.Y), dot(d, a.Z)};
            // Правое место отсчитывается от точки водителя, отражённой через ось машины (водитель слева, y<0)
            const Vec& offset = seat == 1 ? g_seat_offset_right : g_seat_offset;
            g_seat_local = {driver.X + offset.X, (seat == 1 ? -driver.Y : driver.Y) + offset.Y, driver.Z + offset.Z};

            Vec start = car_local(pose, actor_location(character));
            Vec gap{start.X - g_seat_local.X, start.Y - g_seat_local.Y, start.Z - g_seat_local.Z};
            if (std::sqrt(dot(gap, gap)) <= AnimMaxStartCm) g_anim = {true, true, now_ms(), start, door_threshold(seat, g_seat_local), g_seat_local};
            else
            {
                g_anim.active = false;
                camera_fade_in(character, 0.6f); // далеко от машины: как раньше, телепорт из чёрного
            }
            set_walking(character, false);
            g_seat = seat;
            save_seat(seat);
            Presence::set_seated(true);
            car_audio_inside(CarSync::puppet_car(), true);
            install_highlight_filter();
            install_in_car_percent<0>();
            install_in_car_percent<1>();
            if (g_anim.active) step_anim(character);
            else follow_car(character);
            Output::send<LogLevel::Verbose>(STR("[Passenger] сел на задний {}: водитель в осях машины ({:.0f}, {:.0f}, {:.0f}), место ({:.0f}, {:.0f}, {:.0f})\n"),
                                            seat == 0 ? STR("левый") : STR("правый"), driver.X, driver.Y, driver.Z, g_seat_local.X, g_seat_local.Y, g_seat_local.Z);
        }

        void stand_up(const wchar_t* reason)
        {
            UObject* character = own_character();
            if (character)
            {
                UObject* car = CarSync::puppet_car();
                if (!car) car = Cached::car();
                Coop::CarStateBody pose{};
                bool have_pose = CarSync::puppet_pose(pose);
                Vec exit;
                if (g_exit_side_cm > 0 && have_pose && g_seat >= 0)
                {
                    // Выходим сбоку, с той стороны, где сидели: штатная точка (LocateSafeExitPoint) ставила на крышу (тест 2026-10-31).
                    // Только поворот по yaw — наклон машины не должен уносить точку вверх/вниз; по высоте над кузовом, дальше земля подхватит
                    // (SetMovementMode Walking). Левое место — минус по оси «вправо» машины
                    constexpr float ToRad = 3.14159265f / 180.0f;
                    Vec forward{std::cos(pose.Yaw * ToRad), std::sin(pose.Yaw * ToRad), 0}, right{-forward.Y, forward.X, 0};
                    float side = (g_seat == 1 ? 1.0f : -1.0f) * g_exit_side_cm, along = g_seat_local.X;
                    exit = {pose.X + forward.X * along + right.X * side, pose.Y + forward.Y * along + right.Y * side, pose.Z + ExitHeightCm};
                }
                else
                {
                    // Запасной вариант (exit_side_cm=0 или нет позы): штатная точка машины; нет её — над машиной
                    exit = car ? actor_location(car) : actor_location(character);
                    exit.Z += 150;
                    UFunctionCall safe(car, STR("LocateSafeExitPoint"));
                    auto* p_tf = safe ? safe.param<Transform>(STR("Transform")) : nullptr;
                    auto* p_failed = safe ? safe.param<bool>(STR("Failed_UsingSafetyTrace")) : nullptr;
                    if (p_tf && p_failed)
                    {
                        safe.call();
                        if (!*p_failed) exit = {p_tf->Translation[0], p_tf->Translation[1], p_tf->Translation[2]};
                    }
                }
                // Выходим через порог своей стороны на точку выхода; ходьба включится, когда дойдём (step_anim)
                if (have_pose && g_seat >= 0 && g_exit_side_cm > 0)
                {
                    g_anim = {true, false, now_ms(), g_seat_local, door_threshold(g_seat, g_seat_local), exit};
                    place(character, car_world(pose, g_seat_local), pose.Yaw);
                }
                else
                {
                    camera_fade_in(character, 0.5f);
                    place(character, exit, pose.Yaw);
                    set_walking(character, true);
                }
            }
            if (g_resume_seat < 0) clear_saved_seat(); // вышли сами — не возвращать после перезапуска; унесло зоной/связью — оставить
            g_seat = -1;
            Presence::set_seated(false);
            car_audio_inside(CarSync::puppet_car(), false);
            show_hotbar();
            Output::send<LogLevel::Verbose>(STR("[Passenger] вышел из машины ({})\n"), reason);
        }

        void on_key()
        {
            if (g_seat >= 0)
            {
                Net::send_seat(Coop::RequestExitVehicle, {static_cast<uint8_t>(g_seat), 0});
                Output::send<LogLevel::Verbose>(STR("[Passenger] прошу выход\n"));
                return;
            }
            UObject* car = CarSync::puppet_car();
            UObject* character = own_character();
            if (car && character)
                if (CarState state = character_car_state(character); state.inside())
                {
                    g_guard_attempts = 0; // ручной запрос — охрана снова может пробовать
                    bool called = force_exit_car(character);
                    Output::send<LogLevel::Verbose>(STR("[Passenger] Ctrl+E: вы внутри машины-куклы ({}) — вытаскиваю (прикреплён={}, в машине {:.2f})\n"),
                                                    called ? STR("Exit Car вызван") : STR("Exit Car НЕ найден"), state.Attached, state.Percent);
                    return;
                }
            if (!car || !character)
            {
                Output::send<LogLevel::Verbose>(STR("[Passenger] сесть нельзя: машина не под управлением Host (нет связи/CarState)\n"));
                return;
            }
            Vec a = actor_location(car), b = actor_location(character);
            float distance = std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
            if (distance > MaxSeatDistance)
            {
                Output::send<LogLevel::Verbose>(STR("[Passenger] до машины {:.0f} см — подойдите ближе {:.0f}\n"), distance, MaxSeatDistance);
                return;
            }
            // У Client своя копия машины из сейва Host — те же модули; не просим заведомо занятое (решает всё равно Host)
            // Сторона — та, с которой стоит пассажир (y в осях машины: минус — слева); на другую не пересаживаем
            Coop::CarStateBody pose{};
            int preferred = 0;
            if (CarSync::puppet_pose(pose))
            {
                Axes ax = axes(pose.Pitch, pose.Yaw, pose.Roll);
                preferred = dot({b.X - pose.X, b.Y - pose.Y, b.Z - pose.Z}, ax.Y) < 0 ? 0 : 1;
            }
            int seat = preferred;
            if (!seat_free(car, seat))
            {
                Output::send<LogLevel::Verbose>(STR("[Passenger] на этом сиденье стоит модуль — освободите слот (спека 3.3.3)\n"));
                Notify::push(Notify::Kind::Warn, "A car part is on that seat");
                return;
            }
            Net::send_seat(Coop::RequestSeat, {static_cast<uint8_t>(seat), 0});
            Output::send<LogLevel::Verbose>(STR("[Passenger] прошу место сзади: {} (подошёл с {} стороны)\n"), seat == 0 ? STR("левое") : STR("правое"),
                                            preferred == 0 ? STR("левой") : STR("правой"));
        }

        void host_on_request(Coop::PacketId id, const Coop::SeatBody& request)
        {
            if (id == Coop::RequestExitVehicle)
            {
                Net::send_seat(Coop::PassengerDetach, {request.Seat, Coop::SeatOk});
                Output::send<LogLevel::Verbose>(STR("[Passenger] пассажир вышел\n"));
                g_host_passenger = -1;
                return;
            }
            if (id != Coop::RequestSeat) return;
            UObject* car = Cached::car();
            Coop::SeatBody reply{request.Seat, Coop::SeatNoCar};
            if (car)
            {
                // Только та сторона, с которой подошёл пассажир (решение пользователя 2026-11-06): модуль на сиденье — отказ;
                // дверь снята — можно, открывать не нужно; дверь есть — должна быть открыта
                int seat = request.Seat == 1 ? 1 : 0;
                reply = {static_cast<uint8_t>(seat), Coop::SeatOk};
                if (!seat_free(car, seat)) reply.Result = Coop::SeatBlockedByModule;
                else if (!rear_door_passable(car, seat)) reply.Result = Coop::SeatDoorClosed;
            }
            if (car && reply.Result == Coop::SeatBlockedByModule)
            {
                // Диагностика отказа: что именно лежит в слотах сидений у Host (Client видит свою копию машины иначе — лог 2026-10-31)
                for (int seat = 0; seat < 2; ++seat)
                {
                    auto** slot = property<UObject*>(car, SeatSlot[seat]);
                    auto** item_slot = slot && *slot ? property<UObject*>(*slot, STR("ItemSlot")) : nullptr;
                    auto* items = item_slot && *item_slot ? property<RawArray>(*item_slot, STR("Items")) : nullptr;
                    Output::send<LogLevel::Verbose>(STR("[Passenger] слот {}: {}, предметов {}, {}\n"), SeatSlot[seat], slot && *slot ? STR("найден") : STR("НЕ найден"),
                                                    items ? items->Num : -1, items && items->Num > 0 ? Items::wide(Items::archetype_path(static_cast<UObject**>(items->Data)[0])) : std::wstring());
                }
            }
            Net::send_seat(Coop::PassengerAttach, reply);
            if (reply.Result == Coop::SeatOk) g_host_passenger = reply.Seat;
            Output::send<LogLevel::Verbose>(STR("[Passenger] запрос посадки → {}\n"), reply.Result == Coop::SeatOk ? STR("разрешено") : reply.Result == Coop::SeatNoCar ? STR("нет машины") : reply.Result == Coop::SeatDoorClosed ? STR("задняя дверь закрыта") : STR("оба места заняты модулями"));
        }
    } // namespace

    void set_restrict_interactions(bool on) { g_restrict = on; }

    void set_exit_side_cm(float cm) { g_exit_side_cm = cm; }

    void set_seat_offsets(float left_x, float left_y, float left_z, float right_x, float right_y, float right_z)
    {
        g_seat_offset = {left_x, left_y, left_z};
        g_seat_offset_right = {right_x, right_y, right_z};
    }

    void on_seat(Coop::PacketId id, const Coop::SeatBody& body, bool is_host)
    {
        if (is_host)
        {
            host_on_request(id, body);
            return;
        }
        if (id == Coop::PassengerAttach)
        {
            if (body.Result == Coop::SeatOk) sit(body.Seat);
            else
            {
                Output::send<LogLevel::Verbose>(STR("[Passenger] Host отказал: {}\n"), body.Result == Coop::SeatNoCar ? STR("у него нет машины") : body.Result == Coop::SeatDoorClosed ? STR("задняя дверь закрыта") : STR("задние места заняты модулями — освободите слот"));
                Notify::push(Notify::Kind::Warn, body.Result == Coop::SeatDoorClosed ? "Open the door first" : body.Result == Coop::SeatNoCar ? "The driver has no car right now" : "The rear seats are taken by car parts");
            }
        }
        else if (id == Coop::PassengerDetach && g_seat >= 0)
        {
            stand_up(L"по подтверждению Host");
        }
    }

    void on_map_loaded()
    {
        if (g_seat >= 0) g_resume_seat = g_seat; // загрузка присланной зоны сбрасывает посадку: вернёмся на то же место, когда машина Host появится
        g_seat = g_host_passenger = -1;
        g_anim.active = false;
        Presence::set_seated(false);
    }

    namespace
    {
        // Камера пассажира поворачивается вместе с машиной: каждый кадр к рысканию взгляда добавляем, на сколько повернулась машина.
        // Наклон и крен взгляда не трогаем — горизонт остаётся ровным, куда смотрит игрок внутри машины, сохраняется
        bool g_camera_follow = true;
        float g_last_car_yaw = NAN;

        struct Rotator
        {
            float Pitch, Yaw, Roll;
        };

        void camera_follow(UObject* character)
        {
            UObject* car = CarSync::puppet_car();
            UFunctionCall get_car(car, STR("K2_GetActorRotation"));
            auto* car_rot = get_car ? get_car.param<Rotator>(STR("ReturnValue")) : nullptr;
            if (!g_camera_follow || !car_rot) return;
            get_car.call();
            float yaw = car_rot->Yaw;
            float delta = std::isnan(g_last_car_yaw) ? 0.f : yaw - g_last_car_yaw;
            g_last_car_yaw = yaw;
            delta = std::remainder(delta, 360.f); // переход через ±180
            // больше 45° за кадр — не поворот, а телепорт машины (сброс, новая зона): камеру не дёргаем
            if (std::fabs(delta) < 0.001f || std::fabs(delta) > 45.f) return;
            auto** controller = property<UObject*>(character, STR("Controller"));
            if (!controller || !*controller) return;
            UFunctionCall get_view(*controller, STR("GetControlRotation"));
            auto* view = get_view ? get_view.param<Rotator>(STR("ReturnValue")) : nullptr;
            UFunctionCall set_view(*controller, STR("SetControlRotation"));
            auto* new_view = set_view ? set_view.param<Rotator>(STR("NewRotation")) : nullptr;
            if (!view || !new_view) return;
            get_view.call();
            *new_view = {view->Pitch, view->Yaw + delta, view->Roll};
            set_view.call();
        }
    } // namespace

    void set_camera_follow(bool on) { g_camera_follow = on; }

    void tick(bool is_host)
    {
        if (is_host)
        {
            g_key_pressed = false; // водитель садится штатно
            return;
        }
        static bool saved_loaded = false;
        if (!saved_loaded) // после перезапуска игры: прежнее место из файла, сядем сами, когда машина Host появится
        {
            saved_loaded = true;
            g_resume_seat = load_saved_seat();
        }
        if (g_key_pressed.exchange(false) && Net::connected()) on_key();
        if (g_seat >= 0)
        {
            if (!Net::connected() || !CarSync::puppet_car())
            {
                g_resume_seat = g_seat; // Host уехал в новую зону: как только машина появится снова — сядем на то же место сами
                stand_up(L"связь/машина Host пропали");
            }
            else if (UObject* character = own_character())
            {
                if (g_anim.active) step_anim(character);
                else follow_car(character);
                camera_follow(character);
            }
            return;
        }
        if (g_anim.active) // идёт выход через дверь: g_seat уже сброшен, но игрок ещё в пути
        {
            if (UObject* character = own_character()) step_anim(character);
            else g_anim.active = false;
        }
        g_last_car_yaw = NAN; // встал — при следующей посадке начинаем отсчёт заново
        if (g_resume_seat >= 0 && Net::connected() && CarSync::puppet_car() && own_character() && now_ms() - g_last_resume_ms > ResumeRetryMs)
        {
            g_last_resume_ms = now_ms();
            Net::send_seat(Coop::RequestSeat, {static_cast<uint8_t>(g_resume_seat), 0});
            Output::send<LogLevel::Verbose>(STR("[Passenger] машина Host снова рядом — прошу прежнее место: {}\n"), g_resume_seat == 0 ? STR("левое") : STR("правое"));
            g_resume_seat = -1; // ответ Host-а (PassengerAttach) посадит; отказ — дальше садиться вручную
        }
        // Штатный вход в машину-куклу ломает игрока (падение под карту, потеря управления) — сразу выходим штатно же
        uint64_t now = now_ms();
        if (g_describe_at && now >= g_describe_at)
        {
            g_describe_at = 0;
            log_character_state(STR("через 2 с после Exit Car"));
        }
        if (!CarSync::puppet_car() || now - g_last_guard_ms < GuardIntervalMs) return;
        g_last_guard_ms = now;
        UObject* character = own_character();
        if (!character) return;
        CarState state = character_car_state(character);
        if (!state.inside())
        {
            g_guard_attempts = 0;
            return;
        }
        // Повтор не чаще раза в секунду и не больше 5 раз: если игра не отпускает, не долбим её каждые 200 мс
        if (g_guard_attempts >= 5 || now - g_last_exit_ms < 1000) return;
        g_last_exit_ms = now;
        ++g_guard_attempts;
        log_character_state(STR("до Exit Car"));
        bool called = force_exit_car(character);
        Output::send<LogLevel::Verbose>(STR("[Passenger] штатный вход в машину Host заблокирован ({}): прикреплён={}, в машине {:.2f}, попытка {} — садитесь Ctrl+E\n"),
                                        called ? STR("Exit Car вызван") : STR("Exit Car НЕ найден"), state.Attached, state.Percent, g_guard_attempts);
        g_describe_at = now + 2000;
    }

    void describe() { log_character_state(STR("Ctrl+F6")); }
} // namespace PassengerSync
