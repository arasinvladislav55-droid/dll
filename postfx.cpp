#include "postfx.h"

#include "imgui.h"
#include "state.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace uc {

namespace {

const unsigned GL_CLAMP_TO_EDGE_ = 0x812F;

struct Tex { unsigned id = 0; int w = 0, h = 0; };
Tex curT, prevT;            // curT — чистый кадр игры; prevT — готовый кадр с размытием (для следующего кадра)
int gW = 0, gH = 0;
unsigned long long prevAt = 0;   // когда в prevT последний раз писали кадр
float zoomAnim = 0.f;

const int kZoomKeys[5] = {'C', 'Z', 'X', 'V', 'B'};
const float kRatios[6] = {0.f, 4.f / 3.f, 5.f / 4.f, 16.f / 10.f, 21.f / 9.f, 32.f / 9.f};
const uint32_t kTints[6] = {0, 0x3B82F6, 0xEF4444, 0x10B981, 0xFFB454, 0x8888A0};

float clamp01(float v) { return std::min(1.f, std::max(0.f, v)); }
float approach(float v, float target, float k) { return v + (target - v) * std::min(1.f, k); }

/** Копирует текущий экранный кадровый буфер в текстуру. Состояние OpenGL, которое трогаем, возвращаем как было. */
void copyTo(Tex& t) {
    if (gW <= 0 || gH <= 0) return;
    int savedTex = 0, savedRead = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
    glGetIntegerv(GL_READ_BUFFER, &savedRead);
    glReadBuffer(GL_BACK);
    if (!t.id) glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    if (t.w != gW || t.h != gH) {   // первый раз или окно изменило размер
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (int)GL_CLAMP_TO_EDGE_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (int)GL_CLAMP_TO_EDGE_);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, gW, gH, 0);
        t.w = gW;
        t.h = gH;
    } else {
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, gW, gH);
    }
    glBindTexture(GL_TEXTURE_2D, (unsigned)savedTex);
    glReadBuffer((unsigned)savedRead);
}

void cbCapture(const ImDrawList*, const ImDrawCmd*) { copyTo(curT); }
void cbRemember(const ImDrawList*, const ImDrawCmd*) { copyTo(prevT); prevAt = GetTickCount64(); }

bool zoomHeld(Module* zm, bool inWorld, bool focused) {
    if (!inWorld || !focused || !zm || !zm->enabled) return false;
    int idx = std::min(4, std::max(0, zm->mode("key")));
    return (GetAsyncKeyState(kZoomKeys[idx]) & 0x8000) != 0;
}

}  // namespace

bool postfxActive(bool inWorld) {
    if (!inWorld) return false;
    Module* ar = findModule("aspect");
    Module* zm = findModule("zoom");
    Module* mb = findModule("motionblur");
    Module* tn = findModule("tint");
    if (mb && mb->enabled) return true;
    if (tn && tn->enabled) return true;
    if (ar && ar->enabled && ar->mode("ratio") > 0) return true;
    if (zm && zm->enabled && (zoomAnim > 0.002f || zoomHeld(zm, true, true))) return true;
    return false;
}

void postfxBegin(ImDrawList* dl, int W, int H, float dt, bool inWorld, bool focused) {
    gW = W;
    gH = H;
    Module* ar = findModule("aspect");
    Module* zm = findModule("zoom");
    Module* mb = findModule("motionblur");
    Module* tn = findModule("tint");

    // приближение: плавно въезжает и уезжает
    bool held = zoomHeld(zm, inWorld, focused);
    zoomAnim = approach(zoomAnim, held ? 1.f : 0.f, dt * (held ? 9.f : 7.f));
    if (!inWorld || !zm || !zm->enabled) zoomAnim = approach(zoomAnim, 0.f, dt * 12.f);
    float zs = zoomAnim * zoomAnim * (3.f - 2.f * zoomAnim);                       // smoothstep
    float zoomScale = 1.f + (zm ? zm->val("factor", 4) - 1.f : 0.f) * zs;

    bool wantAspect = inWorld && ar && ar->enabled && ar->mode("ratio") > 0;
    bool wantZoom = inWorld && zoomAnim > 0.002f;
    bool wantBlur = inWorld && mb && mb->enabled;
    bool wantTint = inWorld && tn && tn->enabled;

    // область исходного кадра (в долях 0..1 от левого верхнего угла), которая будет растянута на весь экран
    float x0 = 0, x1 = 1, y0 = 0, y1 = 1;
    if (wantAspect) {
        float T = kRatios[std::min(5, std::max(0, ar->mode("ratio")))], S = (float)W / (float)H;
        if (T < S) { float half = T / S / 2.f; x0 = 0.5f - half; x1 = 0.5f + half; }          // уже — обрезаем бока
        else if (T > S) { float half = S / T / 2.f; y0 = 0.5f - half; y1 = 0.5f + half; }      // шире — обрезаем верх и низ
    }
    if (wantZoom) {
        float cx = (x0 + x1) / 2.f, cy = (y0 + y1) / 2.f, hw = (x1 - x0) / 2.f / zoomScale, hh = (y1 - y0) / 2.f / zoomScale;
        x0 = cx - hw; x1 = cx + hw; y0 = cy - hh; y1 = cy + hh;
    }

    ImVec2 full0(0, 0), full1((float)W, (float)H);
    if (wantAspect || wantZoom || wantBlur) dl->AddCallback(cbCapture, nullptr);   // чистый кадр игры → curT

    // 1) преобразование: растягиваем нужную часть кадра на весь экран. В текстуре строки идут снизу вверх, поэтому v = 1 - y.
    if ((wantAspect || wantZoom) && curT.id && curT.w == W && curT.h == H) {
        dl->AddRectFilled(full0, full1, IM_COL32(0, 0, 0, 255));
        dl->AddImage((ImTextureID)(intptr_t)curT.id, full0, full1, ImVec2(x0, 1.f - y0), ImVec2(x1, 1.f - y1), IM_COL32(255, 255, 255, 255));
    }

    // 2) размытие: поверх кадра — предыдущий готовый кадр. Доля сохраняется по времени, а не по кадрам,
    //    поэтому шлейф одинаков и на 60, и на 240 FPS.
    if (wantBlur) {
        float s = mb->val("strength", 50) / 100.f;
        float a = std::pow(s, std::max(0.05f, dt * 60.f));
        bool fresh = prevT.id && prevT.w == W && prevT.h == H && GetTickCount64() - prevAt < 120;   // старый кадр (после меню и т.п.) не подмешиваем
        if (fresh) dl->AddImage((ImTextureID)(intptr_t)prevT.id, full0, full1, ImVec2(0, 1), ImVec2(1, 0), IM_COL32(255, 255, 255, (int)(clamp01(a) * 255.f)));
        dl->AddCallback(cbRemember, nullptr);   // итог без наших плашек → prevT
    }

    // 3) тонировка
    if (wantTint) {
        int ci = std::min(5, std::max(0, tn->mode("color")));
        uint32_t rgb = ci == 0 ? currentPalette().a : kTints[ci];
        float a = tn->val("power", 20) / 100.f * 0.55f;
        dl->AddRectFilled(full0, full1, IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(clamp01(a) * 255.f)));
    }
}

}  // namespace uc
