#include "title.h"

#include "imgui.h"
#include "state.h"
#include "ui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace uc {

namespace {

// ═════════════════ данные от Java ═════════════════

struct VWidget { int x = 0, y = 0, w = 0, h = 0; bool active = true; std::string label; };

std::mutex mtx;
VWidget stage[48];
std::vector<VWidget> live;
std::atomic<int> g_kind{-1};
std::atomic<int> g_gui{0};
std::atomic<long long> g_commitMs{0};

long long nowMs() { return (long long)GetTickCount64(); }

void appendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

// ═════════════════ имя игрока и версия из командной строки игры ═════════════════

std::string g_user = "Player", g_mcver = "Minecraft";

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void parseCommandLine() {
    static bool done = false;
    if (done) return;
    done = true;
    std::wstring cmd = GetCommandLineW();
    std::vector<std::wstring> tok;
    std::wstring cur;
    bool quoted = false, any = false;
    for (wchar_t c : cmd) {
        if (c == L'"') { quoted = !quoted; any = true; }
        else if (c == L' ' && !quoted) { if (any || !cur.empty()) tok.push_back(cur); cur.clear(); any = false; }
        else { cur += c; any = true; }
    }
    if (any || !cur.empty()) tok.push_back(cur);
    for (size_t i = 0; i + 1 < tok.size(); i++) {
        if (tok[i] == L"--username") g_user = toUtf8(tok[i + 1]);
        else if (tok[i] == L"--version") g_mcver = "Minecraft " + toUtf8(tok[i + 1]);
    }
}

// ═════════════════ рисование ═════════════════

ImDrawList* dl = nullptr;
float U = 3.f;               // один «пиксель интерфейса» игры в реальных пикселях
float A = 1.f;               // общая прозрачность входа
float mouseX = -1, mouseY = -1;
bool focused = false;
double enterT = 0;           // 0..∞ секунд с момента показа главного меню
double sessionStart = -1;

struct Ripple { float x, y; double t0; int idx; };
struct Hit { float x, y, w, h; int id; };
std::vector<Ripple> ripples;
std::vector<Hit> hits;
std::vector<float> hoverA(48, 0.f), pressA(48, 0.f);
std::unordered_map<int, float> hoverById;

float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
float approach(float v, float target, float k) { return v + (target - v) * std::min(1.f, k); }
float snap(float v) { return std::floor(v + 0.5f); }
float frac(float v) { return v - std::floor(v); }
float easeOut(float t) { t = clamp01(t); return 1.f - std::pow(1.f - t, 3.f); }

uint32_t hashu(uint32_t n) { n ^= n >> 16; n *= 0x7feb352dU; n ^= n >> 15; n *= 0x846ca68bU; n ^= n >> 16; return n; }
float hash01(uint32_t n) { return (hashu(n) & 0xFFFFFF) / 16777216.f; }

uint32_t mixc(uint32_t x, uint32_t y, float t) {
    t = clamp01(t);
    auto ch = [&](int sh) { return (uint32_t)std::lround(((x >> sh) & 255) * (1 - t) + ((y >> sh) & 255) * t); };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
ImU32 col(uint32_t rgb, float a = 1.f) {
    float k = clamp01(a * A);
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(k * 255.f + 0.5f));
}

// размеры задаются в единицах интерфейса игры — всё масштабируется вместе с ним
float fpx(float size) { return std::max(9.f, std::round(size * U)); }
float tw(const std::string& s, float size, ImFont* f) { return f->CalcTextSizeA(fpx(size), FLT_MAX, 0.f, s.c_str()).x; }
void text(float x, float y, uint32_t rgb, const std::string& s, float size, ImFont* f, float a = 1.f) {
    dl->AddText(f, fpx(size), ImVec2(snap(x), snap(y)), col(rgb, a), s.c_str());
}
void textCentered(float cx, float cy, uint32_t rgb, const std::string& s, float size, ImFont* f, float a = 1.f) {
    float w = tw(s, size, f);
    text(cx - w / 2.f, cy - fpx(size) / 2.f - 1.f, rgb, s, size, f, a);
}
std::string fit(const std::string& s, float maxW, float size, ImFont* f) {
    if (tw(s, size, f) <= maxW) return s;
    std::string t = s;
    while (!t.empty() && tw(t + "...", size, f) > maxW) {   // убираем символ целиком: байты-продолжения и затем ведущий байт
        unsigned char b;
        do { b = (unsigned char)t.back(); t.pop_back(); } while (!t.empty() && (b & 0xC0) == 0x80);
    }
    return t + "...";
}
bool inRect(float px, float py, float x, float y, float w, float h) { return px >= x && px < x + w && py >= y && py < y + h; }

void glowCircle(float cx, float cy, float R, uint32_t rgb, float peak) {
    const int N = 16;
    for (int k = 0; k < N; k++)
        dl->AddCircleFilled(ImVec2(cx, cy), R * (1.f - (float)k / N), col(rgb, peak / N * 1.8f), 48);
}

void edgeFade(float W, float H, float depth, uint32_t rgb, float alpha) {
    ImU32 c = col(rgb, alpha), z = col(rgb, 0.f);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, depth), c, c, z, z);
    dl->AddRectFilledMultiColor(ImVec2(0, H - depth), ImVec2(W, H), z, z, c, c);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(depth, H), c, z, z, c);
    dl->AddRectFilledMultiColor(ImVec2(W - depth, 0), ImVec2(W, H), z, c, c, z);
}

