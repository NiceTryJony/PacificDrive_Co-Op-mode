// Предметы, которые игрок положил на землю (выбросил, снял деталь с машины, достал из багажника): появляются и у собеседника.
// Признак «положил игрок»: игра создаёт проявление через InventorySystem.SpawnItemInstanceActor (RECON.md 6.39), позже 15 с после
// загрузки карты и рядом с самим игроком (подгрузка уровня создаёт предметы так же, но далеко и в первые секунды).
// Создание от сети не пересылается обратно (флаг на время вызова). Подбор уже снимает LootSync (по типу и позиции).
// shortcut: катящийся предмет у собеседника может лечь дальше LootMatchRadius от точки броска — подбор тогда не снимет копию, пока не доработаем позицию
#pragma once
#include "Networking/Protocol.h"

namespace WorldItemSync
{
    void tick(bool is_host);
    void on_map_loaded();
    void on_world_item(const Coop::WorldItemBody& body, const std::string& path, const std::vector<Coop::TrunkValue>& values);
} // namespace WorldItemSync
