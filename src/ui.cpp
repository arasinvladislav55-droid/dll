#include "ui.h"

#include "imgui.h"
#include "state.h"
#include "title.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <string>
#include <unordered_map>

namespace uc {

bool g_menuOpen = false;

namespace {

ImFont* F = nullptr;
ImFont* FB = nullptr;
ImDrawList* dl = nullptr;

float K = 1.f;           // итоговый масштаб (разрешение экрана × настройка меню)
float A = 1.f;           // общая прозрачность (анимация открытия)
float openAnim = 0.f;    // 0..1 — состояние открытия
float appliedScale = 1.f; // масштаб, который реально используется в раскладке (см. комментарий в drawMenu)
bool appliedInit = false;
int cat = 0;
Module* selected = nullptr;
float scrollList = 0, scrollListT = 0, maxList = 0;
float scrollCont = 0, scrollContT = 0, maxCont = 0;
Setting* dragging = nullptr;
float clipT = -1e9f, clipB = 1e9f;
std::deque<double> clicks[2];
std::unordered_map<std::string, float> anims;

const uint32_t BORDER = 0x2A2A44, PANEL = 0x0C0C16, CARD = 0x13131F;
const uint32_t TEXTC = 0xE6E6F0, MUTED = 0x8A8AA8, OFFC = 0x2A2A44, CHIP = 0x1B1B2E;

// ── утилиты ──
float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
float approach(float v, float target, float k) { return v + (target - v) * std::min(1.f, k); }
float P(float v) { return v * K; }
float snap(float v) { return std::floor(v + 0.5f); }

uint32_t mixc(uint32_t x, uint32_t y, float t) {
    t = clamp01(t);
    auto ch = [&](int sh) { return (uint32_t)std::lround(((x >> sh) & 255) * (1 - t) + ((y >> sh) & 255) * t); };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}
ImU32 col(uint32_t rgb, float a = 1.f) {
    float k = clamp01(a * A);
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(k * 255.f + 0.5f));
}
float& anim(const std::string& key) { return anims[key]; }   // состояния наведения: 0..1

float fontPx(float size) { return std::max(8.f, std::round(size * K)); }
float tw(const std::string& s, float size = 15.f, ImFont* font = nullptr) {
    return (font ? font : F)->CalcTextSizeA(fontPx(size), FLT_MAX, 0.f, s.c_str()).x;
}
void text(float x, float y, uint32_t rgb, const std::string& s, float a = 1.f, float size = 15.f, ImFont* font = nullptr) {
    dl->AddText(font ? font : F, fontPx(size), ImVec2(snap(x), snap(y)), col(rgb, a), s.c_str());
}
// по центру вертикали блока высотой h
void textV(float x, float y, float h, uint32_t rgb, const std::string& s, float a = 1.f, float size = 15.f, ImFont* font = nullptr) {
    text(x, y + (h - fontPx(size)) / 2.f - 1.f, rgb, s, a, size, font);
}
std::string fit(const std::string& s, float maxW, float size = 15.f) {
    if (tw(s, size) <= maxW) return s;
    std::string t = s;
    while (!t.empty() && tw(t + "...", size) > maxW) {   // убираем символ UTF-8 целиком: байты-продолжения и затем ведущий байт
        unsigned char b;
        do { b = (unsigned char)t.back(); t.pop_back(); } while (!t.empty() && (b & 0xC0) == 0x80);
    }
    return t + "...";
}

bool inRect(ImVec2 m, float x, float y, float w, float h) { return m.x >= x && m.x < x + w && m.y >= y && m.y < y + h; }
bool hov(float x, float y, float w, float h) {
    ImVec2 m = ImGui::GetIO().MousePos;
    return inRect(m, x, y, w, h) && m.y >= clipT && m.y < clipB;
}

void rrect(float x, float y, float w, float h, float r, ImU32 c) {
    dl->AddRectFilled(ImVec2(snap(x), snap(y)), ImVec2(snap(x + w), snap(y + h)), c, r);
}
void rline(float x, float y, float w, float h, float r, ImU32 c, float t = 1.f) {
    dl->AddRect(ImVec2(snap(x) + .5f, snap(y) + .5f), ImVec2(snap(x + w) - .5f, snap(y + h) - .5f), c, r, 0, t);
}
void toggle(float x, float y, float p) {
    float w = P(26), h = P(14);
    rrect(x, y, w, h, h / 2, col(mixc(OFFC, currentPalette().a, p)));
    dl->AddCircleFilled(ImVec2(snap(x + P(7) + p * P(12)), snap(y + h / 2)), P(5), col(0xFFFFFF));
}
void accentLine(float x, float y, float w) {
    Palette pal = currentPalette();
    const int N = 6;
    float seg = w / N;
    for (int i = 0; i < N; i++) {
        uint32_t c0 = mixc(pal.a, pal.c, i / (float)N), c1 = mixc(pal.a, pal.c, (i + 1) / (float)N);
        float f0 = std::sin(3.14159f * i / N), f1 = std::sin(3.14159f * (i + 1) / N);
        dl->AddRectFilledMultiColor(ImVec2(snap(x + seg * i), snap(y)), ImVec2(snap(x + seg * (i + 1)), snap(y) + 1),
                                    col(c0, f0), col(c1, f1), col(c1, f1), col(c0, f0));
    }
}

// ═════════════════════════ HUD ═════════════════════════

double cps(int b) {
    double now = ImGui::GetTime();
    while (!clicks[b].empty() && now - clicks[b].front() > 1.0) clicks[b].pop_front();
    return (double)clicks[b].size();
}

struct Widget { float w = 0, h = 0; };

void hudCard(float x, float y, float w, float h, float bgAlpha) {
    rrect(x, y, w, h, P(5), col(0x0B0B14, bgAlpha));
    rrect(x, y + P(4), P(2), h - P(8), P(1), col(currentPalette().a));
}

uint32_t fpsColor(int fps) {
    if (fps >= 120) return 0x4ADE80;
    if (fps >= 60) return 0xA3E635;
    if (fps >= 30) return 0xFFB454;
    return 0xEF4444;
}

std::string clockText(Module& m) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char b[32];
    bool h24 = m.on("h24"), sec = m.on("sec");
    std::strftime(b, sizeof b, h24 ? (sec ? "%H:%M:%S" : "%H:%M") : (sec ? "%I:%M:%S %p" : "%I:%M %p"), &tmv);
    return b;
}

