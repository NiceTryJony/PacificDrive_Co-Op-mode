// Этап 1 (спека, раздел 4): присутствие второго игрока — своя поза собеседнику и его марионетка у нас.
#pragma once
#include "Networking/Protocol.h"

namespace Presence
{
    void on_pose(const Coop::PoseBody& pose); // из сети (игровой поток)
    void on_map_loaded();                     // LoadMap post: старая марионетка ушла вместе с миром
    void tick(float delta_seconds);           // игровой поток
    void set_seated(bool seated);             // Client: посажен модом на заднее сиденье — в позе флаг «в машине»
} // namespace Presence
