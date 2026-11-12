#include "HeapGuard.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UFunction.hpp>

#include "Game/Cached.h"
#include "Game/UFunctionCall.h"
#include "Networking/Net.h"
#include "Sync/Notify.h"

#include <chrono>
#include <cmath>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

namespace HeapGuard
{
    namespace
    {
        constexpr uint64_t PollMs = 500, DenyCooldownMs = 3000;
        constexpr float GazeDist = 400.f, GazeCos = 0.85f; // ~32°: точка актёра у пола, а свалка высокая (тест: на крышку cos 0,90); флаг гасит подсветку, поэтому «взгляд» считаем сами
        struct Vec // FVector в UE 4.27 — float
        {
            float X, Y, Z;
        };
        const TCHAR* const Classes[] = {STR("BP_Garage_LuckyJunkHeap_C"), STR("BP_WorldStation_FriendlyDumpster_C")};
        const TCHAR* const Events[] = {
                STR("BndEvt__Highlight_K2Node_ComponentBoundEvent_0_OnHighlightInteract__DelegateSignature"),
                STR("BndEvt__Highlight_K2Node_ComponentBoundEvent_1_OnHighlightInteract__DelegateSignature"),
                STR("BndEvt__Highlight_K2Node_ComponentBoundEvent_2_OnHighlightInteract__DelegateSignature"),
                STR("BndEvt__Highlight_K2Node_ComponentBoundEvent_3_OnHighlightInteract__DelegateSignature")};

        uint64_t g_poll_ms = 0, g_deny_ms = 0;
        bool g_hooked[2]{}, g_client = false; // g_client: хук знает роль без Net::is_host

        uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        void deny()
        {
            uint64_t now = now_ms();
            if (now - g_deny_ms < DenyCooldownMs) return;
            g_deny_ms = now;
            Notify::push(Notify::Kind::Warn, "Only the driver can use the junk heap");
        }

        // Игрок смотрит на актёр: камера ближе GazeDist и угол между взглядом и направлением на актёр мал
        bool looking_at(UObject* actor)
        {
            auto* ch = Cached::character();
            if (!ch) return false;
            auto** ctrl = property<UObject*>(ch, STR("Controller"));
            auto** cam = ctrl && *ctrl ? property<UObject*>(*ctrl, STR("PlayerCameraManager")) : nullptr;
            if (!cam || !*cam) return false;
            UFunctionCall gl(*cam, STR("GetCameraLocation")), gr(*cam, STR("GetCameraRotation")), al(actor, STR("K2_GetActorLocation"));
            auto *from = gl ? gl.param<Vec>(STR("ReturnValue")) : nullptr, *rot = gr ? gr.param<Vec>(STR("ReturnValue")) : nullptr,
                 *to = al ? al.param<Vec>(STR("ReturnValue")) : nullptr; // FRotator = Pitch, Yaw, Roll — те же три float
            if (!from || !rot || !to) return false;
            gl.call(), gr.call(), al.call();
            float dx = to->X - from->X, dy = to->Y - from->Y, dz = to->Z - from->Z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < 1.f || d > GazeDist) return false;
            constexpr float Rad = 3.14159265f / 180.f;
            float cp = std::cos(rot->X * Rad), fx = cp * std::cos(rot->Y * Rad), fy = cp * std::sin(rot->Y * Rad), fz = std::sin(rot->X * Rad);
            return (fx * dx + fy * dy + fz * dz) / d > GazeCos;
        }

        void install_hooks(UObject* heap)
        {
            for (auto* ev : Events)
            {
                if (!heap->GetFunctionByNameInChain(ev)) continue;
                UObjectGlobals::RegisterHook(
                        heap->GetClassPrivate()->GetPathName() + STR(":") + ev,
                        [](UnrealScriptFunctionCallableContext& ctx, void*) {
                            if (!g_client) return;
                            Output::send<LogLevel::Verbose>(STR("[Heap] Client нажал на свалку — отказ\n"));
                            deny();
                        },
                        [](UnrealScriptFunctionCallableContext&, void*) {}, nullptr);
            }
        }
    } // namespace

    void tick(bool is_host, bool test_as_client)
    {
        g_client = test_as_client || (!is_host && Net::connected());
        if (!g_client) return;
        uint64_t now = now_ms();
        if (now - g_poll_ms < PollMs) return;
        g_poll_ms = now;
        for (int c = 0; c < 2; ++c)
        {
            auto* cls = Classes[c];
            std::vector<UObject*> found;
            UObjectGlobals::FindAllOf(cls, found);
            for (auto* heap : found)
            {
                if (!g_hooked[c]) g_hooked[c] = true, install_hooks(heap);
                if (auto* allow = property<bool>(heap, STR("AllowHighlightInteraction"))) *allow = false;
                if (looking_at(heap)) deny();
            }
        }
    }
} // namespace HeapGuard
