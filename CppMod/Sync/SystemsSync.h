// Зажигание, свет, топливо и аккумулятор машины (RECON.md «6.9»). Только Host → Client: пассажир этим не управляет.
#pragma once
#include "Networking/Protocol.h"

namespace SystemsSync
{
    void on_systems(const Coop::SystemsBody& body); // Client, из сети (игровой поток)
    void on_connected(bool is_host);                // Host: новому клиенту — полное состояние
    void on_map_loaded();
    void tick(bool is_host);                        // игровой поток
    const Coop::SystemsBody* last();                // последнее известное состояние (Host — отправленное, Client — полученное) для окна статуса; nullptr — ещё нет
} // namespace SystemsSync
