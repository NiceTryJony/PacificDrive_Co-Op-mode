// Двери, капот и багажник машины (RECON.md «6.2»): состояние — BP_CarManager_C.DoorStates (EOpenableDoors 0..5).
// Каждый игрок опрашивает свои двери; изменение, сделанное игроком, уходит собеседнику (Host → DoorState, Client → DoorRequest).
#pragma once
#include "Networking/Protocol.h"

namespace DoorSync
{
    void on_door(Coop::PacketId id, const Coop::DoorBody& body, bool is_host); // из сети (игровой поток)
    void on_connected(bool is_host);                                           // Host: новому клиенту — полное состояние дверей
    void on_map_loaded();                                                      // сбросить «исходное состояние»
    void tick(bool is_host);                                                   // игровой поток
    int known_mask();                                                          // открытые двери (биты) для окна статуса; -1 — исходное состояние ещё не снято
} // namespace DoorSync
