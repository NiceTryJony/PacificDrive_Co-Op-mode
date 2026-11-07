#include "Game/SaveFile.h"

#include <algorithm>
#include <cstring>

namespace SaveFile
{
    namespace
    {
        constexpr size_t TableStart = 0x10, RowSize = 24;

        uint32_t u32(const std::vector<uint8_t>& d, size_t at)
        {
            uint32_t v;
            std::memcpy(&v, d.data() + at, 4);
            return v;
        }

        bool is_tag(const std::vector<uint8_t>& d, size_t at)
        {
            for (size_t i = 0; i < 4; ++i)
                if (uint8_t ch = d[at + i] | 0x20; ch < 'a' || ch > 'z') return false; // ASCII-буквы, как isalpha() в sav_unpack.py
            return true;
        }
    } // namespace

    Block* Container::find(const std::string& tag)
    {
        for (auto& b : Blocks)
            if (b.Tag == tag) return &b;
        return nullptr;
    }

    std::optional<std::vector<uint8_t>> lz4_decompress(const uint8_t* src, size_t size, size_t expected)
    {
        std::vector<uint8_t> out;
        out.reserve(expected);
        size_t i = 0;
        // Длина в LZ4: 15 в полубайте токена = дальше байты-добавки, пока не встретится не-255
        auto length = [&](size_t len) -> std::optional<size_t> {
            if (len != 15) return len;
            for (;;)
            {
                if (i >= size) return std::nullopt;
                uint8_t b = src[i++];
                len += b;
                if (b != 255) return len;
            }
        };
        while (i < size)
        {
            uint8_t token = src[i++];
            auto lit = length(token >> 4);
            if (!lit || *lit > size - i || out.size() + *lit > expected) return std::nullopt;
            out.insert(out.end(), src + i, src + i + *lit);
            i += *lit;
            if (i >= size) break; // последняя последовательность — только литералы
            if (size - i < 2) return std::nullopt;
            size_t offset = src[i] | (src[i + 1] << 8);
            i += 2;
            auto match = length(token & 15);
            if (!match || offset == 0 || offset > out.size()) return std::nullopt;
            size_t n = *match + 4;
            if (out.size() + n > expected) return std::nullopt;
            for (size_t k = 0, from = out.size() - offset; k < n; ++k) out.push_back(out[from + k]); // окна могут перекрываться
        }
        if (out.size() != expected) return std::nullopt;
        return out;
    }

    std::vector<uint8_t> lz4_compress(const std::vector<uint8_t>& src)
    {
        const size_t n = src.size();
        std::vector<uint8_t> out;
        out.reserve(n / 2 + 64);
        auto put_len = [&](size_t len) {
            for (; len >= 255; len -= 255) out.push_back(255);
            out.push_back(static_cast<uint8_t>(len));
        };
        // Последовательность: литералы + (кроме последней) совпадение длиной mlen >= 4 со смещением offset
        auto emit = [&](size_t from, size_t lit, size_t offset, size_t mlen) {
            out.push_back(static_cast<uint8_t>((std::min<size_t>(lit, 15) << 4) | (mlen ? std::min<size_t>(mlen - 4, 15) : 0)));
            if (lit >= 15) put_len(lit - 15);
            out.insert(out.end(), src.begin() + from, src.begin() + from + lit);
            if (!mlen) return;
            out.push_back(static_cast<uint8_t>(offset));
            out.push_back(static_cast<uint8_t>(offset >> 8));
            if (mlen - 4 >= 15) put_len(mlen - 4 - 15);
        };
        std::vector<uint32_t> table(1u << 16, 0); // позиция + 1; 0 — пусто
        size_t anchor = 0, ip = 0;
        // Правила формата: последнее совпадение начинается не позже n-12 и кончается не ближе 5 байт к концу
        if (n >= 13)
        {
            while (ip <= n - 12)
            {
                uint32_t seq;
                std::memcpy(&seq, &src[ip], 4);
                uint32_t& slot = table[(seq * 2654435761u) >> 16];
                size_t ref = slot;
                slot = static_cast<uint32_t>(ip + 1);
                uint32_t prev;
                if (ref-- && ip - ref <= 65535 && (std::memcpy(&prev, &src[ref], 4), prev == seq))
                {
                    size_t len = 4;
                    while (ip + len < n - 5 && src[ref + len] == src[ip + len]) ++len;
                    emit(anchor, ip - anchor, ip - ref, len);
                    ip += len;
                    anchor = ip;
                }
                else ++ip;
            }
        }
        emit(anchor, n - anchor, 0, 0);
        return out;
    }