std::string watermarkText(Module& m) {
    char b[64];
    if (m.on("fps")) std::snprintf(b, sizeof b, "Universal Client  |  %d FPS", (int)(ImGui::GetIO().Framerate + 0.5f));
    else std::snprintf(b, sizeof b, "Universal Client");
    return b;
}

Widget hudMeasure(Module& m) {
    float pad = P(10), h = P(22);
    if (m.id == "watermark") return {tw("Universal Client  |  000 FPS") + pad * 2, h};
    if (m.id == "fps") return {tw("000 FPS") + pad * 2 + (m.on("bar") ? P(4) : 0), h};
    if (m.id == "clock") return {tw(clockText(m)) + pad * 2, h};
    if (m.id == "keys") {
        float u = P(24), g = P(3), hh = 2 * u + g;
        if (m.on("space")) hh += g + P(16);
        if (m.on("mouse")) hh += g + u;
        return {3 * u + 2 * g, hh};
    }
    return {0, 0};
}

void keyBox(const std::string& id, float x, float y, float w, float h, const std::string& label, bool down, float dt, float bgAlpha, float size = 14.f) {
    float& a = anim("key_" + id);
    a = approach(a, down ? 1.f : 0.f, dt * 20.f);
    rrect(x, y, w, h, P(5), col(mixc(0x0B0B14, currentPalette().a, a * 0.9f), bgAlpha + (1.f - bgAlpha) * a));
    float lw = tw(label, size);
    textV(x + (w - lw) / 2, y, h, mixc(TEXTC, 0x0B0B14, a), label, 1.f, size);
}