// ── фон: затемнение, северное сияние, частицы, виньетка ──
void drawBackground(Module& mm, float W, float H, double t, float enter) {
    Palette pal = currentPalette();
    float dim = mm.val("dim", 30) / 100.f;

    ImU32 top = col(0x05050C, dim * 0.8f * enter), bot = col(0x05050C, std::min(1.f, dim * 1.35f) * enter);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), top, top, bot, bot);

    if (mm.on("aurora")) {
        float power = mm.val("auroraPower", 60) / 100.f;
        uint32_t cols[3] = {pal.a, pal.b, pal.c};
        const float ax[4] = {0.10f, 0.90f, 0.86f, 0.14f}, ay[4] = {0.18f, 0.22f, 0.86f, 0.84f};   // по углам: центр с кнопками остаётся чистым
        for (int i = 0; i < 4; i++) {
            float ph = i * 1.7f;
            float cx = W * (ax[i] + std::sin((float)t * 0.13f + ph) * 0.05f);
            float cy = H * (ay[i] + std::cos((float)t * 0.11f + ph * 1.3f) * 0.05f);
            float R = std::max(W, H) * (0.30f + 0.05f * (i % 2));
            glowCircle(cx, cy, R, cols[i % 3], 0.15f * power * enter);
        }
    }
    if (mm.on("vignette")) edgeFade(W, H, std::min(W, H) * 0.24f, 0x000000, 0.55f * enter);

    if (mm.on("particles")) {
        int count = (int)mm.val("count", 70);
        uint32_t pc = mixc(0xFFFFFF, pal.c, 0.45f);
        for (int i = 0; i < count; i++) {
            uint32_t b = (uint32_t)i * 8u;
            float sx = hash01(b + 1), sy = hash01(b + 2), speed = 0.008f + 0.03f * hash01(b + 3);
            float size = 0.5f + 1.3f * hash01(b + 4), ph = hash01(b + 5) * 6.2832f, drift = hash01(b + 6) - 0.5f;
            float x = frac(sx + std::sin((float)t * 0.2f + ph) * 0.02f + drift * (float)t * 0.004f) * W;
            float y = frac(sy - (float)t * speed) * H;
            float tw = 0.5f + 0.5f * std::sin((float)t * (0.8f + hash01(b + 7) * 1.5f) + ph);
            float r = size * U * 0.55f;
            dl->AddCircleFilled(ImVec2(x, y), r * 2.8f, col(pc, 0.05f * tw * enter), 16);
            dl->AddCircleFilled(ImVec2(x, y), r, col(pc, (0.22f + 0.5f * tw) * enter), 10);
        }
    }
}

