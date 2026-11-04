#include "Game/SaveObjects.h"

#include <cstring>
#include <stdexcept>

namespace SaveObjects
{
    namespace
    {
        struct ParseError : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        struct Reader
        {
            const std::vector<uint8_t>& D;
            size_t P;

            void need(size_t n) const
            {
                if (n > D.size() - P) throw ParseError("выход за конец блока");
            }
            uint8_t u8()
            {
                need(1);
                return D[P++];
            }
            uint32_t u32()
            {
                need(4);
                uint32_t v;
                std::memcpy(&v, D.data() + P, 4);
                P += 4;
                return v;
            }
            void skip(size_t n)
            {
                need(n);
                P += n;
            }
            std::string str()
            {
                uint32_t n = u32();
                if (n == 0) return {}; // пустая строка — без \0
                need(n);
                if (D[P + n - 1] != 0) throw ParseError("строка без \\0");
                std::string s(reinterpret_cast<const char*>(D.data() + P), n - 1);
                P += n;
                return s;
            }
            void skip_str() { (void)str(); }
        };

        void skip_text(Reader& r) // FText: флаги + вид истории (как text() в sav_json.py)
        {
            r.u32();
            uint8_t h = r.u8();
            if (h == 0x0B) // строка из таблицы: таблица + ключ
            {
                r.skip_str();
                r.skip_str();
                return;
            }
            if (h != 0x04) throw ParseError("неизвестный вид FText");
            uint8_t vt = r.u8(); // AsNumber: значение, опции, культура
            if (vt > 3) throw ParseError("неизвестный тип числа в FText");
            r.skip(vt == 2 ? 4 : 8);
            if (r.u32()) r.skip(4 + 4 + 1 + 4 * 4);
            r.skip_str();
        }

        void skip_ref(Reader& r)
        {
            switch (r.u8())
            {
            case 0x00: return;
            case 0x01: r.skip_str(); r.skip(16); return; // объект сейва: класс + FGuid
            case 0x02:                                   // особый по имени (__GAMEINSTANCE__)
            case 0x03: r.skip_str(); r.skip_str(); return; // ассет: класс + путь
            default: throw ParseError("неизвестный вид ссылки");
            }
        }

        Value value(Reader& r, uint8_t type);

        void fields(Reader& r, Value& v)
        {
            uint32_t n = r.u32();
            v.Fields.reserve(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                std::string key = r.str();
                uint8_t t = r.u8();
                v.Fields.emplace_back(std::move(key), value(r, t));
            }
        }

        Value value(Reader& r, uint8_t type)
        {
            Value v;
            v.Type = type;
            size_t start = r.P;
            switch (type)
            {
            case 0x10: case 0x15: case 0x16: fields(r, v); return v;
            case 0x13:
            {
                v.ElemType = r.u8();
                uint32_t n = r.u32();
                for (uint32_t i = 0; i < n; ++i) v.Items.push_back(value(r, v.ElemType));
                return v;
            }
            case 0x14:
            {
                v.KeyType = r.u8();
                v.ElemType = r.u8();
                uint32_t nk = r.u32(), nv = r.u32();
                for (uint32_t i = 0; i < nk; ++i) v.Keys.push_back(value(r, v.KeyType));
                for (uint32_t i = 0; i < nv; ++i) v.Items.push_back(value(r, v.ElemType));
                return v;
            }
            case 0x01: case 0x02: case 0x03: r.skip(4); break;
            case 0x04: case 0x05: case 0x0E: case 0x11: case 0x19: case 0x1A: r.skip_str(); break;
            case 0x0F: case 0x18: r.skip(8); break;  // FDateTime, int64
            case 0x07: case 0x08: r.skip(12); break; // FVector, FRotator
            case 0x0C: case 0x0D: r.skip(16); break; // FLinearColor, FGuid
            case 0x0A: r.skip(40); break;            // FTransform
            case 0x17: r.skip(r.u32()); break;       // байты
            case 0x06: skip_text(r); break;
            case 0x12: skip_ref(r); break;
            default: throw ParseError("неизвестный тип значения");
            }
            v.Raw.assign(r.D.begin() + start, r.D.begin() + r.P);
            return v;
        }

        void put_u32(std::vector<uint8_t>& out, uint32_t v)
        {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            out.insert(out.end(), b, b + 4);
        }

        void put_str(std::vector<uint8_t>& out, const std::string& s)
        {
            if (s.empty())
            {
                put_u32(out, 0);
                return;
            }
            put_u32(out, static_cast<uint32_t>(s.size() + 1));
            out.insert(out.end(), s.begin(), s.end());
            out.push_back(0);
        }

        void dump(std::vector<uint8_t>& out, const Value& v)
        {
            switch (v.Type)
            {
            case 0x10: case 0x15: case 0x16:
                put_u32(out, static_cast<uint32_t>(v.Fields.size()));
                for (auto& [key, child] : v.Fields)
                {
                    put_str(out, key);
                    out.push_back(child.Type);
                    dump(out, child);
                }
                return;
            case 0x13:
                out.push_back(v.ElemType);
                put_u32(out, static_cast<uint32_t>(v.Items.size()));
                for (auto& e : v.Items) dump(out, e);
                return;
            case 0x14:
                out.push_back(v.KeyType);
                out.push_back(v.ElemType);
                put_u32(out, static_cast<uint32_t>(v.Keys.size()));
                put_u32(out, static_cast<uint32_t>(v.Items.size()));
                for (auto& k : v.Keys) dump(out, k);
                for (auto& e : v.Items) dump(out, e);
                return;
            default: out.insert(out.end(), v.Raw.begin(), v.Raw.end());
            }
        }

