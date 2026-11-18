// Этап 3 (спека 3.3, раздел 4): посадка пассажира на заднее сиденье по подтверждению Host и высадка.
#pragma once
#include "Networking/Protocol.h"

#include <atomic>

namespace PassengerSync
{
    extern std::atomic<bool> g_key_pressed; // Client: Ctrl+E — сесть/выйти (поток клавиш UE4SS → игровой поток)
    extern std::atomic<bool> g_key_driver;  // Client: Ctrl+Q — сесть за руль (место 2), если client_drive=1; выход — Ctrl+E
    bool driving();                         // Client: сидим на месте водителя
    int host_passenger_seat();              // Host: где сидит пассажир (0 — задний левый, 1 — задний правый, SeatDriver), -1 — не сидит
    void on_seat(Coop::PacketId id, const Coop::SeatBody& body, bool is_host); // из сети (игровой поток)
    void on_map_loaded();
    void set_exit_side_cm(float cm); // из coop.ini (exit_side_cm): высадка сбоку на столько см от оси машины; 0 — штатная точка игры
    void set_restrict_interactions(bool on); // из coop.ini (restrict_interactions): пассажиру доступны только двери и карта
    void set_camera_follow(bool on);         // из coop.ini (camera_follow): камера сидящего пассажира поворачивается с машиной
    // из coop.ini: левое место — seat_x/y/z, правое — seat_right_x/y/z (см от точки водителя; у правого — отражённой через ось машины)
    void set_seat_offsets(float left_x, float left_y, float left_z, float right_x, float right_y, float right_z);
    void tick(bool is_host); // игровой поток
    void describe();         // Ctrl+F6 (Diag): состояние персонажа — прикреплён, таймлайны входа/выхода, режим движения, игнор ввода
} // namespace PassengerSync