// ── иконки для квадратных кнопок без подписи (язык и специальные возможности) ──
void drawIcon(float cx, float cy, float size, bool globe, uint32_t rgb, float a) {
    float th = std::max(1.f, U * 0.5f);
    if (globe) {
        float r = size * 0.27f;
        dl->AddCircle(ImVec2(cx, cy), r, col(rgb, a), 28, th);
        dl->AddLine(ImVec2(cx - r, cy), ImVec2(cx + r, cy), col(rgb, a), th);
        dl->AddLine(ImVec2(cx, cy - r), ImVec2(cx, cy + r), col(rgb, a), th);
        dl->AddCircle(ImVec2(cx, cy), r * 0.5f, col(rgb, a), 20, th);
    } else {
        float r = size * 0.11f;
        dl->AddCircle(ImVec2(cx, cy - size * 0.17f), r, col(rgb, a), 18, th);
        dl->AddLine(ImVec2(cx - size * 0.2f, cy - size * 0.02f), ImVec2(cx + size * 0.2f, cy - size * 0.02f), col(rgb, a), th);
        dl->AddLine(ImVec2(cx, cy - size * 0.02f), ImVec2(cx, cy + size * 0.08f), col(rgb, a), th);
        dl->AddLine(ImVec2(cx, cy + size * 0.08f), ImVec2(cx - size * 0.15f, cy + size * 0.27f), col(rgb, a), th);
        dl->AddLine(ImVec2(cx, cy + size * 0.08f), ImVec2(cx + size * 0.15f, cy + size * 0.27f), col(rgb, a), th);
    }
}

