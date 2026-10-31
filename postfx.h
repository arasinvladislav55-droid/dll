#pragma once
// Экранные эффекты: размытие в движении, соотношение сторон, приближение, тонировка.
// Кадр игры копируется в текстуру прямо внутри списка команд ImGui (через callback), поэтому порядок рисования точный.
struct ImDrawList;

namespace uc {

/** Добавляет в начало списка команд всё нужное для включённых эффектов. Вызывать ДО любого другого рисования. */
void postfxBegin(ImDrawList* dl, int width, int height, float dt, bool inWorld, bool gameFocused);

/** Нужен ли кадр ImGui ради эффектов (чтобы не тратить кадры, когда всё выключено). */
bool postfxActive(bool inWorld);

}  // namespace uc
