#include "Game/SaveInventory.h"

#include "Game/SaveFile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace SaveInventory
{
    namespace
    {
        using SaveObjects::Game;
        using SaveObjects::Object;
        using SaveObjects::Value;

        bool ends_with(const std::string& s, const std::string& tail)
        {
            return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
        }

        // Единственный объект класса (по окончанию пути класса); "" — нет или несколько
        std::string one(const Game& g, const std::string& cls)
        {
            std::string found;
            for (auto& o : g.Objects)
                if (ends_with(SaveObjects::class_of(o), cls))
                {
                    if (!found.empty()) return {};
                    found = o.Id;
                }
            return found;
        }

        const Value* data_field(const Game& g, const std::string& id, const std::string& name)
        {
            const Object* o = g.find(id);
            const Value* d = o ? o->Record.field("_data") : nullptr;
            return d ? d->field(name) : nullptr;
        }

        bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

        void set_data(Value& record, const std::string& key, const Value& val) // как set_data() в sav_inventory.py
        {
            Value& d = *record.field("_data");
            if (Value* f = d.field(key)) *f = val;
            else d.Fields.emplace_back(key, val);
        }

        bool has_bytes(const uint8_t* begin, const uint8_t* end, const std::vector<uint8_t>& needle)
        {
            return std::search(begin, end, needle.begin(), needle.end()) != end;
        }

        void replace_all(std::vector<uint8_t>& bytes, const std::vector<uint8_t>& from, const std::vector<uint8_t>& to) // равной длины
        {
            for (auto it = std::search(bytes.begin(), bytes.end(), from.begin(), from.end()); it != bytes.end();
                 it = std::search(it + from.size(), bytes.end(), from.begin(), from.end()))
                std::copy(to.begin(), to.end(), it);
        }

        // ID из массива ссылок (Slots, Items, Shortcuts); поля нет — пусто
        std::vector<std::string> ref_ids(const Game& g, const std::string& id, const std::string& name)
        {
            std::vector<std::string> out;
            if (const Value* a = data_field(g, id, name))
                for (auto& e : a->Items) out.push_back(SaveObjects::ref_id(e));
            return out;
        }
    } // namespace

    std::optional<Inventory> find(const Game& g, std::string& error)
    {
        Inventory inv;
        inv.InventoryManager = one(g, ".BP_InventoryManager_C");
        inv.CharacterManager = one(g, ".BP_MainCharacterManager_C");
        if (inv.InventoryManager.empty() || inv.CharacterManager.empty())
        {
            error = "нет (или не один) BP_InventoryManager_C / BP_MainCharacterManager_C";
            return std::nullopt;
        }
        for (const char* name : {"Inventory_Player", "Inventory_PlayerHand"})
        {
            const Value* ref = data_field(g, inv.InventoryManager, name);
            std::string container = ref ? SaveObjects::ref_id(*ref) : std::string();
            if (container.empty() || !g.find(container))
            {
                error = std::string("нет контейнера ") + name;
                return std::nullopt;
            }
            inv.Owned.push_back(container);
            for (auto& slot : ref_ids(g, container, "Slots"))
            {
                inv.Owned.push_back(slot);
                for (auto& item : ref_ids(g, slot, "Items")) inv.Owned.push_back(item);
            }        }
        const Value* hotbar_ref = data_field(g, inv.CharacterManager, "HotbarShortcuts");
        std::string hotbar = hotbar_ref ? SaveObjects::ref_id(*hotbar_ref) : std::string();
        if (hotbar.empty())
        {
            error = "нет HotbarShortcuts";
            return std::nullopt;
        }
        inv.Shortcuts = ref_ids(g, hotbar, "Shortcuts");
        for (auto& s : inv.Shortcuts) // ячейка в сейве игры ссылается на предмет рюкзака; сирот (след проб) подбираем, чтобы не оставить висеть (RECON.md «5b.13»)
            if (const Value* item = data_field(g, s, "ShortcutItem"))
                if (auto id = SaveObjects::ref_id(*item); !id.empty() && std::find(inv.Owned.begin(), inv.Owned.end(), id) == inv.Owned.end())
                    inv.Owned.push_back(id);
        // Висящие ссылки здесь не ошибка (у цели переноса их просто нечего удалять) — источник проверяет перенос
        std::erase(inv.Owned, std::string());
        return inv;
    }

    std::optional<std::vector<uint8_t>> transfer_files(const std::vector<uint8_t>& src_file, const std::vector<uint8_t>& dst_file,
                                                       std::string& error, std::vector<std::string>& warnings)
    {
        try
        {
            auto src_save = SaveFile::read(src_file, error);
            if (!src_save) return error = "источник: " + error, std::nullopt;
            auto dst_save = SaveFile::read(dst_file, error);
            if (!dst_save) return error = "цель: " + error, std::nullopt;
            auto src = SaveObjects::parse(src_save->find("GAME")->Data);
            auto dst = SaveObjects::parse(dst_save->find("GAME")->Data);
            auto result = transfer(src, dst, error);
            if (!result) return std::nullopt;
            warnings = std::move(result->Warnings);
            dst_save->find("GAME")->Data = std::move(result->Game);
            return SaveFile::write(*dst_save);
        }
        catch (const std::exception& e) // повреждённый сейв не должен ронять игру
        {
            error = std::string("исключение: ") + e.what();
            return std::nullopt;
        }
    }

    static std::optional<Transfer> transfer_impl(const Game& src, const Game& dst, std::string& error)
    {
        auto s = find(src, error);
        if (!s) return error = "источник: " + error, std::nullopt;
        auto d = find(dst, error);
        if (!d) return error = "цель: " + error, std::nullopt;
        for (auto& i : s->Owned)
            if (!src.find(i)) return error = "в источнике нет объекта " + i, std::nullopt;
        size_t home = dst.find(d->Owned[0])->MapCount; // карта _objects, где у цели лежит рюкзак
        for (auto& i : d->Owned)
            if (auto* o = dst.find(i); o && o->MapCount != home) return error = "объекты инвентаря цели в разных картах _objects", std::nullopt;
        for (auto& i : s->Owned)
            // shortcut: при совпадении ID с чужим объектом цели — стоп; выдавать новые ID, если встретится на деле
            if (dst.find(i) && !contains(d->Owned, i)) return error = "ID предмета источника занят в цели: " + i, std::nullopt;

        const Value& s_mgr = *src.find(s->InventoryManager)->Record.field("_data");
        const Value& d_mgr = *dst.find(d->InventoryManager)->Record.field("_data");
        const char* names[] = {"Inventory_Player", "Inventory_PlayerHand"};
        // Контейнеры игрока упоминаются и в других объектах (ItemShoppingList.Containers) — переводим на контейнеры источника
        std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> remap;
        std::vector<std::string> remapped;
        for (const char* name : names)
            if (auto old_c = SaveObjects::ref_id(*d_mgr.field(name)), new_c = SaveObjects::ref_id(*s_mgr.field(name)); old_c != new_c)
            {
                remap.emplace_back(SaveObjects::id_to_guid(old_c), SaveObjects::id_to_guid(new_c));
                remapped.push_back(old_c);
            }
        std::vector<std::string> gone; // удаляются без замены с тем же ID
        for (auto& i : d->Owned)
            if (!contains(s->Owned, i) && !contains(remapped, i)) gone.push_back(i);

        // Новые записи: объекты источника и изменённые объекты цели
        std::vector<std::pair<std::string, Value>> records;
        auto record = [&](const std::string& id) -> Value* {
            for (auto& [k, v] : records)
                if (k == id) return &v;
            return nullptr;
        };
        for (auto& i : s->Owned) records.emplace_back(i, src.find(i)->Record);
        Value mgr = dst.find(d->InventoryManager)->Record;
        std::string s_containers[] = {s->Owned[0], SaveObjects::ref_id(*s_mgr.field(names[1]))};
        for (int k = 0; k < 2; ++k)
        {
            // контейнер принадлежит менеджеру цели
            *record(s_containers[k])->field("_outer") = *dst.find(SaveObjects::ref_id(*d_mgr.field(names[k])))->Record.field("_outer");
            set_data(mgr, names[k], *s_mgr.field(names[k]));
        }
        records.emplace_back(d->InventoryManager, mgr);
        Value ch = dst.find(d->CharacterManager)->Record;
        if (const Value* cursor = src.find(s->CharacterManager)->Record.field("_data")->field("HotbarCursor")) set_data(ch, "HotbarCursor", *cursor);
        records.emplace_back(d->CharacterManager, ch);

        // Важные (сюжетные) предметы: остаются те, что у цели не удалены, плюс пришедшие с инвентарём источника
        std::string crit = one(dst, ".CriticalItemController"), crit_done;
        if (!crit.empty())
            if (const Value* list = dst.find(crit)->Record.field("_data")->field("CriticalItemInstances"))
            {
                Value refs;
                refs.Type = 0x13;
                refs.ElemType = 0x12;
                for (auto& r : list->Items)
                    if (!contains(gone, SaveObjects::ref_id(r))) refs.Items.push_back(r);
                if (std::string s_crit = one(src, ".CriticalItemController"); !s_crit.empty())
                    if (const Value* s_list = src.find(s_crit)->Record.field("_data")->field("CriticalItemInstances"))
                        for (auto& r : s_list->Items)
                            if (contains(s->Owned, SaveObjects::ref_id(r)) &&
                                std::none_of(refs.Items.begin(), refs.Items.end(), [&](const Value& x) { return x.Raw == r.Raw; }))
                                refs.Items.push_back(r);
                Value c = dst.find(crit)->Record;
                set_data(c, "CriticalItemInstances", refs);
                records.emplace_back(crit, c);
                crit_done = crit;
            }
        for (size_t k = 0; k < d->Shortcuts.size(); ++k) // ячейки цели по номеру получают привязки источника
        {
            Value sc = dst.find(d->Shortcuts[k])->Record;
            sc.field("_data")->Fields =
                k < s->Shortcuts.size() ? src.find(s->Shortcuts[k])->Record.field("_data")->Fields : decltype(Value::Fields){};
            records.emplace_back(d->Shortcuts[k], sc);
        }

        auto keep = [&](const std::string& id) {
            return !contains(d->Owned, id) && id != d->InventoryManager && id != d->CharacterManager && !contains(d->Shortcuts, id) &&
                   id != crit_done;
        };
        struct Cut
        {
            size_t Start, End;
            std::vector<uint8_t> Bytes;
        };
        std::vector<Cut> cuts;
        Transfer result;
        for (auto& o : dst.Objects)
        {
            if (!keep(o.Id)) continue;
            const uint8_t *b = dst.Data.data() + o.Start, *e = dst.Data.data() + o.End;
            std::vector<uint8_t> rec(b, e);
            for (auto& [from, to] : remap) replace_all(rec, from, to); // GUID той же длины — байты как есть
            if (!std::equal(rec.begin(), rec.end(), b)) cuts.push_back({o.Start, o.End, std::move(rec)});
            for (auto& g : gone)
                if (has_bytes(b, e, SaveObjects::id_to_guid(g)))
                    result.Warnings.push_back("на удалённый объект цели " + g + " ссылается " + o.Id + " (" + SaveObjects::class_of(o) + ")");
        }

        // Сборка: удалить инвентарь цели, на место первого удалённого — объекты источника; поправить счётчик карты
        size_t removed = 0, insert_at = SIZE_MAX;
        for (auto& i : d->Owned)
            if (auto* o = dst.find(i))
            {
                cuts.push_back({o->Start, o->End, {}});
                ++removed;
                insert_at = std::min(insert_at, o->Start);
            }
        for (auto& [id, rec] : records)
            if (auto* o = dst.find(id); o && !contains(s->Owned, id))
                cuts.push_back({o->Start, o->End, SaveObjects::dump_object({id, 0, 0, 0, rec})});
        std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.Start < b.Start; });

        auto& out = result.Game;
        size_t p = 0;
        int64_t shift = 0; // сдвиг счётчика карты home от правок до него
        for (auto& c : cuts)
        {
            out.insert(out.end(), dst.Data.begin() + p, dst.Data.begin() + c.Start);
            if (c.Start == insert_at)
                for (auto& i : s->Owned)
                {
                    auto bytes = SaveObjects::dump_object({i, 0, 0, 0, *record(i)});
                    out.insert(out.end(), bytes.begin(), bytes.end());
                }
            out.insert(out.end(), c.Bytes.begin(), c.Bytes.end());
            if (c.Start < home) shift += static_cast<int64_t>(c.Bytes.size()) - static_cast<int64_t>(c.End - c.Start);
            p = c.End;
        }
        out.insert(out.end(), dst.Data.begin() + p, dst.Data.end());
        size_t pos = static_cast<size_t>(static_cast<int64_t>(home) + shift); // вставка всегда после счётчика своей карты
        uint32_t count;
        std::memcpy(&count, out.data() + pos, 4);
        count = static_cast<uint32_t>(static_cast<int64_t>(count) + static_cast<int64_t>(s->Owned.size()) - static_cast<int64_t>(removed));
        std::memcpy(out.data() + pos, &count, 4);
        return result;
    }

    // Ссылки на объект (16 байт FGuid) заменяем во всех значениях, где они лежат (ссылка — 0x12, но проще и надёжнее по байтам)
    static void rename_refs(SaveObjects::Value& v, const std::vector<uint8_t>& from, const std::vector<uint8_t>& to)
    {
        replace_all(v.Raw, from, to);
        for (auto& [name, f] : v.Fields) rename_refs(f, from, to);
        for (auto& k : v.Keys) rename_refs(k, from, to);
        for (auto& i : v.Items) rename_refs(i, from, to);
    }

    // Сейвы двух ПК из одной «родословной» делят ID объектов: предмет, который Host переложил (например, в багажник), у Client
    // всё ещё в рюкзаке под тем же ID. Такие предметы источника получают новые ID, иначе перенос невозможен и Client остаётся
    // с рюкзаком Host (тест 2026-10-30 19:24). Результат тогда НЕ совпадает с Python-эталоном — он на этих входах отказывает.
    std::optional<Transfer> transfer(const Game& src, const Game& dst, std::string& error)
    {
        auto s = find(src, error);
        if (!s) return error = "источник: " + error, std::nullopt;
        auto d = find(dst, error);
        if (!d) return error = "цель: " + error, std::nullopt;
        std::vector<std::pair<std::string, std::string>> renames; // старый ID → новый
        for (auto& i : s->Owned)
            if (dst.find(i) && !contains(d->Owned, i)) renames.emplace_back(i, std::string());
        if (renames.empty()) return transfer_impl(src, dst, error);

        Game fixed = src;
        for (size_t k = 0; k < renames.size(); ++k)
        {
            auto& [old_id, new_id] = renames[k];
            auto taken = [&](const std::string& id) { // свободен: нет ни у источника, ни у цели, ни среди уже выданных (до k)
                return src.find(id) || dst.find(id) ||
                       std::any_of(renames.begin(), renames.begin() + static_cast<std::ptrdiff_t>(k), [&](const auto& r) { return r.second == id; });
            };
            new_id = old_id;
            do // последние 8 hex-цифр +1, пока ID не станет свободным
            {
                uint32_t tail = static_cast<uint32_t>(std::stoul(new_id.substr(24), nullptr, 16)) + 1;
                char buf[9];
                std::snprintf(buf, sizeof(buf), "%08X", tail);
                new_id.replace(24, 8, buf);
            } while (taken(new_id));
            auto from = SaveObjects::id_to_guid(old_id), to = SaveObjects::id_to_guid(new_id);
            for (auto& o : fixed.Objects)
            {
                if (o.Id == old_id) o.Id = new_id;
                rename_refs(o.Record, from, to);
            }
        }
        auto result = transfer_impl(fixed, dst, error);
        if (result) result->Warnings.push_back("новые ID у предметов источника из-за совпадения с объектами цели: " + std::to_string(renames.size()));
        return result;
    }
} // namespace SaveInventory