// ── «стеклянная» кнопка ровно поверх настоящей кнопки игры ──
void drawButton(const VWidget& w, int i, float dt, double t, float enter, bool mouseDown, float W) {
    ImFont* FB = fontBold();
    Palette pal = currentPalette();
    uint32_t acc = pal.a;
    float x = (float)w.x * U, y = (float)w.y * U, ww = (float)w.w * U, hh = (float)w.h * U;
    bool hover = w.active && focused && inRect(mouseX, mouseY, x, y, ww, hh);
    hoverA[i] = approach(hoverA[i], hover ? 1.f : 0.f, dt * 14.f);
    pressA[i] = approach(pressA[i], (hover && mouseDown) ? 1.f : 0.f, dt * 24.f);
    float ha = hoverA[i], pa = pressA[i];
    float r = std::min(hh * 0.24f, 2.4f * U);
    float th = std::max(1.f, U * 0.45f);

    // 1) непрозрачная ПРЯМОУГОЛЬНАЯ подложка: у настоящей кнопки прямые углы, скруглённая бы их не закрыла
    dl->AddRectFilled(ImVec2(x - 1.f, y - 1.f), ImVec2(x + ww + 1.f, y + hh + 1.f), IM_COL32(8, 8, 14, 255));
    // 2) корпус с лёгким цветом темы, при наведении светлеет; блик — несколько полос, чтобы не было резкой границы
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + ww, y + hh), col(mixc(0x14142A, acc, 0.10f + 0.16f * ha)), r);
    for (int k = 0; k < 4; k++)
        dl->AddRectFilled(ImVec2(x + 1, y + 1), ImVec2(x + ww - 1, y + 1 + hh * (0.55f - 0.12f * k)), col(0xFFFFFF, 0.017f + 0.008f * ha), r, ImDrawFlags_RoundCornersTop);
    if (pa > 0.01f) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + ww, y + hh), col(0x000000, 0.28f * pa), r);
    // 3) мягкое свечение при наведении
    for (int k = 1; k <= 6; k++) {
        float e = k * U * 0.45f;
        dl->AddRect(ImVec2(x - e, y - e), ImVec2(x + ww + e, y + hh + e), col(acc, 0.075f * (7 - k) / 6.f * ha), r + e, 0, th);
    }
    // 4) рамка и «выезжающая» линия снизу
    dl->AddRect(ImVec2(x, y), ImVec2(x + ww, y + hh), col(mixc(0x34345A, acc, ha), w.active ? 0.95f : 0.5f), r, 0, th);
    float uw = ww * 0.60f * ha;
    dl->AddRectFilled(ImVec2(x + (ww - uw) / 2, y + hh - U * 1.4f), ImVec2(x + (ww + uw) / 2, y + hh - U * 0.5f), col(mixc(acc, 0xFFFFFF, 0.35f), ha));

    // 5) подпись или иконка
    float textA = w.active ? 1.f : 0.4f;
    float cy = y + hh / 2.f + pa * U * 0.4f;
    bool icon = w.label.empty() || w.w == w.h;
    uint32_t tcol = mixc(0xC9C9DD, 0xFFFFFF, ha);
    if (icon) {
        drawIcon(x + ww / 2.f, cy, hh, x + ww / 2.f < W * 0.5f, tcol, textA);
    } else {
        float size = 8.2f;
        float need = tw(w.label, size, FB), room = ww - 6.f * U;
        if (need > room && need > 0) size *= room / need;   // длинная подпись не вылезает за кнопку
        textCentered(x + ww / 2.f, cy, tcol, w.label, size, FB, textA);
    }

    // 6) блик при появлении меню (по очереди на каждой кнопке) и волна от клика
    (void)enter;
    float sweepT = ((float)enterT - 0.25f - i * 0.07f) / 0.55f;
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + ww, y + hh), true);
    if (sweepT > 0.f && sweepT < 1.f) {
        float bw = hh * 1.6f, cx = x - bw + sweepT * (ww + bw * 2);
        float a = std::sin(3.14159f * sweepT) * 0.22f;
        dl->AddRectFilledMultiColor(ImVec2(cx - bw / 2, y), ImVec2(cx, y + hh), col(0xFFFFFF, 0), col(0xFFFFFF, a), col(0xFFFFFF, a), col(0xFFFFFF, 0));
        dl->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(cx + bw / 2, y + hh), col(0xFFFFFF, a), col(0xFFFFFF, 0), col(0xFFFFFF, 0), col(0xFFFFFF, a));
    }
    for (auto& rp : ripples) {
        if (rp.idx != i) continue;
        float age = (float)(t - rp.t0), life = 0.6f;
        if (age < 0 || age > life) continue;
        dl->AddCircleFilled(ImVec2(rp.x, rp.y), age / life * ww * 0.55f + U, col(mixc(acc, 0xFFFFFF, 0.4f), (1.f - age / life) * 0.32f), 40);
    }
    dl->PopClipRect();
}

// ── боковые панели ──
void card(float x, float y, float w, float h) {
    Palette pal = currentPalette();
    float r = 5.f * U;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col(0x0B0B15, 0.80f), r);
    dl->AddRectFilled(ImVec2(x + 1, y + 1), ImVec2(x + w - 1, y + h * 0.35f), col(0xFFFFFF, 0.03f), r, ImDrawFlags_RoundCornersTop);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col(mixc(0x2A2A44, pal.a, 0.28f), 0.95f), r, 0, std::max(1.f, U * 0.4f));
    dl->AddRectFilled(ImVec2(x + r, y), ImVec2(x + w * 0.45f, y + std::max(1.f, U * 0.5f)), col(pal.a, 0.85f));   // акцентная черта сверху
}

float hoverFor(int id, bool hover, float dt) {
    float& v = hoverById[id];
    v = approach(v, hover ? 1.f : 0.f, dt * 14.f);
    return v;
}

void addHit(float x, float y, float w, float h, int id) { hits.push_back({x, y, w, h, id}); }

