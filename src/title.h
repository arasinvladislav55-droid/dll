#pragma once
// Оформление главного меню Minecraft. Данные об экране и кнопках присылает Java-часть (uc-boot.jar) через JNI.

namespace uc {

// ── приём данных от Java (вызываются из JNI-функций в main.cpp) ──
void javaWidget(int idx, int x, int y, int w, int h, int active);
void javaChar(int idx, int codePoint);
void javaCommit(int count, int kind, int guiScale);

/** 0 — в мире без экрана, 1 — главное меню, 2 — другой экран; -1 — данных нет или они устарели (тогда работают запасные эвристики). */
int javaKind();

bool titleActive();                                   // нужно ли сейчас рисовать оформление главного меню
void setRealMouse(float x, float y, bool focused);    // курсор ОС в клиентских координатах окна
void renderTitle(int width, int height, float dt);
void titleIdle();                                     // вызывается, пока главное меню не показано (сброс анимации входа)
bool titleClick(int x, int y);                        // true — клик забрало наше оформление (игре его отдавать не нужно)

}  // namespace uc
