// Разведка «что происходит в зоне»: каждое появление (BeginPlay) и исчезновение (EndPlay) актора во время игры — в лог
// строкой [Zone] с классом и позицией, у Host и у Client. Сравнение двух логов (tools/compare_zone.py) показывает, что игра
// каждого из двоих породила сама (аномалии, события). Ничего не пишет в игру.
#pragma once
#include <atomic>

#include "Networking/Protocol.h"

namespace ZoneTrace
{
    // Ctrl+F7 «здесь что-то не так»: метка с местом в лог обоих ПК (пакет Mark) и внеочередной срез census у обоих —
    // пользователю не нужно запоминать время
    extern std::atomic<bool> g_mark_requested;
    void on_peer_mark(const Coop::AnchorBody& at);
    // Срез собеседника (ZoneCensus): карта и «Класс=число …» без законно разных классов — сверяется со своим, расхождения в [ZoneDiff]
    void on_peer_census(const std::string& map, const std::string& text);
    void install(bool is_host); // один раз при старте мода
    void uninstall();
    void on_map_loaded();       // начальное наполнение карты не пишем: первые SettleMs после загрузки
    void tick();                // раз в 30 с: срез аномалий по классам ([Zone] … hazards), только при изменении
} // namespace ZoneTrace
