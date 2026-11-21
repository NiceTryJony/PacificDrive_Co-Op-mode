// Этап 1 (спека, раздел 4): присутствие второго игрока — своя поза собеседнику и его марионетка у нас.
#pragma once
#include "Networking/Protocol.h"

namespace Presence
{
    void on_pose(const Coop::PoseBody& pose); // из сети (игровой поток)
    void on_hand(const std::string& tag, const std::string& mesh_path); // из сети: предмет в руке собеседника (путь меша, пусто — рука пуста)
    void on_map_loaded();                   // LoadMap post: старая марионетка ушла вместе с миром
    void tick(float delta_seconds, bool hand_fx, float hand_scale); // игровой поток; hand_fx — показывать работу предмета в руке собеседника (coop.ini)
    void set_seated(bool seated);             // Client: посажен модом на заднее сиденье — в позе флаг «в машине»
    bool seated();                            // то же, что задали в set_seated
    const Coop::PoseBody* mate();             // последняя поза собеседника, nullptr — нет связи или поза старше StaleMs (для HUD)
} // namespace Presence
