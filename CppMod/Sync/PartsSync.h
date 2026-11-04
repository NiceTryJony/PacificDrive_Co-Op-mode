// Детали машины: что стоит в слотах (двери, панели, колёса, двигатель, навесное), этап 4 спеки (CarDetailedState, «модули»).
// Host → Client: опрос слотов (CarPart). Client → Host: пассажир сменил деталь на своей копии — запрос (CarPartRequest), решает Host.
// Слот = SlotSceneComponent машины, сам меняет видимую деталь по своему ItemSlot (RECON.md «6.10»).
#pragma once
#include <string>

namespace PartsSync
{
    void on_part(const std::string& tag, const std::string& path);         // Client, из сети (игровой поток)
    void on_part_request(const std::string& tag, const std::string& path); // Host, из сети: Client сменил деталь
    void set_client_parts(bool on);                                         // из coop.ini (client_parts); false — запрет, правки Client откатываются
    void on_connected(bool is_host);                                        // Host: новому клиенту — все слоты
    void on_map_loaded();
    void tick(bool is_host); // игровой поток
} // namespace PartsSync
