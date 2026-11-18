// Client за рулём (docs/design/DECISIONS.md 2026-11-13): Client шлёт Host-у оси газа и руля (пакет DriveInput), Host подменяет ими ось машины.
// Подмена: перехват AccelerateBrake (газ) и свой вызов SteerLeftRight каждый тик (руль). Те же значения можно задать вручную в coop.ini
// (drive_probe_throttle / drive_probe_steer) — это проба механизма; сетевой ввод, пока он свежий, их перекрывает.
#pragma once
#include <cstddef>
#include <cstdint>

namespace DriveProbe
{
    void tick(float throttle, float steer);                 // Host, игровой поток: подставить ввод в машину
    void on_input(const uint8_t* body, size_t size);        // Host: пришёл DriveInput (игровой поток, из Net::tick)
    void on_action(const uint8_t* body, size_t size);       // Host: пришло DriveAction — вызвать то же действие у своей машины
    void suppress_actions(bool on);                         // Client: не пересылать вызовы, которые мы сами делаем (SystemsSync применяет состояние Host)
    void client_tick(bool enabled);                        // Client, игровой поток: enabled = client_drive и сидим в машине; слать оси Host-у
} // namespace DriveProbe
