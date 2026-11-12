// Инвентарь игрока внутри сейва: рюкзак, рука, ячейки быстрого выбора (RECON.md «5b.8», docs/design/DECISIONS.md 2026-10-30).
// Чистый C++ без Unreal. Python-эталон: tools/sav_inventory.py.
#pragma once
#include "Game/SaveObjects.h"

#include <optional>
#include <string>
#include <vector>

namespace SaveInventory
{
    struct Inventory
    {
        std::string InventoryManager;   // BP_InventoryManager_C: ссылки Inventory_Player / Inventory_PlayerHand
        std::string CharacterManager;   // BP_MainCharacterManager_C: HotbarCursor, HotbarShortcuts
        std::vector<std::string> Shortcuts; // BP_HotbarShortcut_C по номеру ячейки
        // Переносятся целиком: контейнеры, их клетки и предметы, затем предметы ячеек, не попавшие в контейнеры (в сейвах игры ячейки ссылаются на предметы рюкзака; сирота — след наших тестов).
        // Owned[0] — контейнер рюкзака
        std::vector<std::string> Owned;
    };

    std::optional<Inventory> find(const SaveObjects::Game& game, std::string& error);

    struct Transfer
    {
        std::vector<uint8_t> Game;         // новый блок GAME (распакованный)
        std::vector<std::string> Warnings; // ссылки других объектов цели на удалённые предметы — после переноса повиснут
    };

    // Инвентарь источника вместо инвентаря цели; всё остальное — от цели. Порядок действий — как transfer() в
    // tools/sav_inventory.py: результат обязан совпасть с ним байт в байт
    std::optional<Transfer> transfer(const SaveObjects::Game& src, const SaveObjects::Game& dst, std::string& error);

    // Всё целиком: файл-источник + файл-цель → файл сейва цели с инвентарём источника. Ни при каких входных данных
    // не бросает исключений: любая ошибка — nullopt и текст в error (мод при этом грузит цель как есть)
    std::optional<std::vector<uint8_t>> transfer_files(const std::vector<uint8_t>& src_file, const std::vector<uint8_t>& dst_file,
                                                       std::string& error, std::vector<std::string>& warnings);
} // namespace SaveInventory
