// Диагностика без участия человека: строки [Diag] в UE4SS.log при изменениях (уровни деталей, причуды, якоря) на обеих сторонах,
// плюс полный срез по Ctrl+F6. Читают только известные объекты, ничего не пишут в игру.
#pragma once
#include <atomic>
#include <chrono>

namespace Diag
{
    extern std::atomic<bool> g_snapshot_requested; // Ctrl+F6 (поток клавиш → игровой поток)
    void tick(bool is_host);                       // игровой поток

    // Замер времени tick() подсистем: раз в 10 с строка [Perf] в лог (среднее и максимум за кадр, мс)
    constexpr int PerfCount = 16;
    void perf_add(int index, double ms);
    void perf_report();
    template <class F> void perf(int index, F&& fn)
    {
        auto start = std::chrono::steady_clock::now();
        fn();
        perf_add(index, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
} // namespace Diag
