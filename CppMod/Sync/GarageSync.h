// Двери и ворота гаража (BP_GarageDoor_C): открыта/закрыта. Host → Client (GarageDoor) и Client → Host (GarageDoorRequest, решает Host);
// опознаются по позиции (RECON.md «6.20»).
#pragma once
#include "Networking/Protocol.h"

namespace GarageSync
{
    // Из сети (игровой поток): GarageDoor — Client, GarageDoorRequest — Host
    void on_garage_door(Coop::PacketId id, const Coop::GarageDoorBody& body, bool is_host);
    void on_connected(bool is_host); // Host: новому клиенту — все двери
    void on_map_loaded();
    void tick(bool is_host);         // игровой поток
} // namespace GarageSync
