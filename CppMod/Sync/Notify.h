// Уведомления для HUD (tools/PDCoopRecon/Scripts/hud.lua): строки в coop/notify.txt, Lua читает только дописанное.
// Формат строки: "<kind><text>", kind: пусто = info, '!' = warning, '-' = error. Текст — ASCII (HUD на английском).
#pragma once
#include <filesystem>
#include <string>

namespace Notify
{
    enum class Kind { Info, Warn, Error };

    void init(const std::filesystem::path& file); // обнуляет файл: старые события новой сессии не нужны
    void push(Kind kind, const std::string& text); // игровой поток
    // События по смене состояния: подключение/отключение и двери (doors — DoorSync::known_mask(), -1 — ещё не снято). Игровой поток
    void tick(bool connected, bool host, int doors);
} // namespace Notify
