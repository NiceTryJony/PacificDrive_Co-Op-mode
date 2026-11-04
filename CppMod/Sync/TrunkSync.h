// Содержимое багажника машины (RECON.md «6.11», docs/DECISIONS.md). Host прав: Client видит то же, что у Host;
// что пассажир взял/положил, Client сообщает Host-у (TrunkTake/TrunkPut), Host меняет свой багажник или отказывает.
#pragma once
#include <cstdint>
#include <string>

#include "Networking/Protocol.h"

namespace TrunkSync
{
    // из сети (игровой поток); values — значения свойств стопки (прочность, топливо, заряд)
    void on_packet(Coop::PacketId id, uint16_t count, const std::string& path, const std::vector<Coop::TrunkValue>& values, bool is_host);
    void on_connected(bool is_host);                                                         // Host: новому клиенту — всё содержимое
    void on_map_loaded();
    void tick(bool is_host); // игровой поток
} // namespace TrunkSync
