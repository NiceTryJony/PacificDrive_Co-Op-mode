// Якоря в основаниях (BP_Spawn_Anchor_Base_01_C: якорь — ItemInstance, флаг AnchorRemoved; RECON.md 6.28/6.30).
// Оба опрашивают свои основания; взял якорь сам (флаг поднялся или экземпляр пропал) — шлёт позицию основания (AnchorTaken).
// Получив — снимает якорь у себя: убирает экземпляр, ставит флаги, точку взятия, свет, FX, метку и объём радиации, ставит тег радара.
// AnchorTaken() основания НЕ вызывается (DECISIONS 2026-11-06). Host сам «будит» зону (HazardDisturbance) и пересылает аномалии.
#pragma once
#include "Networking/Protocol.h"

namespace AnchorSync
{
    void tick(bool is_host);
    void install(); // один раз при старте мода: перехват появления аномалий
    void on_map_loading(); // перед LoadMap: аномалии, появляющиеся при загрузке, — из сейва, их не трогаем
    void on_map_loaded();
    // AnchorTaken — якорь взят у собеседника; PortalActive — собеседник активировал портал выхода (тем же телом, по позиции)
    void on_anchor(Coop::PacketId id, const Coop::AnchorBody& body);
    // Аномалии от взятия якоря решает Host: появившиеся у него (BeginPlay) пересылает классом и местом (HazardSpawn),
    // Client свои такие убирает сразу и создаёт присланные
    void on_hazard(const std::string& where, const std::string& class_path);
} // namespace AnchorSync