void hudDraw(Module& m, float x, float y, Widget w, float dt) {
    float a = m.val("bg", 70) / 100.f, pad = P(10);
    if (m.id == "watermark") {
        hudCard(x, y, w.w, w.h, a);
        textV(x + pad, y, w.h, currentPalette().a, watermarkText(m), 1.f, 14.f);
    } else if (m.id == "fps") {
        int fps = (int)(ImGui::GetIO().Framerate + 0.5f);
        hudCard(x, y, w.w, w.h, a);
        float bx = x + pad;
        if (m.on("bar")) {   // цветная полоса слева от числа
            rrect(x + pad - P(1), y + P(5), P(2), w.h - P(10), P(1), col(fpsColor(fps)));
            bx += P(5);
        }
        char b[24]; std::snprintf(b, sizeof b, "%d FPS", fps);
        textV(bx, y, w.h, TEXTC, b, 1.f, 14.f);
    } else if (m.id == "clock") {
        hudCard(x, y, w.w, w.h, a);
        textV(x + pad, y, w.h, TEXTC, clockText(m), 1.f, 14.f);
    } else if (m.id == "keys") {
        float u = P(24), g = P(3);
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        keyBox("w", x + u + g, y, u, u, "W", down('W'), dt, a);
        keyBox("a", x, y + u + g, u, u, "A", down('A'), dt, a);
        keyBox("s", x + u + g, y + u + g, u, u, "S", down('S'), dt, a);
        keyBox("d", x + 2 * (u + g), y + u + g, u, u, "D", down('D'), dt, a);
        float yy = y + 2 * (u + g);
        if (m.on("space")) { keyBox("sp", x, yy, w.w, P(16), "ПРОБЕЛ", down(VK_SPACE), dt, a, 11.f); yy += P(16) + g; }
        if (m.on("mouse")) {
            float bw = (w.w - g) / 2;
            char l[24], r[24];
            std::snprintf(l, sizeof l, "ЛКМ %d", (int)cps(0));
            std::snprintf(r, sizeof r, "ПКМ %d", (int)cps(1));
            keyBox("lm", x, yy, bw, u, l, down(VK_LBUTTON), dt, a, 12.f);
            keyBox("rm", x + bw + g, yy, bw, u, r, down(VK_RBUTTON), dt, a, 12.f);
        }
    }
}

void edgeFade(float W, float H, float depth, uint32_t rgb, float alpha) {
    ImU32 c = col(rgb, alpha), z = col(rgb, 0.f);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, depth), c, c, z, z);                 // сверху
    dl->AddRectFilledMultiColor(ImVec2(0, H - depth), ImVec2(W, H), z, z, c, c);             // снизу
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(depth, H), c, z, z, c);                 // слева
    dl->AddRectFilledMultiColor(ImVec2(W - depth, 0), ImVec2(W, H), z, c, c, z);             // справа
}

void drawCrosshair(Module& m, float W, float H) {
    float len = P(m.val("len", 6)), gap = P(m.val("gap", 3)), t = std::max(1.f, std::round(P(m.val("thick", 2))));
    uint32_t c = 0xFFFFFF;
    switch (m.mode("color")) { case 0: c = currentPalette().a; break; case 2: c = 0x4ADE80; break; case 3: c = 0xEF4444; break; default: break; }
    float cx = snap(W / 2), cy = snap(H / 2), o = 1.f;     // o — тёмная обводка для читаемости на любом фоне
    auto bar = [&](float x1, float y1, float x2, float y2) {
        dl->AddRectFilled(ImVec2(x1 - o, y1 - o), ImVec2(x2 + o, y2 + o), col(0x000000, 0.55f));
        dl->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), col(c));
    };
    float h2 = std::floor(t / 2);
    bar(cx - gap - len, cy - h2, cx - gap, cy - h2 + t);
    bar(cx + gap, cy - h2, cx + gap + len, cy - h2 + t);
    bar(cx - h2, cy - gap - len, cx - h2 + t, cy - gap);
    bar(cx - h2, cy + gap, cx - h2 + t, cy + gap + len);
    if (m.on("dot")) bar(cx - h2, cy - h2, cx - h2 + t, cy - h2 + t);
}

