// Славная свалка (BP_Garage_LuckyJunkHeap_C / BP_WorldStation_FriendlyDumpster_C): приз разыгрывается случайно там, где её используют,
// поэтому Client пользоваться ею не может — только Host. Client: гасим AllowHighlightInteraction (свалка не реагирует на E),
// а если взаимодействие всё же дошло до игры — HUD-уведомление и анимация отказа (DeniedTimeline).
#pragma once

namespace HeapGuard
{
    void tick(bool is_host, bool test_as_client); // игровой поток
} // namespace HeapGuard
