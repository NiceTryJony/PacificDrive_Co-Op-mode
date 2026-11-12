// Погода зоны (RECON.md 6.50–6.51): событие WEV_* (дождь, туман, гроза…) — статус-эффект на BP_ToDDirector_C.IA_ActiveTODEventItem,
// каждый ПК выбирает его по своему таймеру. Host опрашивает своё и шлёт (WeatherEvent); Client, если у него другое, снимает своё
// и запускает событие Host-а. Пресет времени суток приходит с сейвом зоны и совпадает сам.
#pragma once
#include <string>

namespace WeatherSync
{
    void tick(bool is_host);
    void on_map_loaded();
    // Client: тег "wev" — событие Host-а (путь класса WEV_*, пусто — нет события); "lvl" — уровни погоды Host-а (6 чисел текстом)
    void on_weather(const std::string& tag, const std::string& path);
} // namespace WeatherSync
