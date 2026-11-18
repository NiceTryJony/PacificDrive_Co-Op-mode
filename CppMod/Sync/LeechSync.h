// Пиявки BP_Hazard_Leech_* (design/LEECH_PLAN.md, DECISIONS 2026-11-17): ведёт Host, у Client — марионетки.
// Host: каждую свою пиявку (BeginPlay) номерует и шлёт LeechSpawn, 10 Гц — LeechState (место тела, при сцепке — относительно Chassis
// машины), пропала — LeechGone. Client: свои пиявки убирает сразу, присланные создаёт предметом в мире и выключает им ИИ и физику;
// карта готова — LeechHello, Host присылает всех заново. Этап 3: Client сбил (KnockOff) или убил марионетку — LeechAction, Host повторяет.
#pragma once
#include <cstddef>
#include <cstdint>

namespace LeechSync
{
    void install(); // один раз при старте мода: перехват BeginPlay пиявок
    void tick(bool is_host);
    void on_map_loading();
    void on_map_loaded();
    // пакеты (Net::on): тело после заголовка
    void on_spawn(const uint8_t* body, size_t size);
    void on_state(const uint8_t* body, size_t size);
    void on_gone(const uint8_t* body, size_t size);
    void on_hello(const uint8_t* body, size_t size);
    void on_action(const uint8_t* body, size_t size); // Host: Client сбил/убил марионетку — повторить у своей (этап 3)
} // namespace LeechSync
