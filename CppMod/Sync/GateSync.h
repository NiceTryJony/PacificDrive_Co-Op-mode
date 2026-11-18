// Ворота выезда гаража (docs/recon/reference/route-selection.md, «Что открывает ворота гаража»): переднюю дверь открывает директор игры
// BP_Director_GSS_GarageFrontDoor, когда в текущем маршруте есть отрезок со статусом раскрытия 2 (Explicit). Статус ставит выбор места на
// карте — у того, кто выбирал (Host). Host опрашивает статусы отрезков и шлёт раскрытые (GateState, тег "legs", «индекс RouteShapeLeg:статус»
// через запятую) при смене и раз в 10 с; Client вызывает RevealLeg на отрезке с тем же индексом, если у него статус ниже.
// Место выбрал Client: он шлёт тем же пакетом отрезки, раскрытые у него выше, чем у Host; Host применяет RevealLeg и рассылает новое состояние.
#pragma once
#include <string>

namespace GateSync
{
    void tick(bool is_host);
    void on_map_loaded();
    void on_gate(const std::string& tag, const std::string& value); // тег "legs", значение "7:2,2:1,8:1"
} // namespace GateSync
