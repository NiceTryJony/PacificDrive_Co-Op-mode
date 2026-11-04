// Этап 2 (спека, раздел 4): Host ездит, Client видит движение машины. Host шлёт CarState, у Client машина — кинематическая кукла.
#pragma once
#include "Networking/Protocol.h"

namespace RC::Unreal { class UObject; }
using UObjectPtr = RC::Unreal::UObject*;

namespace CarSync
{
    void on_car_state(const Coop::CarStateBody& state); // Client: из сети (игровой поток)
    void on_map_loaded();
    void tick(float delta_seconds, bool is_host); // игровой поток
    UObjectPtr puppet_car(); // Client: машина, которую сейчас ведёт Host (иначе nullptr)
    // Client: где кукла поставлена в этом кадре (X..Roll; скорость не заполняется). false — куклы нет
    bool puppet_pose(Coop::CarStateBody& out);
} // namespace CarSync