void drawLeft(float x, float y, float pw, float dt) {
    ImFont* F = fontNormal(); ImFont* FB = fontBold();
    Palette pal = currentPalette();
    float p = 7.f * U;

    // профиль
    float ch = 36.f * U;
    card(x, y, pw, ch);
    float av = 22.f * U, ax = x + p, ay = y + (ch - av) / 2.f;
    dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + av, ay + av), col(pal.a), 5.f * U);
    dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + av, ay + av * 0.5f), col(pal.c, 0.35f), 5.f * U, ImDrawFlags_RoundCornersTop);
    std::string initial = g_user.empty() ? "?" : g_user.substr(0, 1);
    if (!g_user.empty() && (g_user[0] & 0x80)) {   // первый символ UTF-8 целиком
        size_t n = 1; while (n < g_user.size() && (g_user[n] & 0xC0) == 0x80) n++;
        initial = g_user.substr(0, n);
    } else if (!initial.empty()) initial[0] = (char)std::toupper((unsigned char)initial[0]);
    textCentered(ax + av / 2.f, ay + av / 2.f, 0x0B0B14, initial, 12.f, FB);
    float tx = ax + av + 6.f * U, room = x + pw - tx - p;
    text(tx, y + 8.f * U, 0xFFFFFF, fit(g_user, room, 9.f, FB), 9.f, FB);
    text(tx, y + 21.f * U, 0x8A8AA8, fit(g_mcver, room, 6.5f, F), 6.5f, F);
    y += ch + 5.f * U;

    // статистика
    float rowH = 11.f * U;
    ch = 8.f * U + 3 * rowH;
    card(x, y, pw, ch);
    int fps = (int)(ImGui::GetIO().Framerate + 0.5f);
    std::time_t now = std::time(nullptr); std::tm tmv{}; localtime_s(&tmv, &now);
    char clk[16]; std::strftime(clk, sizeof clk, "%H:%M:%S", &tmv);
    int sess = (int)(ImGui::GetTime() - sessionStart);
    char ses[24]; std::snprintf(ses, sizeof ses, "%02d:%02d", sess / 60 % 100, sess % 60);
    char fpsS[16]; std::snprintf(fpsS, sizeof fpsS, "%d", fps);
    uint32_t fc = fps >= 120 ? 0x4ADE80 : fps >= 60 ? 0xA3E635 : fps >= 30 ? 0xFFB454 : 0xEF4444;
    struct Row { const char* k; std::string v; uint32_t c; } rows[3] = {{"FPS", fpsS, fc}, {"Время", clk, 0xE6E6F0}, {"В игре", ses, 0xE6E6F0}};
    for (int i = 0; i < 3; i++) {
        float ry = y + 4.f * U + i * rowH;
        text(x + p, ry + (rowH - fpx(6.5f)) / 2.f - 1.f, 0x8A8AA8, rows[i].k, 6.5f, F);
        float vw = tw(rows[i].v, 7.5f, FB);
        text(x + pw - p - vw, ry + (rowH - fpx(7.5f)) / 2.f - 1.f, rows[i].c, rows[i].v, 7.5f, FB);
    }
    y += ch + 5.f * U;

    // выбор темы — кружки по цветам тем; текущая обведена
    ch = 26.f * U;
    card(x, y, pw, ch);
    text(x + p, y + 5.f * U, pal.a, "ТЕМА", 5.5f, FB);
    Module* ui = findModule("interface");
    Setting* th = ui ? ui->find("theme") : nullptr;
    float gap = 3.5f * U, inner = pw - 2 * p;
    float d = std::min(8.f * U, (inner - 6 * gap) / 7.f);
    float total = 7 * d + 6 * gap, cx0 = x + p + (inner - total) / 2.f, cy = y + 16.5f * U;
    for (int i = 0; i < 7; i++) {
        float cx = cx0 + i * (d + gap) + d / 2.f;
        bool hv = focused && inRect(mouseX, mouseY, cx - d / 2, cy - d / 2, d, d);
        float ha = hoverFor(100 + i, hv, dt);
        Palette tp = themePalette(i);
        dl->AddCircleFilled(ImVec2(cx, cy), d / 2.f + ha * U * 0.5f, col(tp.a), 24);
        dl->AddCircleFilled(ImVec2(cx - d * 0.12f, cy - d * 0.14f), d * 0.22f, col(tp.c, 0.55f), 14);
        if (th && th->idx == i) dl->AddCircle(ImVec2(cx, cy), d / 2.f + U * 1.6f, col(0xFFFFFF, 0.9f), 28, std::max(1.f, U * 0.5f));
        addHit(cx - d / 2 - gap / 2, cy - d / 2 - U, d + gap, d + 2 * U, 10 + i);
    }
}

