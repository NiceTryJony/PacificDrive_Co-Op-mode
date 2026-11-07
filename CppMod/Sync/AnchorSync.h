// Якоря в основаниях (BP_Spawn_Anchor_Base_01_C: якорь — ItemInstance, флаг AnchorRemoved; RECON.md 6.28/6.30).
// Оба опрашивают свои основания; взял якорь сам (флаг поднялся или экземпляр пропал) — шлёт позицию основания (AnchorTaken).
// Получив — снимает якорь у себя: убирает экземпляр, ставит флаг и вызывает AnchorTaken() основания. У Host это же
// «будит» зону (аномалии от взятия якоря) — Host решает, что появится; передача этих аномалий Client-у — следующий шаг.
#pragma once
#include "Networking/Protocol.h"

namespace AnchorSync
{
    void tick(bool is_host);
    void on_map_loaded();
    void on_anchor(const Coop::AnchorBody& body);
} // namespace AnchorSync
