// Разведка «что происходит в зоне»: каждое появление (BeginPlay) и исчезновение (EndPlay) актора во время игры — в лог
// строкой [Zone] с классом и позицией, у Host и у Client. Сравнение двух логов (tools/compare_zone.py) показывает, что игра
// каждого из двоих породила сама (аномалии, события). Ничего не пишет в игру.
#pragma once

namespace ZoneTrace
{
    void install(bool is_host); // один раз при старте мода
    void uninstall();
    void on_map_loaded();       // начальное наполнение карты не пишем: первые SettleMs после загрузки
    void tick();                // раз в 30 с: срез аномалий по классам ([Zone] … hazards), только при изменении
} // namespace ZoneTrace
