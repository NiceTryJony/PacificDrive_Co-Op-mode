#pragma once
// Этап входа Client для карточки в HUD (status.txt stage=). Чистая функция: порядок проверок держит тест CoopProbe selftest-gate.
namespace JoinStage
{
    struct In
    {
        bool version_mismatch = false; // Host отклонил: версии мода не совпадают
        bool ready = true;             // карта уже загружалась (игра стартовала)
        bool loading = false;          // UserLoadGame вызван, карта ещё грузится
        bool connected = true;
        bool was_connected = false;    // связь уже была в этой игре
        bool receiving = false;        // идёт приём сейва зоны
        bool restoring = false;        // восстанавливается инвентарь
        bool world_synced = true;      // присланная зона загружена в этом подключении
    };

    inline const char* of(const In& in)
    {
        if (in.version_mismatch) return "version";
        if (!in.ready) return "start";
        if (in.loading) return "build";
        if (!in.connected) return in.was_connected ? "lost" : "search";
        if (in.receiving) return "recv";
        if (in.restoring) return "inv";
        if (!in.world_synced) return "wait";
        return "live";
    }
} // namespace JoinStage
