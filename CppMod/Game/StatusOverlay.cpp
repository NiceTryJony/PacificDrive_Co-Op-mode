#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // без заголовков UE4SS: winsock/макросы Windows с ними конфликтуют (как Net.cpp)

#include "StatusOverlay.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <thread>

namespace StatusOverlay
{
    namespace
    {
        constexpr wchar_t ClassName[] = L"PacificDriveCoopStatusOverlay";
        constexpr UINT_PTR TimerId = 1;
        constexpr UINT TimerMs = 200;

        std::mutex g_mutex;
        std::wstring g_text;            // что показывать; пусто — скрыто
        std::atomic<bool> g_quit{false};
        std::atomic<HWND> g_overlay{nullptr};
        std::thread g_thread;
        std::wstring g_shown;           // поток окна: что нарисовано сейчас
        std::wstring g_original_title;  // заголовок окна игры до наших правок
        HWND g_title_window = nullptr;
        int g_font_px = 20;            // размер шрифта заголовка (считает layout от высоты окна игры, использует отрисовка)
        int g_title_h = 25;            // высота строки заголовка по замеру layout

        // Вид — как карточка загрузки HUD (docs/design/HUD_PLAN.md, вариант B): тёмная плашка, кремовая рамка, скругление
        constexpr COLORREF Glass = RGB(24, 20, 17), Cream = RGB(239, 230, 207), CreamSoft = RGB(185, 178, 159);
        constexpr wchar_t Title[] = L"Joining the driver's world";
        HFONT make_font(int px, int weight, const wchar_t* face)
        {
            return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        }
        HFONT title_font() { return make_font(g_font_px, FW_SEMIBOLD, L"Bahnschrift"); } // нет шрифта — Windows подставит похожий
        HFONT body_font() { return make_font(g_font_px * 4 / 5, FW_NORMAL, L"Segoe UI"); }
        int pad() { return g_font_px; }
        int radius() { return g_font_px * 4 / 5; }
        // Текст фазы — со строчной (status.txt), в окне — с заглавной
        std::wstring body_text(std::wstring s)
        {
            if (!s.empty()) s[0] = static_cast<wchar_t>(towupper(s[0]));
            return s;
        }

        // Главное окно игры: видимое окно верхнего уровня нашего процесса без владельца (UE4: класс UnrealWindow)
        HWND find_game_window()
        {
            struct Search
            {
                DWORD Pid;
                HWND Found;
            } search{GetCurrentProcessId(), nullptr};
            EnumWindows(
                    [](HWND hwnd, LPARAM param) -> BOOL {
                        auto* s = reinterpret_cast<Search*>(param);
                        DWORD pid = 0;
                        GetWindowThreadProcessId(hwnd, &pid);
                        wchar_t cls[64] = {};
                        GetClassNameW(hwnd, cls, 64);
                        if (pid != s->Pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || std::wstring(cls) != L"UnrealWindow") return TRUE;
                        s->Found = hwnd;
                        return FALSE;
                    },
                    reinterpret_cast<LPARAM>(&search));
            return search.Found;
        }

        // Окно игры принадлежит игровому потоку, а он во время загрузки уровня не качает сообщения: обычные
        // GetWindowText/SetWindowText ждали бы его (и наш поток вместе с ним) — только с таймаутом
        constexpr UINT MessageTimeoutMs = 150;

        // Запасной вариант на случай, если окно поверх игры не видно: тот же текст в заголовке окна игры
        void update_title(HWND game, const std::wstring& text)
        {
            if (!game) return;
            if (game != g_title_window)
            {
                wchar_t buf[256] = {};
                DWORD_PTR got = 0;
                if (!SendMessageTimeoutW(game, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(buf), SMTO_ABORTIFHUNG, MessageTimeoutMs, &got)) return;
                g_title_window = game;
                g_original_title = buf;
            }
            std::wstring title = text.empty() ? g_original_title : g_original_title + L"  |  Co-op: " + text;
            DWORD_PTR ignored = 0;
            SendMessageTimeoutW(game, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title.c_str()), SMTO_ABORTIFHUNG, MessageTimeoutMs, &ignored);
        }

