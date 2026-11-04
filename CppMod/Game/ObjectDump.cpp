#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include "Game/ObjectDump.h"
#include "Game/ItemProps.h"
#include "Game/Items.h"
#include "Game/UFunctionCall.h"

#include <excpt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace RC;
using namespace RC::Unreal;

// Обход свойств объекта по рефлексии. Падение F4 на гаражном хранилище (RECON.md «6.20») показало, что слепой обход опасен, поэтому
// лимиты жёсткие: глубина структур, элементов в массиве, всего значений на снимок. Ссылки на объекты не раскрываем (только путь),
// так что циклов быть не может. Живые свойства предметов (IP_*) лежат не в полях — их читает ItemProps, здесь их нет.
namespace ObjectDump
{
    namespace
    {
        constexpr int MaxDepth = 3;
        constexpr int MaxArray = 64;
        constexpr int MaxValues = 3000;
        constexpr int MaxString = 256;

        struct Ctx
        {
            std::string out;
            int budget = MaxValues;
            bool truncated = false;
        };

        // Указатель можно читать (SEH: функция без объектов с деструкторами, иначе __try не компилируется)
        bool readable(const void* p)
        {
            if (!p) return false;
            __try
            {
                volatile char c = *static_cast<const volatile char*>(p);
                (void)c;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        void append_string(std::string& out, const StringType& text)
        {
            out += '"';
            size_t count = 0;
            for (auto c : text)
            {
                if (++count > MaxString)
                {
                    out += "...";
                    break;
                }
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else if (c < 0x20 || c > 0x7e) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else out += static_cast<char>(c);
            }
            out += '"';
        }

        // Имена полей BP-структур: «Durability_5_448C…32 символа» → «Durability» (как ItemProps::clean_field_name)
        std::string clean_name(const std::string& name)
        {
            auto guid = name.rfind('_');
            if (guid == std::string::npos || name.size() - guid - 1 != 32) return name;
            auto num = name.rfind('_', guid - 1);
            return num == std::string::npos ? name : name.substr(0, num);
        }

        void dump_struct(Ctx& ctx, UStruct* type, uint8_t* base, int depth, EFieldIterationFlags flags);

        void dump_ref(Ctx& ctx, UObject* object)
        {
            if (!object) ctx.out += "null";
            else if (!readable(object) || !readable(object->GetClassPrivate())) ctx.out += "\"<нечитаемо>\"";
            else
            {
                ctx.out += "{\"ref\":";
                append_string(ctx.out, object->GetFullName());
                ctx.out += '}';
            }
        }

        template <typename T>
        void put_number(Ctx& ctx, const uint8_t* at)
        {
            T value;
            std::memcpy(&value, at, sizeof(T));
            ctx.out += std::format("{}", value);
        }

        void dump_value(Ctx& ctx, FProperty* p, uint8_t* at, int depth)
        {
            if (--ctx.budget <= 0)
            {
                ctx.truncated = true;
                ctx.out += "\"...\"";
                return;
            }
            if (!readable(at))
            {
                ctx.out += "\"<нечитаемо>\"";
                return;
            }
            if (auto* b = CastField<FBoolProperty>(p)) ctx.out += (at[b->GetByteOffset()] & b->GetFieldMask()) ? "true" : "false";
            else if (CastField<FFloatProperty>(p)) put_number<float>(ctx, at);
            else if (CastField<FDoubleProperty>(p)) put_number<double>(ctx, at);
            else if (CastField<FIntProperty>(p)) put_number<int32_t>(ctx, at);
            else if (CastField<FInt64Property>(p)) put_number<int64_t>(ctx, at);
            else if (CastField<FUInt32Property>(p)) put_number<uint32_t>(ctx, at);
            else if (CastField<FUInt64Property>(p)) put_number<uint64_t>(ctx, at);
            else if (CastField<FInt16Property>(p)) put_number<int16_t>(ctx, at);
            else if (CastField<FUInt16Property>(p)) put_number<uint16_t>(ctx, at);
            else if (CastField<FInt8Property>(p)) put_number<int8_t>(ctx, at);
            else if (CastField<FByteProperty>(p)) put_number<uint8_t>(ctx, at);
            else if (CastField<FNameProperty>(p)) append_string(ctx.out, reinterpret_cast<FName*>(at)->ToString());
            else if (CastField<FObjectProperty>(p)) dump_ref(ctx, *reinterpret_cast<UObject**>(at));
            else if (auto* s = CastField<FStructProperty>(p))
            {
                UStruct* type = s->GetStruct(); // TObjectPtr<UScriptStruct> → указатель
                if (depth >= MaxDepth || !type)
                {
                    ctx.truncated = true;
                    ctx.out += type ? "{\"struct\":" : "null";
                    if (type)
                    {
                        append_string(ctx.out, type->GetName());
                        ctx.out += '}';
                    }
                }
                else dump_struct(ctx, type, at, depth + 1, EFieldIterationFlags::IncludeDeprecated);
            }
            else if (auto* a = CastField<FArrayProperty>(p))
            {
                struct Header
                {
                    uint8_t* Data;
                    int32_t Num;
                    int32_t Max;
                };
                auto* header = reinterpret_cast<Header*>(at);
                auto* inner = a->GetInner();
                int shown = (std::min)(header->Num, MaxArray);
                if (!inner || header->Num < 0 || (header->Num > 0 && !readable(header->Data)))
                {
                    ctx.out += "\"<массив нечитаем>\"";
                    return;
                }
                if (header->Num > MaxArray) ctx.truncated = true;
                ctx.out += std::format("{{\"n\":{},\"items\":[", header->Num);
                for (int i = 0; i < shown; ++i)
                {
                    if (i) ctx.out += ',';
                    dump_value(ctx, inner, header->Data + static_cast<size_t>(i) * inner->GetElementSize(), depth + 1);
                }
                ctx.out += "]}";
            }
            else
            {
                // Строки, enum, карты, множества, делегаты: имя типа; строки FString читаем как есть
                auto type = p->GetClass().GetName();
                if (type == STR("StrProperty"))
                {
                    struct Str
                    {
                        const wchar_t* Data;
                        int32_t Num;
                        int32_t Max;
                    };
                    auto* str = reinterpret_cast<Str*>(at);
                    if (str->Num > 1 && readable(str->Data)) append_string(ctx.out, StringType(str->Data, static_cast<size_t>((std::min)(str->Num - 1, MaxString + 1))));
                    else ctx.out += "\"\"";
                }
                else if (type == STR("EnumProperty") && p->GetElementSize() <= 8)
                {
                    uint64_t value = 0;
                    std::memcpy(&value, at, static_cast<size_t>(p->GetElementSize()));
                    ctx.out += std::format("{}", value);
                }
                else
                {
                    ctx.out += "\"<";
                    ctx.out += Items::narrow(type);
                    ctx.out += ">\"";
                }
            }
        }

        void dump_struct(Ctx& ctx, UStruct* type, uint8_t* base, int depth, EFieldIterationFlags flags)
        {
            ctx.out += '{';
            bool first = true;
            for (FProperty* p : TFieldRange<FProperty>(type, flags))
            {
                if (p->GetClass().GetName().find(STR("Delegate")) != StringType::npos) continue; // делегаты: шум без значений
                if (!first) ctx.out += ',';
                first = false;
                append_string(ctx.out, Items::wide(clean_name(Items::narrow(p->GetName()))));
                ctx.out += ':';
                dump_value(ctx, p, base + p->GetOffset_ForInternal(), depth);
            }
            ctx.out += '}';
        }
    } // namespace

    std::string struct_to_json(UStruct* type, void* memory)
    {
        Ctx ctx;
        dump_struct(ctx, type, static_cast<uint8_t*>(memory), 0, EFieldIterationFlags::IncludeDeprecated);
        return ctx.out;
    }

    std::string to_json(UObject* object)
    {
        if (!object || !readable(object) || !readable(object->GetClassPrivate())) return "null";
        Ctx ctx;
        dump_struct(ctx, static_cast<UStruct*>(object->GetClassPrivate()), reinterpret_cast<uint8_t*>(object), 0, EFieldIterationFlags::Default);
        std::string json = "{\"class\":";
        append_string(json, object->GetClassPrivate()->GetPathName());
        json += ",\"path\":";
        append_string(json, object->GetFullName());
        json += std::format(",\"truncated\":{},\"props\":", ctx.truncated ? "true" : "false");
        json += ctx.out;
        // Объект держит предмет (деталь машины, якорь, зарядка) — добавляем его живые свойства
        for (auto field : {STR("Instance"), STR("ItemInstance")})
        {
            auto** instance = property<UObject*>(object, field);
            if (!instance || !*instance || !readable(*instance)) continue;
            if (auto item = ItemProps::item_json(*instance); !item.empty())
            {
                json += ",\"item\":" + item;
                break;
            }
        }
        // Машина: предметы во всех её слотах (аккумулятор, бак, гоблер…) с живыми свойствами — у самих слотов в полях только ссылки
        if (Items::narrow(object->GetClassPrivate()->GetName()).find("PlayerCar") != std::string::npos)
        {
            std::vector<UObject*> comps;
            UObjectGlobals::FindAllOf(STR("SlotSceneComponent"), comps);
            std::string slots;
            for (auto* comp : comps)
            {
                if (!comp || comp->GetOuterPrivate() != object) continue;
                auto* tag = property<FName>(comp, STR("SlotName"));
                auto** item_slot = property<UObject*>(comp, STR("ItemSlot"));
                if (!tag || !item_slot || !*item_slot) continue;
                auto items = Items::items_of(*item_slot);
                for (int32_t i = 0; i < items.Num; ++i)
                {
                    if (!items.Data[i] || !readable(items.Data[i])) continue;
                    auto item = ItemProps::item_json(items.Data[i]);
                    if (item.empty()) continue;
                    if (!slots.empty()) slots += ',';
                    append_string(slots, tag->ToString());
                    slots += ':' + item;
                }
            }
            json += ",\"slots\":{" + slots + '}';
        }
        json += '}';
        return json;
    }

    UObject* target()
    {
        auto* manager = UObjectGlobals::FindFirstOf(STR("BP_HighlightManager_C"));
        auto** highlight = manager ? property<UObject*>(manager, STR("CurrentHighlight")) : nullptr;
        if (!highlight || !*highlight || !readable(*highlight)) return nullptr;
        return (*highlight)->GetOuterPrivate(); // компонент подсветки принадлежит самому объекту
    }

    namespace
    {
        // Снимок в файл: имя = время + короткое имя класса (только буквы/цифры/_), запись — в фоновом потоке
        void save_snapshot(UObject* object)
        {
            auto json = to_json(object);
            std::string cls = Items::narrow(object->GetClassPrivate()->GetName());
            for (auto& c : cls)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
            if (cls.size() > 40) cls.resize(40); // имя файла Client → Host в лаунчере не длиннее 80 знаков
            std::time_t now = std::time(nullptr);
            std::tm tm;
            localtime_s(&tm, &now);
            static int counter = 0; // несколько снимков за одну секунду не должны затирать друг друга
            std::filesystem::path file = std::filesystem::path(L"ue4ss/Mods/PDCoopRecon") /
                                         std::format("obj_{:04}{:02}{:02}_{:02}{:02}{:02}_{}_{}.json", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ++counter % 100, cls);
            Output::send<LogLevel::Verbose>(STR("[ObjectDump] {}: {} байт, пишу в фоне\n"), Items::wide(file.filename().string()), json.size());
            // shortcut: поток на каждый снимок — файлы редкие (по нажатию), очередь не нужна; заменить, если снимки пойдут пачками
            std::thread([file, json = std::move(json)] {
                std::ofstream out(file, std::ios::binary);
                out << json;
            }).detach();
        }

        struct Vec3
        {
            float X, Y, Z;
        };

        // Положение актёра в мире: RelativeLocation корня. Присоединённые к чему-то (в машине, на детали) пропускаем — у них оно не мировое
        bool world_location(UObject* actor, Vec3& out)
        {
            auto** root = property<UObject*>(actor, STR("RootComponent"));
            if (!root || !*root || !readable(*root)) return false;
            auto** parent = property<UObject*>(*root, STR("AttachParent"));
            if (parent && *parent) return false;
            auto* location = property<Vec3>(*root, STR("RelativeLocation"));
            if (!location || !readable(location)) return false;
            out = *location;
            return true;
        }

        // Ближайшие к игроку интерактивные объекты: актёры-блюпринты (/Game/…) в радиусе; камни, свет, меши движка отсекаются по пути класса
        std::vector<UObject*> nearby(float radius, size_t limit)
        {
            std::vector<UObject*> found;
            auto* controller = UObjectGlobals::FindFirstOf(STR("PlayerController"));
            auto** pawn = controller ? property<UObject*>(controller, STR("Pawn")) : nullptr;
            Vec3 me{};
            if (!pawn || !*pawn || !world_location(*pawn, me)) return found;
            std::vector<UObject*> actors;
            UObjectGlobals::FindAllOf(STR("Actor"), actors);
            std::vector<std::pair<float, UObject*>> close;
            for (auto* actor : actors)
            {
                if (!actor || actor == *pawn || !readable(actor) || !readable(actor->GetClassPrivate())) continue;
                if (actor->GetName().starts_with(STR("Default__"))) continue;
                if (Items::narrow(actor->GetClassPrivate()->GetPathName()).rfind("/Game/", 0) != 0) continue;
                Vec3 at{};
                if (!world_location(actor, at)) continue;
                float dx = at.X - me.X, dy = at.Y - me.Y, dz = at.Z - me.Z;
                float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance <= radius) close.push_back({distance, actor});
            }
            std::sort(close.begin(), close.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < close.size() && i < limit; ++i) found.push_back(close[i].second);
            return found;
        }
    } // namespace

    void dump_target()
    {
        constexpr float NearbyRadius = 300.0f; // см (3 м): точка актёра — его начало, а не поверхность, поэтому с запасом к «двум метрам»
        constexpr size_t NearbyLimit = 12;
        auto* object = target();
        auto close = nearby(NearbyRadius, NearbyLimit);
        if (!object && close.empty())
        {
            Output::send<LogLevel::Warning>(STR("[ObjectDump] ни цели под прицелом, ни интерактивных объектов в радиусе {} см\n"), static_cast<int>(NearbyRadius));
            return;
        }
        if (object) save_snapshot(object);
        int saved = object ? 1 : 0;
        for (auto* near : close)
        {
            if (near == object) continue;
            save_snapshot(near);
            ++saved;
        }
        Output::send<LogLevel::Verbose>(STR("[ObjectDump] снимков: {} (прицел: {}, рядом в {} см: {})\n"), saved, object ? 1 : 0, static_cast<int>(NearbyRadius), close.size());
    }
} // namespace ObjectDump