void renderHud(int W, int H, float dt, bool focused, bool captured) {
    if (!focused) return;
    float margin = P(8), gap = P(5);
    float cur[4] = {margin, margin, margin, margin};   // сколько уже занято в каждом углу

    for (auto& m : g_modules) {
        if (m.category != 0 || !m.enabled) continue;
        if (captured && m.id == "vignette") edgeFade((float)W, (float)H, std::min(W, H) * (0.18f + 0.30f * m.val("power", 45) / 100.f), 0x000000, 0.25f + 0.70f * m.val("power", 45) / 100.f);
        else if (captured && m.id == "edgeglow") edgeFade((float)W, (float)H, std::min(W, H) * 0.16f, currentPalette().a, 0.10f + 0.45f * m.val("power", 40) / 100.f);
        else if (captured && m.id == "crosshair") drawCrosshair(m, (float)W, (float)H);
    }
    for (auto& m : g_modules) {
        if (m.category != 0 || !m.enabled) continue;
        Widget w = hudMeasure(m);
        if (w.w <= 0) continue;
        int c = m.mode("pos");
        bool right = (c == 1 || c == 3), bottom = (c == 2 || c == 3);
        float x = right ? W - margin - w.w : margin;
        float y = bottom ? H - cur[c] - w.h : cur[c];
        hudDraw(m, snap(x), snap(y), w, dt);
        cur[c] += w.h + gap;
    }
}

// ═════════════════════════ меню ═════════════════════════

float rowHeight(const Setting& s, float width) {
    if (s.type == Setting::Bool) return P(28);
    if (s.type == Setting::Slider) return P(30);
    float cx = 0; int lines = 1;
    for (auto& o : s.opts) {
        float cw = tw(o, 14.f) + P(18);
        if (cx > 0 && cx + cw > width) { lines++; cx = 0; }
        cx += cw + P(6);
    }
    return P(22) + lines * P(26);
}

void drawSetting(Setting& s, float x, float y, float w, float dt, const std::string& key) {
    ImGuiIO& io = ImGui::GetIO();
    bool clicked = ImGui::IsMouseClicked(0);
    uint32_t acc = currentPalette().a;

    if (s.type == Setting::Bool) {
        float h = P(28);
        s.anim = approach(s.anim, s.b ? 1.f : 0.f, dt * 14.f);
        bool hv = hov(x, y, w, h);
        float& hv_a = anim(key); hv_a = approach(hv_a, hv ? 1.f : 0.f, dt * 14.f);
        textV(x, y, h, mixc(0xB4B4C8, 0xFFFFFF, hv_a), s.label);
        toggle(x + w - P(26), y + (h - P(14)) / 2, s.anim);
        if (clicked && hv) s.b = !s.b;

    } else if (s.type == Setting::Slider) {
        float h = P(30);
        char val[32]; std::snprintf(val, sizeof val, "%d%s", (int)std::lround(s.f), s.unit.c_str());
        float vw = tw(val);
        textV(x, y, h, 0xB4B4C8, s.label);
        textV(x + w - vw, y, h, TEXTC, val);
        float tx = x + w * 0.42f, tl = w - w * 0.42f - vw - P(14), by = y + h / 2 - P(2);
        float target = clamp01((s.f - s.mn) / (s.mx - s.mn));
        bool fresh = anims.find(key) == anims.end();
        float& fa = anim(key);
        if (fresh) fa = target; else fa = (dragging == &s) ? target : approach(fa, target, dt * 16.f);   // плавный ход ползунка
        rrect(tx, by, tl, P(4), P(2), col(OFFC));
        rrect(tx, by, std::max(P(3), tl * fa), P(4), P(2), col(acc));
        bool hv = hov(tx - P(6), y, tl + P(12), h) || dragging == &s;
        float& ka = anim(key + "#k"); ka = approach(ka, hv ? 1.f : 0.f, dt * 14.f);
        dl->AddCircleFilled(ImVec2(snap(tx + tl * fa), snap(by + P(2))), P(5) + P(1.5f) * ka, col(0xFFFFFF));
        if (clicked && hov(tx - P(6), y, tl + P(12), h)) dragging = &s;
        if (dragging == &s) {
            if (ImGui::IsMouseDown(0)) {
                float f = clamp01((io.MousePos.x - tx) / tl);
                float v = s.mn + f * (s.mx - s.mn);
                s.f = std::min(s.mx, std::max(s.mn, std::round(v / s.step) * s.step));
            } else {
                dragging = nullptr;
                saveConfig();
            }
        }

    } else {
        text(x, y + P(2), MUTED, s.label, 1.f, 12.f);
        float cx = 0, cy = y + P(22);
        for (int i = 0; i < (int)s.opts.size(); i++) {
            float cw = tw(s.opts[i], 14.f) + P(18);
            if (cx > 0 && cx + cw > w) { cx = 0; cy += P(26); }
            bool sel = i == s.idx;
            bool hv = hov(x + cx, cy, cw, P(22));
            float& ca = anim(key + "#" + std::to_string(i));
            ca = approach(ca, sel ? 1.f : (hv ? 0.5f : 0.f), dt * 14.f);
            rrect(x + cx, cy, cw, P(22), P(7), col(mixc(CHIP, acc, ca * 0.30f)));
            if (sel) rline(x + cx, cy, cw, P(22), P(7), col(acc, 0.75f));
            textV(x + cx + P(9), cy, P(22), mixc(MUTED, 0xFFFFFF, ca), s.opts[i], 1.f, 14.f);
            if (clicked && hv) { s.idx = i; saveConfig(); }
            cx += cw + P(6);
        }
    }
}