        void layout(HWND hwnd, HWND game, const std::wstring& text)
        {
            RECT client{};
            GetClientRect(game, &client);
            POINT origin{0, 0};
            ClientToScreen(game, &origin);
            int width = client.right - client.left, height = client.bottom - client.top;
            // Размер шрифта — от высоты окна игры (1080p → заголовок 20 px, как карточка HUD)
            g_font_px = std::clamp(height / 54, 14, 40);
            HDC dc = GetDC(hwnd);
            auto measure = [dc](HFONT font, const std::wstring& s) {
                HGDIOBJ old = SelectObject(dc, font);
                RECT box{0, 0, 0, 0};
                DrawTextW(dc, s.c_str(), -1, &box, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, old);
                DeleteObject(font);
                return SIZE{box.right, box.bottom};
            };
            SIZE title = measure(title_font(), Title), body = measure(body_font(), body_text(text));
            ReleaseDC(hwnd, dc);
            g_title_h = title.cy;
            int w =std::min<int>(std::max(title.cx, body.cx) + 2 * pad(), width), h = title.cy + body.cy + pad() * 3 / 2;
            SetWindowPos(hwnd, HWND_TOPMOST, origin.x + (width - w) / 2, origin.y + height / 12, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            // Скруглённые углы — формой окна (рисуем только внутри неё); окно владеет регионом, удалять его не нужно
            SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, radius() * 2, radius() * 2), TRUE);
        }

        void tick(HWND hwnd)
        {
            std::wstring text;
            {
                std::lock_guard lock(g_mutex);
                text = g_text;
            }
            HWND game = find_game_window();
            update_title(game, text);
            // Только пока игра на переднем плане: окно topmost не должно торчать поверх чужих программ
            bool visible = !text.empty() && game && !IsIconic(game) && GetForegroundWindow() == game;
            if (!visible)
            {
                ShowWindow(hwnd, SW_HIDE);
                g_shown.clear();
                return;
            }
            if (text != g_shown || !IsWindowVisible(hwnd))
            {
                g_shown = text;
                layout(hwnd, game, text);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }

        LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            switch (msg)
            {
            case WM_TIMER:
                if (wparam == TimerId) tick(hwnd);
                return 0;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                RECT rc;
                GetClientRect(hwnd, &rc);
                HBRUSH background = CreateSolidBrush(Glass);
                FillRect(dc, &rc, background);
                DeleteObject(background);
                // рамка 3 px (на 1080p) внутри формы окна
                HPEN pen = CreatePen(PS_INSIDEFRAME, std::max(2, g_font_px * 3 / 20), Cream);
                HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, 0, 0, rc.right, rc.bottom, radius() * 2, radius() * 2);
                SelectObject(dc, old_pen);
                SelectObject(dc, old_brush);
                DeleteObject(pen);
                SetBkMode(dc, TRANSPARENT);
                auto line = [dc, &rc](HFONT font, COLORREF color, const std::wstring& s, int top) {
                    HGDIOBJ old = SelectObject(dc, font);
                    SetTextColor(dc, color);
                    RECT r{pad(), top, rc.right - pad(), rc.bottom};
                    DrawTextW(dc, s.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                    SelectObject(dc, old);
                    DeleteObject(font);
                };
                int top = pad() * 3 / 5;
                line(title_font(), Cream, Title, top);
                line(body_font(), CreamSoft, body_text(g_shown), top + g_title_h + pad() / 4);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_NCHITTEST:
                return HTTRANSPARENT; // клики проходят в игру
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        void thread_main()
        {
            HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSW wc{};
            wc.lpfnWndProc = wnd_proc;
            wc.hInstance = instance;
            wc.lpszClassName = ClassName;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW (макрос зависит от UNICODE)
            RegisterClassW(&wc);
            HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, ClassName, L"", WS_POPUP, 0, 0,
                                        10, 10, nullptr, nullptr, instance, nullptr);
            if (!hwnd) return;
            SetLayeredWindowAttributes(hwnd, 0, 225, LWA_ALPHA);
            g_overlay = hwnd;
            SetTimer(hwnd, TimerId, TimerMs, nullptr);
            MSG msg;
            while (!g_quit && GetMessageW(&msg, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            KillTimer(hwnd, TimerId);
            update_title(g_title_window, L""); // вернуть заголовок игры
            DestroyWindow(hwnd);
            UnregisterClassW(ClassName, instance);
            g_overlay = nullptr;
        }
    } // namespace

    void set(const std::wstring& text)
    {
        {
            std::lock_guard lock(g_mutex);
            if (g_text == text) return;
            g_text = text;
        }
        if (!text.empty() && !g_thread.joinable()) g_thread = std::thread(thread_main); // окно создаём при первом показе
    }

    void stop()
    {
        {
            std::lock_guard lock(g_mutex);
            g_text.clear(); // поток больше не допишет заголовок
        }
        // Заголовок возвращаем отсюда: вызывающий поток — владелец окна игры и не заблокирован, а поток надписи ждал бы его
        if (g_title_window) SetWindowTextW(g_title_window, g_original_title.c_str());
        g_quit = true;
        if (HWND hwnd = g_overlay) PostMessageW(hwnd, WM_CLOSE, 0, 0); // DefWindowProc → WM_DESTROY → PostQuitMessage
        if (g_thread.joinable()) g_thread.join();
    }
} // namespace StatusOverlay
