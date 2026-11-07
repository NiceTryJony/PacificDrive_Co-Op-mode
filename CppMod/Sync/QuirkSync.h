// Причуды машины (SE_Quirk_MixAndMatch_C на скрытой детали Car.Equip.Internal.Soul, RECON.md 6.28/6.30).
// Host раз в секунду проверяет свой набор и при изменении (и раз в 10 с) шлёт его Client-у (QuirkSet).
// Client: причуды, которых у Host больше нет, лечит у себя; свою вылеченную — сообщает Host-у (QuirkCure), тот лечит у себя.
// Лечение — BP_StatusEffectManager_C.CureCarQuirk. Новые причуды не создаём: AddCarQuirk разыгрывает пару случайно.
#pragma once
#include "Networking/Protocol.h"

namespace QuirkSync
{
    void tick(bool is_host);
    void on_map_loaded();
    void on_quirk_set(const Coop::QuirkSetBody& body); // Client
    void on_quirk_cure(const Coop::QuirkBody& body);   // Host
    void on_quirk_know(const Coop::QuirkKnowBody& body); // Host: пассажир выяснил о причуде (диагноз, часть/тип условия и поведения)
} // namespace QuirkSync