    std::vector<uint8_t> write(const Container& c)
    {
        auto put = [](std::vector<uint8_t>& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
        std::vector<uint8_t> out = c.Header;
        std::vector<std::vector<uint8_t>> stored;
        std::vector<uint32_t> offsets;
        for (auto& b : c.Blocks)
        {
            stored.push_back(b.Type == 6 ? lz4_compress(b.Data) : b.Data);
            offsets.push_back(static_cast<uint32_t>(out.size()));
            out.insert(out.end(), stored.back().begin(), stored.back().end());
        }
        auto end = static_cast<uint32_t>(out.size());
        for (size_t row = TableStart; row + RowSize <= c.Header.size(); row += RowSize)
        {
            size_t k = (row - TableStart) / RowSize;
            if (k < c.Blocks.size())
            {
                // тип 6: size = usize = сжатый размер, extra = распакованный; тип 2: extra = 0 — как у игры
                auto size = static_cast<uint32_t>(stored[k].size());
                put(out, row + 4, stored[k].empty() ? end : offsets[k]);
                put(out, row + 8, size);
                put(out, row + 12, size);
                put(out, row + 16, c.Blocks[k].Type);
                put(out, row + 20, c.Blocks[k].Type == 6 ? static_cast<uint32_t>(c.Blocks[k].Data.size()) : 0);
            }
            else if (!u32(out, row) && std::any_of(out.begin() + row + 4, out.begin() + row + RowSize, [](uint8_t x) { return x != 0; }))
                put(out, row + 4, end); // пустые строки таблицы у игры указывают на конец файла
        }
        return out;
    }

    std::optional<Container> read(const std::vector<uint8_t>& file, std::string& error)
    {
        if (file.size() < TableStart + RowSize || std::memcmp(file.data(), "SAVC", 4))
        {
            error = "нет заголовка SAVC";
            return std::nullopt;
        }
        Container c;
        size_t first = u32(file, TableStart + 4); // таблица (с пустыми строками) занимает всё до первого блока
        if (first > file.size() || first < TableStart + RowSize)
        {
            error = "таблица блоков за пределами файла";
            return std::nullopt;
        }
        c.Header.assign(file.begin(), file.begin() + first);
        for (size_t row = TableStart; row + RowSize <= first && is_tag(file, row); row += RowSize)
        {
            Block b;
            b.Tag.assign(reinterpret_cast<const char*>(file.data() + row), 4);
            uint32_t offset = u32(file, row + 4), size = u32(file, row + 8), extra = u32(file, row + 20);
            b.Type = u32(file, row + 16);
            if (offset > file.size() || size > file.size() - offset)
            {
                error = b.Tag + ": блок за пределами файла";
                return std::nullopt;
            }
            const uint8_t* raw = file.data() + offset;
            if (b.Type == 6)
            {
                auto data = lz4_decompress(raw, size, extra);
                if (!data)
                {
                    error = b.Tag + ": LZ4 не распаковался";
                    return std::nullopt;
                }
                b.Data = std::move(*data);
            }
            else if (b.Type == 2) b.Data.assign(raw, raw + size);
            else
            {
                error = b.Tag + ": неизвестный вид блока " + std::to_string(b.Type);
                return std::nullopt;
            }
            c.Blocks.push_back(std::move(b));
        }
        if (!c.find("GAME"))
        {
            error = "нет блока GAME";
            return std::nullopt;
        }
        return c;
    }
} // namespace SaveFile
