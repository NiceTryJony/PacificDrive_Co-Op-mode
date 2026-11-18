// Защита тика: каждый модуль вызывается через Guard::run. Сбой (access violation, деление на 0, C++ исключение) не доходит до UE4SS и
// не роняет игру; после FaultsToDisable сбоев подряд модуль выключается до следующей загрузки карты (Guard::reset).
// Без Unreal-заголовков — подключается откуда угодно. Только игровой поток.
#pragma once
#include <excpt.h>

#include <type_traits>

namespace Guard
{
    constexpr int MaxModules = 40, FaultsToDisable = 3;
    enum class State { Ok, Off, Fault, Disabled }; // Off — модуль выключен, не вызывался; Fault — сбой, модуль ещё включён; Disabled — этим сбоем выключен
    struct Result
    {
        State state = State::Ok;
        unsigned long code = 0; // код SEH; 0 — C++ исключение
    };

    namespace detail
    {
        constexpr unsigned long CppException = 0xE06D7363; // C++ throw — оставляем catch(...) ниже, SEH-фильтр его не берёт
        inline unsigned long g_code = 0;
        inline int g_faults[MaxModules]{};
        inline bool g_off[MaxModules]{};

        inline int filter(unsigned long code)
        {
            if (code == CppException) return EXCEPTION_CONTINUE_SEARCH;
            g_code = code;
            return EXCEPTION_EXECUTE_HANDLER;
        }

        // Без объектов с деструкторами в теле — иначе __try не компилируется
        inline bool seh_call(void (*fn)(void*), void* ctx)
        {
            __try
            {
                fn(ctx);
                return true;
            }
            __except (filter(GetExceptionCode()))
            {
                return false;
            }
        }
    } // namespace detail

    template <class F>
    Result run(int id, F&& f)
    {
        if (id < 0 || id >= MaxModules) { f(); return {}; }
        if (detail::g_off[id]) return {State::Off, 0};
        using Fn = std::remove_cv_t<std::remove_reference_t<F>>;
        bool ok = false;
        unsigned long code = 0;
        try
        {
            ok = detail::seh_call([](void* c) { (*static_cast<Fn*>(c))(); }, const_cast<Fn*>(&f));
            if (!ok) code = detail::g_code;
        }
        catch (...)
        {
            ok = false;
        }
        if (ok)
        {
            detail::g_faults[id] = 0;
            return {};
        }
        if (++detail::g_faults[id] < FaultsToDisable) return {State::Fault, code};
        detail::g_off[id] = true;
        return {State::Disabled, code};
    }

    // Карта сменилась — состояние модулей свежее, даём им второй шанс
    inline void reset()
    {
        for (int i = 0; i < MaxModules; ++i) detail::g_faults[i] = 0, detail::g_off[i] = false;
    }
} // namespace Guard
