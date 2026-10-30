// uc_menu.dll — меню клиента для Minecraft (Windows x64, OpenGL).
// Загружается лаунчером через System.load (точка входа JNI_OnLoad) либо через -agentpath (Agent_OnLoad).
// Хукает gdi32!SwapBuffers (его вызывает GLFW/LWJGL) и рисует ImGui поверх кадра игры.
#include <windows.h>
#include <GL/gl.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "state.h"
#include "ui.h"

namespace {

typedef BOOL(WINAPI* SwapBuffers_t)(HDC);
SwapBuffers_t oSwap = nullptr;
WNDPROC oWndProc = nullptr;
HWND g_hwnd = nullptr;
bool g_ready = false, g_installed = false;
float g_vx = 0, g_vy = 0;       // виртуальный курсор меню (пиксели клиентской области)
bool g_gotRaw = false;          // приходят ли сырые движения мыши (WM_INPUT)
int g_lastX = 0, g_lastY = 0;   // запасной путь: позиции из WM_MOUSEMOVE
int g_lostFrames = 0;
LARGE_INTEGER g_freq{}, g_last{};

typedef void(APIENTRY* glBindFramebuffer_t)(unsigned int, unsigned int);
glBindFramebuffer_t pBindFramebuffer = nullptr;
const unsigned GL_FRAMEBUFFER_ = 0x8D40, GL_FRAMEBUFFER_BINDING_ = 0x8CA6;

void clientSize(int& w, int& h) {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
}

/**
 * Игра «захватила» мышь, т.е. мы в мире, а не в паузе/инвентаре/чате/настройках?
 * Minecraft в мире просит GLFW отключить курсор: GLFW прячет его (SetCursor(NULL)) и зажимает в окне (ClipCursor).
 * В любом экране (пауза, настройки, чат, инвентарь, главное меню) курсор обычный и не зажат.
 */
bool cursorCaptured() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        if (ci.hCursor == nullptr) return true;           // курсор убран через SetCursor(NULL)
        if (!(ci.flags & CURSOR_SHOWING)) return true;    // или через ShowCursor(FALSE)
    }
    RECT clip{}, client{};
    if (GetClipCursor(&clip) && GetClientRect(g_hwnd, &client)) {
        POINT tl{client.left, client.top}, br{client.right, client.bottom};
        ClientToScreen(g_hwnd, &tl);
        ClientToScreen(g_hwnd, &br);
        bool matchesWindow = std::abs(clip.left - tl.x) <= 2 && std::abs(clip.top - tl.y) <= 2 &&
                             std::abs(clip.right - br.x) <= 2 && std::abs(clip.bottom - br.y) <= 2;
        bool wholeScreen = (clip.right - clip.left) >= GetSystemMetrics(SM_CXVIRTUALSCREEN) &&
                           (clip.bottom - clip.top) >= GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (matchesWindow && !wholeScreen) return true;   // курсор зажат ровно в окне игры
    }
    return false;
}

void clampCursor() {
    int w, h;
    clientSize(w, h);
    g_vx = std::fmin((float)(w - 1), std::fmax(0.f, g_vx));
    g_vy = std::fmin((float)(h - 1), std::fmax(0.f, g_vy));
}

void openMenu() {
    int w, h;
    clientSize(w, h);
    g_vx = w / 2.f;
    g_vy = h / 2.f;
    g_gotRaw = false;
    g_lastX = w / 2;
    g_lastY = h / 2;
    g_lostFrames = 0;
    uc::setMenuOpen(true);
}

void closeMenu() { uc::setMenuOpen(false); }

int xOf(LPARAM lp) { return (int)(short)(lp & 0xFFFF); }
int yOf(LPARAM lp) { return (int)(short)((lp >> 16) & 0xFFFF); }

LRESULT CALLBACK hkWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ImGuiIO& io = ImGui::GetIO();

    // ── горячие клавиши: Insert и Right Shift (у правого Shift скан-код 0x36) ──
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30))) {
        bool rshift = wp == VK_SHIFT && ((lp >> 16) & 0xFF) == 0x36;
        if (rshift || wp == VK_INSERT) {
            if (uc::g_menuOpen) { closeMenu(); return 0; }
            // открывать можно ТОЛЬКО в игре: на паузе, в настройках, чате и инвентаре клавиша уходит игре как обычно
            if (cursorCaptured() && GetForegroundWindow() == g_hwnd) { openMenu(); return 0; }
        }
    }
    if (!uc::g_menuOpen) {
        if (msg == WM_LBUTTONDOWN) uc::registerClick(0);
        else if (msg == WM_RBUTTONDOWN) uc::registerClick(1);
        return CallWindowProcW(oWndProc, h, msg, wp, lp);
    }

    // ── меню открыто: игра ввода не получает ──
    switch (msg) {
        case WM_INPUT: {   // сырые движения мыши двигают наш курсор, игровой камере они не достаются
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_gotRaw = true;
                g_vx += (float)ri.data.mouse.lLastX;
                g_vy += (float)ri.data.mouse.lLastY;
                clampCursor();
            }
            return DefWindowProcW(h, msg, wp, lp);
        }
        case WM_MOUSEMOVE: {
            // Запасной путь, если «сырой ввод» в игре выключен: GLFW сам возвращает курсор в центр после каждого
            // сдвига, поэтому событие ровно в центре считаем возвратом, а не движением.
            if (!g_gotRaw) {
                int w, hh;
                clientSize(w, hh);
                int x = xOf(lp), y = yOf(lp);
                if (x == w / 2 && y == hh / 2) { g_lastX = x; g_lastY = y; }
                else { g_vx += (float)(x - g_lastX); g_vy += (float)(y - g_lastY); g_lastX = x; g_lastY = y; clampCursor(); }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: io.AddMouseButtonEvent(0, true); return 0;
        case WM_LBUTTONUP:   // «отжатия» пропускаем и в игру, чтобы кнопки не залипали
            io.AddMouseButtonEvent(0, false);
            return CallWindowProcW(oWndProc, h, msg, wp, lp);
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            return CallWindowProcW(oWndProc, h, msg, wp, lp);
        case WM_MOUSEWHEEL:
            io.AddMouseWheelEvent(0.f, GET_WHEEL_DELTA_WPARAM(wp) / 120.f);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_ESCAPE) closeMenu();   // Esc закрывает меню и не доходит до игры, т.е. пауза не открывается
            return 0;
        case WM_KILLFOCUS:
            closeMenu();
            break;
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;
        default:
            break;
    }
    return CallWindowProcW(oWndProc, h, msg, wp, lp);
}