void drawMenu(int W, int H, float dt) {
    ImGuiIO& io = ImGui::GetIO();
    Module* ui = findModule("interface");
    float userScale = ui ? ui->val("scale", 100) / 100.f : 1.f;
    float opacity = ui ? ui->val("opacity", 96) / 100.f : 0.96f;
    bool glow = !ui || ui->on("glow");
    bool darken = !ui || ui->on("darken");
    bool clicked = ImGui::IsMouseClicked(0);
    Palette pal = currentPalette();
    uint32_t acc = pal.a;

    // ВАЖНО: пока тянется ползунок масштаба, раскладка использует прежнее значение. Иначе ползунок уезжает
    // из-под курсора вместе с меню и масштаб «убегает». Новое значение применяется плавно после отпускания.
    bool scaleDrag = dragging && ui && dragging == ui->find("scale");
    if (!appliedInit) { appliedScale = userScale; appliedInit = true; }
    if (!scaleDrag) appliedScale = approach(appliedScale, userScale, dt * 12.f);

    float dpi = std::round(std::min(2.5f, std::max(0.75f, H / 1080.f)) * 4.f) / 4.f;
    const float BW = 780.f, BH = 450.f;                       // размер меню в базовых единицах
    float fitK = std::min((W - 24.f) / BW, (H - 24.f) / BH);   // меню никогда не выходит за экран
    K = std::min(dpi * appliedScale, std::max(0.4f, fitK));

    float e = 1.f - std::pow(1.f - openAnim, 3.f);             // ease-out cubic
    A = e;
    if (darken) dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H), col(0x000000, 0.42f));

    float pw = snap(P(BW)), ph = snap(P(BH));
    float px = snap((W - pw) / 2.f), py = snap((H - ph) / 2.f + (1.f - e) * P(16));
    const float HEAD = P(40), FOOT = P(24), LEFT = P(176);

    if (glow) {
        for (int k = 1; k <= 8; k++)
            dl->AddRect(ImVec2(px - k * K, py - k * K), ImVec2(px + pw + k * K, py + ph + k * K),
                        col(acc, 0.055f * (9 - k) / 8.f), P(12) + k * K, 0, std::max(1.f, K * 1.4f));
    }
    rrect(px, py, pw, ph, P(12), col(PANEL, opacity));
    // лёгкая подсветка шапки акцентным цветом
    dl->AddRectFilledMultiColor(ImVec2(px + 1, py + 1), ImVec2(px + pw - 1, py + HEAD),
                                col(acc, 0.09f), col(pal.c, 0.05f), col(pal.c, 0.0f), col(acc, 0.0f));
    rline(px, py, pw, ph, P(12), col(mixc(BORDER, acc, 0.30f)));

    // ── шапка ──
    float hx = px + P(18);
    text(hx, py + (HEAD - fontPx(15)) / 2 - 1, mixc(acc, 0xFFFFFF, 0.55f), "UNIVERSAL CLIENT", 1.f, 15.f, FB);
    hx += tw("UNIVERSAL CLIENT", 15.f, FB) + P(30);
    const char* tabs[2] = {"Визуалы", "Клиент"};
    for (int i = 0; i < 2; i++) {
        float w = tw(tabs[i], 15.f, FB);
        bool sel = cat == i, hv = hov(hx - P(10), py + P(5), w + P(20), HEAD - P(10));
        float& ta = anim(std::string("tab") + std::to_string(i));
        ta = approach(ta, sel ? 1.f : (hv ? 0.6f : 0.f), dt * 14.f);
        textV(hx, py, HEAD, mixc(MUTED, acc, ta), tabs[i], 1.f, 15.f, FB);
        rrect(hx, py + HEAD - P(3), w * (sel ? 1.f : 0.f), P(2), P(1), col(acc));
        if (clicked && hv && !sel) { cat = i; selected = nullptr; scrollList = scrollListT = scrollCont = scrollContT = 0; }
        hx += w + P(30);
    }
    accentLine(px + P(18), py + HEAD, pw - P(36));

    float bodyT = py + HEAD + P(2), bodyB = py + ph - FOOT;
    if (!selected || selected->category != cat) {
        selected = nullptr;
        for (auto& m : g_modules) if (m.category == cat) { selected = &m; break; }
    }

    // ── список модулей ──
    {
        float lx = px + P(12), lw = LEFT - P(8), rowH = P(32);
        float y = bodyT + P(10) - scrollList;
        dl->PushClipRect(ImVec2(px + 1, bodyT), ImVec2(px + LEFT + P(8), bodyB), true);
        clipT = bodyT; clipB = bodyB;
        int n = 0;
        for (auto& m : g_modules) {
            if (m.category != cat) continue;
            n++;
            bool sel = &m == selected, hv = hov(lx, y, lw, rowH);
            float& ha = anim("row_" + m.id); ha = approach(ha, sel ? 1.f : (hv ? 0.5f : 0.f), dt * 14.f);
            float tx = lx + lw - P(34), ty = y + (rowH - P(14)) / 2;
            rrect(lx, y, lw, rowH, P(8), col(mixc(0xFFFFFF, acc, sel ? 1.f : 0.f), 0.045f * ha + (sel ? 0.10f : 0.f)));
            rrect(lx, y + P(7), P(3) * ha, rowH - P(14), P(1.5f), col(acc));
            m.anim = approach(m.anim, m.enabled ? 1.f : 0.f, dt * 14.f);
            textV(lx + P(14), y, rowH, mixc(MUTED, TEXTC, std::max(ha, m.anim * 0.8f)), fit(m.name, lw - P(64)));
            toggle(tx, ty, m.anim);
            if (clicked && hv) {
                if (hov(tx - P(6), y, P(40), rowH)) { m.enabled = !m.enabled; saveConfig(); }
                else selected = &m;
            }
            y += rowH + P(4);
        }
        dl->PopClipRect();
        maxList = std::max(0.f, n * (rowH + P(4)) + P(12) - (bodyB - bodyT));
        scrollListT = std::min(std::max(scrollListT, 0.f), maxList);
        scrollList = approach(scrollList, scrollListT, dt * 16.f);
    }
    // разделитель
    dl->AddRectFilled(ImVec2(snap(px + LEFT + P(8)), snap(bodyT + P(8))), ImVec2(snap(px + LEFT + P(8)) + 1, snap(bodyB - P(4))), col(BORDER, 0.7f));

    // ── настройки выбранного модуля ──
    {
        float rx = px + LEFT + P(26), rw = pw - LEFT - P(44);
        float y = bodyT + P(14) - scrollCont;
        float startY = y;
        dl->PushClipRect(ImVec2(rx - P(8), bodyT), ImVec2(px + pw - 2, bodyB), true);
        clipT = bodyT; clipB = bodyB;
        if (selected) {
            Module& m = *selected;
            text(rx, y, 0xFFFFFF, m.name, 1.f, 22.f, FB);
            text(rx, y + P(30), MUTED, fit(m.desc, rw - P(52), 13.f), 1.f, 13.f);
            m.anim = approach(m.anim, m.enabled ? 1.f : 0.f, dt * 14.f);
            toggle(rx + rw - P(26), y + P(6), m.anim);
            if (clicked && hov(rx + rw - P(34), y, P(34), P(26))) { m.enabled = !m.enabled; saveConfig(); }
            y += P(60);

            std::vector<std::string> groups;
            for (auto& s : m.settings)
                if (std::find(groups.begin(), groups.end(), s.group) == groups.end()) groups.push_back(s.group);
            for (auto& g : groups) {
                float inner = rw - P(28), ch = P(40);
                for (auto& s : m.settings) if (s.group == g) ch += rowHeight(s, inner) + P(2);
                rrect(rx, y, rw, ch, P(9), col(CARD, opacity));
                rline(rx, y, rw, ch, P(9), col(BORDER, 0.85f));
                text(rx + P(14), y + P(12), acc, g, 1.f, 12.f, FB);
                float sy = y + P(34);
                for (auto& s : m.settings) {
                    if (s.group != g) continue;
                    drawSetting(s, rx + P(14), sy, inner, dt, m.id + "." + s.id);
                    sy += rowHeight(s, inner) + P(2);
                }
                y += ch + P(12);
            }
        }
        dl->PopClipRect();
        maxCont = std::max(0.f, (y - startY) + P(14) - (bodyB - bodyT));
        scrollContT = std::min(std::max(scrollContT, 0.f), maxCont);
        scrollCont = approach(scrollCont, scrollContT, dt * 16.f);
    }
    clipT = -1e9f; clipB = 1e9f;

    // ── подвал ──
    const char* hint = "Insert / Right Shift — открыть и закрыть меню   •   Esc — закрыть";
    textV(px + P(18), py + ph - FOOT, FOOT, MUTED, hint, 0.9f, 12.f);

    // колесо мыши: слева — список, справа — настройки (с плавной прокруткой)
    if (io.MouseWheel != 0.f) {
        float d = -io.MouseWheel * P(44);
        if (io.MousePos.x < px + LEFT + P(10)) scrollListT = std::min(std::max(scrollListT + d, 0.f), maxList);
        else scrollContT = std::min(std::max(scrollContT + d, 0.f), maxCont);
    }

    // ── свой курсор: поверх всего, всегда в пределах окна ──
    ImVec2 m = io.MousePos;
    float cs = std::max(1.f, K);
    ImVec2 p0(snap(m.x), snap(m.y));
    ImVec2 p1(p0.x, p0.y + 17 * cs), p2(p0.x + 12 * cs, p0.y + 12.5f * cs);
    dl->AddTriangleFilled(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x - 1, p1.y + 2), ImVec2(p2.x + 2, p2.y + 0.5f), col(0x000000, 0.85f));
    dl->AddTriangleFilled(p0, p1, p2, col(0xFFFFFF));
}

}  // namespace