        bool is_hex_upper(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }

        // Запись объекта: ключ «21 00 00 00» + 32 hex + «00» + тип 0x16 (как OBJ в sav_json.py)
        bool object_at(const std::vector<uint8_t>& d, size_t i)
        {
            if (i + 38 > d.size() || d[i] != 0x21 || d[i + 1] || d[i + 2] || d[i + 3] || d[i + 36] || d[i + 37] != 0x16) return false;
            for (size_t k = 4; k < 36; ++k)
                if (!is_hex_upper(d[i + k])) return false;
            return true;
        }

        constexpr char MapKey[] = "\x09\x00\x00\x00_objects\x00\x15"; // ключ «_objects» + тип 0x15; дальше — счётчик
        constexpr size_t MapKeySize = sizeof(MapKey) - 1;
    } // namespace

    Value* Value::field(const std::string& name)
    {
        for (auto& [k, v] : Fields)
            if (k == name) return &v;
        return nullptr;
    }

    const Value* Value::field(const std::string& name) const { return const_cast<Value*>(this)->field(name); }

    const Object* Game::find(const std::string& id) const { return const_cast<Game*>(this)->find(id); }

    Object* Game::find(const std::string& id)
    {
        for (auto& o : Objects)
            if (o.Id == id) return &o;
        return nullptr;
    }

    Game parse(std::vector<uint8_t> data)
    {
        Game g;
        g.Data = std::move(data);
        const auto& d = g.Data;
        std::vector<size_t> maps, starts;
        for (size_t i = 0; i + MapKeySize <= d.size(); ++i)
        {
            if (d[i] == 0x09 && std::memcmp(d.data() + i, MapKey, MapKeySize) == 0) maps.push_back(i + MapKeySize);
            else if (object_at(d, i)) starts.push_back(i);
        }
        for (size_t k = 0; k < starts.size(); ++k)
        {
            Object o;
            o.Start = starts[k];
            o.Id.assign(reinterpret_cast<const char*>(d.data() + o.Start + 4), 32);
            for (size_t m : maps)
                if (m < o.Start) o.MapCount = m;
            try
            {
                Reader r{d, o.Start + 38};
                o.Record = value(r, 0x16);
                o.End = r.P;
                // за последним объектом карты идут другие поля — следующий объект может начаться дальше, но не раньше
                if (k + 1 < starts.size() && o.End > starts[k + 1]) throw ParseError("запись залезла на следующий объект");
                g.Objects.push_back(std::move(o));
            }
            catch (const ParseError&)
            {
                ++g.Unparsed; // BP_ProgressionManager_C (FText NamedFormat) — инвентаря не касается
            }
        }
        return g;
    }

    std::vector<uint8_t> dump_object(const Object& o)
    {
        std::vector<uint8_t> out;
        put_str(out, o.Id);
        out.push_back(0x16);
        dump(out, o.Record);
        return out;
    }

    std::string class_of(const Object& o)
    {
        const Value* c = o.Record.field("_class");
        return c ? string_of(*c) : std::string();
    }

    std::string string_of(const Value& v)
    {
        if (v.Raw.size() <= 4) return {};
        return std::string(reinterpret_cast<const char*>(v.Raw.data() + 4), v.Raw.size() - 5); // u32 длина + текст + \0
    }

    std::string guid_to_id(const uint8_t* guid)
    {
        static const char Hex[] = "0123456789ABCDEF";
        std::string id;
        for (int part = 0; part < 4; ++part)
        {
            uint32_t x;
            std::memcpy(&x, guid + part * 4, 4);
            for (int shift = 28; shift >= 0; shift -= 4) id += Hex[(x >> shift) & 15];
        }
        return id;
    }

    std::vector<uint8_t> id_to_guid(const std::string& id)
    {
        std::vector<uint8_t> g(16);
        for (int part = 0; part < 4; ++part)
        {
            uint32_t x = std::stoul(id.substr(part * 8, 8), nullptr, 16);
            std::memcpy(g.data() + part * 4, &x, 4);
        }
        return g;
    }

    std::string ref_id(const Value& v)
    {
        // вид 01: класс (u32 длина + текст) + 16 байт FGuid — GUID в конце
        if (v.Type != 0x12 || v.Raw.size() < 1 + 4 + 16 || v.Raw[0] != 0x01) return {};
        return guid_to_id(v.Raw.data() + v.Raw.size() - 16);
    }

    std::string asset_path(const Value& v)
    {
        if (v.Type != 0x12 || v.Raw.empty() || v.Raw[0] != 0x03) return {};
        uint32_t n;
        std::memcpy(&n, v.Raw.data() + 1, 4);
        size_t path = 1 + 4 + n; // за классом — путь (u32 длина + текст + \0)
        if (path + 4 >= v.Raw.size()) return {};
        return std::string(reinterpret_cast<const char*>(v.Raw.data() + path + 4), v.Raw.size() - path - 5);
    }
} // namespace SaveObjects
