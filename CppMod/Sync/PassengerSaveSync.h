// Хранение инвентаря пассажира у Host (спека 3.4.4, docs/sync/SAVES.md, пакеты 26/28).
// Client шлёт Host-у свой кооп-слот, когда игра его сохранила; Host кладёт копию в coop\passenger.sav. Если у Client слота нет
// (другой ПК, слот удалён), он при подключении просит копию обратно — инвентарь переносится в зону из неё (ZoneLoad).
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

#include "Networking/Net.h"

namespace PassengerSaveSync
{
    void init(const Net::Events* events, const std::filesystem::path& stored_file);
    void on_connected(bool is_host);
    void on_save(std::vector<uint8_t>&& file, bool is_host);           // из сети: Host — сейв пассажира, Client — копия от Host-а
    void on_request(const Coop::SaveRequestBody& body, bool is_host);  // из сети
    // slot_file() — путь кооп-слота Client или пустой, если слота ещё нет; вызывается не чаще раза в 2 с
    void tick(bool is_host, std::filesystem::path (*slot_file)());
    bool restore_pending();                // Client: ждём ответ Host-а — загрузку зоны пока не начинать (не дольше 20 с)
    const std::filesystem::path& stored_file(); // coop\passenger.sav
} // namespace PassengerSaveSync