void setFonts(ImFont* normal, ImFont* bold) { F = normal; FB = bold ? bold : normal; }
ImFont* fontNormal() { return F; }
ImFont* fontBold() { return FB; }

void registerClick(int button) { if (button == 0 || button == 1) clicks[button].push_back(ImGui::GetTime()); }

void setMenuOpen(bool open) {
    g_menuOpen = open;
    dragging = nullptr;
    if (!open) saveConfig();
}

bool needsFrame() {
    if (g_menuOpen || openAnim > 0.001f || titleActive()) return true;
    for (auto& m : g_modules) if (m.category == 0 && m.enabled) return true;
    return false;
}

void render(int W, int H, float dt, bool focused, bool captured) {
    if (!F) return;
    dl = ImGui::GetForegroundDrawList();
    float dpi = std::round(std::min(2.5f, std::max(0.75f, H / 1080.f)) * 4.f) / 4.f;
    K = dpi;
    A = 1.f;
    if (titleActive()) renderTitle(W, H, dt); else titleIdle();   // оформление главного меню — под плашками HUD
    renderHud(W, H, dt, focused, captured);

    Module* ui = findModule("interface");
    bool smooth = !ui || ui->on("anim");
    float step = smooth ? dt * (g_menuOpen ? 5.5f : 8.f) : 1.f;      // открытие ≈ 0.18 с, закрытие ≈ 0.12 с
    openAnim = clamp01(openAnim + (g_menuOpen ? step : -step));
    if (openAnim > 0.f) drawMenu(W, H, dt);
    A = 1.f;
}

}  // namespace uc