void drawRight(float x, float y, float pw, float dt) {
    ImFont* F = fontNormal(); ImFont* FB = fontBold();
    Palette pal = currentPalette();
    float p = 8.f * U;

    float ch = 58.f * U;
    card(x, y, pw, ch);
    dl->PushClipRect(ImVec2(x + 1, y + 1), ImVec2(x + pw - 1, y + ch - 1), true);
    glowCircle(x + pw * 0.85f, y + 6.f * U, pw * 0.7f, pal.a, 0.22f);   // мягкое пятно света за заголовком
    dl->PopClipRect();
    text(x + p, y + 8.f * U, 0xFFFFFF, fit("Universal Client", pw - 2 * p, 10.5f, FB), 10.5f, FB);
    text(x + p, y + 21.f * U, 0x8A8AA8, fit("Новости, обновления и поддержка", pw - 2 * p, 6.5f, F), 6.5f, F);

    float bx = x + p, by = y + 31.f * U, bw = pw - 2 * p, bh = 17.f * U;
    bool hv = focused && inRect(mouseX, mouseY, bx, by, bw, bh);
    float ha = hoverFor(1, hv, dt);
    for (int k = 1; k <= 5; k++) {
        float e = k * U * 0.5f;
        dl->AddRect(ImVec2(bx - e, by - e), ImVec2(bx + bw + e, by + bh + e), col(pal.a, 0.09f * (6 - k) / 5.f * (0.35f + ha)), 5.f * U + e, 0, std::max(1.f, U * 0.45f));
    }
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), col(mixc(pal.a, 0xFFFFFF, 0.12f * ha)), 5.f * U);
    dl->AddRectFilled(ImVec2(bx + 1, by + 1), ImVec2(bx + bw - 1, by + bh * 0.5f), col(0xFFFFFF, 0.16f), 5.f * U, ImDrawFlags_RoundCornersTop);
    textCentered(bx + bw / 2.f, by + bh / 2.f, 0x0B0B14, fit("Открыть Discord", bw - 4 * U, 7.8f, FB), 7.8f, FB);
    addHit(bx, by, bw, bh, 1);

    if (!g_launcherVersion.empty()) {
        std::string v = "Лаунчер v" + g_launcherVersion;
        text(x + p, y + ch - 8.5f * U, 0x6A6A88, v, 5.5f, F);
    }
    y += ch + 5.f * U;

    float h2 = 28.f * U;
    card(x, y, pw, h2);
    text(x + p, y + 5.f * U, pal.a, "В ИГРЕ", 5.5f, FB);
    text(x + p, y + 12.f * U, 0xE6E6F0, fit("Insert / Right Shift", pw - 2 * p, 7.f, FB), 7.f, FB);
    text(x + p, y + 19.5f * U, 0x8A8AA8, fit("открывают меню клиента", pw - 2 * p, 6.f, F), 6.f, F);
}

}  // namespace

// ═════════════════ публичное ═════════════════

void javaWidget(int idx, int x, int y, int w, int h, int active) {
    if (idx < 0 || idx >= 48) return;
    std::lock_guard<std::mutex> g(mtx);
    VWidget& s = stage[idx];
    s.x = x; s.y = y; s.w = w; s.h = h; s.active = active != 0;
    s.label.clear();
}

void javaChar(int idx, int cp) {
    if (idx < 0 || idx >= 48 || cp <= 0) return;
    std::lock_guard<std::mutex> g(mtx);
    if (stage[idx].label.size() < 200) appendUtf8(stage[idx].label, (unsigned)cp);
}

void javaCommit(int count, int kind, int gui) {
    std::lock_guard<std::mutex> g(mtx);
    count = std::max(0, std::min(count, 48));
    live.assign(stage, stage + count);
    g_gui = gui;
    g_kind = kind;
    g_commitMs = nowMs();
}

