// Ворота выезда гаража (docs/recon/reference/route-selection.md): состояние держит глобальный переключатель GSS_Garage_ExitGateOpened
// (в сейв не пишется — у Client после загрузки всегда «закрыто», выбор места на карте его не включает). Оба ПК опрашивают переключатель
// и шлют собеседнику, когда он сменился у них самих (GateState, оба направления); получатель ставит такое же через SetGlobalSwitch.
// Host дополнительно досылает значение раз в 10 с — для подключившегося позже Client-а.
#pragma once
#include <string>

namespace GateSync
{
    void tick(bool is_host);
    void on_map_loaded();
    void on_gate(const std::string& tag, const std::string& value); // тег "gate", значение "1" (открыто) / "0" (закрыто)
} // namespace GateSync