void initGui(HWND hwnd) {
    g_hwnd = hwnd;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.MouseDrawCursor = false;   // курсор рисует само меню

    // Segoe UI есть в любой Windows. Диапазоны: кириллица + знаки, которых в ней нет («—», «•», «…»)
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    builder.AddText("—–•…·→✓");
    builder.BuildRanges(&ranges);

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    ImFont* normal = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.f, &cfg, ranges.Data);
    ImFont* bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 16.f, &cfg, ranges.Data);
    if (!normal) normal = io.Fonts->AddFontDefault();
    uc::setFonts(normal, bold ? bold : normal);

    ImGui_ImplOpenGL3_Init("#version 150");
    // Текстура шрифтов создаётся здесь, при запуске игры, а не при первом открытии меню — иначе оно открывалось бы с подвисанием
    ImGui_ImplOpenGL3_NewFrame();

    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    pBindFramebuffer = (glBindFramebuffer_t)wglGetProcAddress("glBindFramebuffer");
    g_ready = true;
}

void drawFrame() {
    ImGuiIO& io = ImGui::GetIO();
    int w, h;
    clientSize(w, h);
    if (w <= 0 || h <= 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart);
    g_last = now;
    dt = std::fmin(0.1f, std::fmax(0.0001f, dt));   // после долгого подвисания анимация не «перепрыгивает»

    bool focused = GetForegroundWindow() == g_hwnd;
    bool captured = cursorCaptured();
    if (uc::g_menuOpen) {
        // игра перешла в экран (например, пауза при потере фокуса) — меню закрываем
        g_lostFrames = captured ? 0 : g_lostFrames + 1;
        if (!focused || g_lostFrames >= 3) closeMenu();
    }
    if (!uc::needsFrame()) return;   // нечего рисовать — кадр ImGui не тратим вообще

    io.DeltaTime = dt;
    io.DisplaySize = ImVec2((float)w, (float)h);
    if (uc::g_menuOpen) io.AddMousePosEvent(g_vx, g_vy);

    // Minecraft мог оставить привязанным свой кадровый буфер — рисуем в экранный и потом возвращаем как было
    int prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_, &prevFbo);
    if (pBindFramebuffer) pBindFramebuffer(GL_FRAMEBUFFER_, 0);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    uc::render(w, h, dt, focused, captured);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (pBindFramebuffer) pBindFramebuffer(GL_FRAMEBUFFER_, (unsigned)prevFbo);
}

BOOL WINAPI hkSwapBuffers(HDC dc) {
    HWND w = WindowFromDC(dc);
    if (!g_ready && w) {
        char cls[64] = {};
        GetClassNameA(w, cls, sizeof cls);
        if (std::strncmp(cls, "GLFW", 4) == 0) initGui(w);   // окно Minecraft создано через GLFW
    }
    if (g_ready && w == g_hwnd) drawFrame();
    return oSwap(dc);
}

void install() {
    if (g_installed) return;
    g_installed = true;

    // файлы лежат рядом с dll
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&install, &self);
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(self, buf, MAX_PATH)) {
        std::wstring p(buf);
        size_t slash = p.find_last_of(L"\\/");
        uc::g_dir = slash == std::wstring::npos ? L"." : p.substr(0, slash);
    }
    uc::initModules();
    uc::loadTheme();
    uc::loadConfig();

    if (MH_Initialize() == MH_OK) {
        void* target = (void*)GetProcAddress(LoadLibraryA("gdi32.dll"), "SwapBuffers");
        if (target && MH_CreateHook(target, (void*)&hkSwapBuffers, (void**)&oSwap) == MH_OK)
            MH_EnableHook(target);
    }
}

}  // namespace

// System.load() вызывает JNI_OnLoad сразу после загрузки библиотеки
extern "C" __declspec(dllexport) int JNI_OnLoad(void* /*vm*/, void* /*reserved*/) {
    install();
    return 0x00010008;   // JNI_VERSION_1_8
}

// запасной путь: -agentpath:uc_menu.dll
extern "C" __declspec(dllexport) int Agent_OnLoad(void* /*vm*/, char* /*options*/, void* /*reserved*/) {
    install();
    return 0;
}