int javaKind() {
    if (nowMs() - g_commitMs.load() > 1500) return -1;
    return g_kind.load();
}

bool titleActive() {
    Module* mm = findModule("mainmenu");
    return mm && mm->enabled && !g_menuOpen && javaKind() == 1;
}

void setRealMouse(float x, float y, bool f) { mouseX = x; mouseY = y; focused = f; }

void titleIdle() { enterT = 0; ripples.clear(); hits.clear(); }

void renderTitle(int Wi, int Hi, float dt) {
    Module* mm = findModule("mainmenu");
    if (!mm || !fontNormal() || !fontBold()) return;
    parseCommandLine();
    dl = ImGui::GetForegroundDrawList();
    float W = (float)Wi, H = (float)Hi;
    double t = ImGui::GetTime();
    if (sessionStart < 0) sessionStart = t;
    enterT += dt;
    float enter = easeOut((float)enterT / 0.7f);

    int gui = g_gui.load();
    U = gui > 0 ? (float)gui : std::max(1.f, std::round(H / 540.f));   // масштаб интерфейса игры
    A = 1.f;
    hits.clear();

    std::vector<VWidget> w;
    {
        std::lock_guard<std::mutex> g(mtx);
        w = live;
    }

    drawBackground(*mm, W, H, t, enter);

    bool mouseDown = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (mm->on("buttons")) {
        for (int i = 0; i < (int)w.size() && i < 48; i++) drawButton(w[i], i, dt, t, enter, mouseDown, W);
    }
    ripples.erase(std::remove_if(ripples.begin(), ripples.end(), [&](const Ripple& r) { return t - r.t0 > 0.7; }), ripples.end());

    // боковые панели — только там, где хватает места рядом с кнопками и логотипом игры
    if (mm->on("panels") && !w.empty()) {
        float minX = W * 0.5f - 140.f * U, maxX = W * 0.5f + 140.f * U;   // логотип игры шире кнопок
        for (auto& v : w) { minX = std::min(minX, (float)v.x * U); maxX = std::max(maxX, (float)(v.x + v.w) * U); }
        float M = 8.f * U;
        float e = easeOut(((float)enterT - 0.15f) / 0.6f);
        A = e;
        float leftSpace = minX - 2 * M, rightSpace = W - maxX - 2 * M;
        if (leftSpace >= 84.f * U) drawLeft(M - (1.f - e) * 14.f * U, M * 1.6f, std::min(150.f * U, leftSpace), dt);
        if (rightSpace >= 84.f * U) {
            float pw = std::min(150.f * U, rightSpace);
            drawRight(W - M - pw + (1.f - e) * 14.f * U, M * 1.6f, pw, dt);
        }
        A = 1.f;
    }
}

bool titleClick(int x, int y) {
    if (!titleActive()) return false;
    float fx = (float)x, fy = (float)y;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        if (!inRect(fx, fy, it->x, it->y, it->w, it->h)) continue;
        if (it->id == 1) {
            ShellExecuteW(nullptr, L"open", L"https://discord.gg/SV2XFkZ6aG", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (it->id >= 10 && it->id <= 16) {
            if (Module* ui = findModule("interface")) if (Setting* th = ui->find("theme")) { th->idx = it->id - 10; saveConfig(); }
        }
        return true;   // клик по нашим панелям игре не нужен
    }
    // клик по кнопке игры: игра обработает его сама, мы только добавляем волну
    std::vector<VWidget> w;
    {
        std::lock_guard<std::mutex> g(mtx);
        w = live;
    }
    for (int i = 0; i < (int)w.size() && i < 48; i++) {
        if (inRect(fx, fy, (float)w[i].x * U, (float)w[i].y * U, (float)w[i].w * U, (float)w[i].h * U) && w[i].active) {
            ripples.push_back({fx, fy, ImGui::GetTime(), i});
            break;
        }
    }
    return false;
}

}  // namespace uc
