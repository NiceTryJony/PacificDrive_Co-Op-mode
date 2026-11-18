// Данные для HUD (tools/PDCoopRecon/Scripts/hud.lua): coop/hud.txt, key=value, до 5 раз в секунду и только при изменении.
// Формат — docs/design/HUD_PLAN.md, шаг 1.1.
#pragma once
#include <filesystem>

namespace HudFeed
{
    void tick(const std::filesystem::path& file, bool host); // игровой поток, каждый кадр (дорогое — не чаще 200 мс)
    void config_changed();                                    // coop.ini изменился — cfg_rev++ (HUD перечитает настройки)
} // namespace HudFeed
