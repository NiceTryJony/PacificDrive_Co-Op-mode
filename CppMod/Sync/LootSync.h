// Этап 5 (спека 3.4): подобранный одним игроком предмет мира исчезает у второго; при споре прав Host.
#pragma once
#include "Networking/Protocol.h"

#include <atomic>

namespace LootSync
{
    void on_loot(Coop::PacketId id, const Coop::LootBody& body, bool is_host); // из сети (игровой поток)
    void on_map_loaded();
    void tick(bool is_host); // игровой поток
    // Host перед сохранением зоны: ящики разыгрывают «предметы при открытии» (OnRollForOpenItems) сейчас, чтобы содержимое
    // попало в сейв и у Client оказалось тем же; иначе каждый разыграет своё при открытии
    void preroll_containers();
    // Отладка на одном ПК (Ctrl+F12): «собеседник забрал ближайший ко мне предмет» — тот же путь, что пакет LootPicked
    extern std::atomic<bool> g_simulate_peer_pick;
} // namespace LootSync
