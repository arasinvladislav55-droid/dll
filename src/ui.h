#pragma once
struct ImFont;

namespace uc {

extern bool g_menuOpen;

void setFonts(ImFont* normal, ImFont* bold);
ImFont* fontNormal();
ImFont* fontBold();
void registerClick(int button);               // 0 — ЛКМ, 1 — ПКМ (для CPS)
void setMenuOpen(bool open);
bool needsFrame();                            // есть ли что рисовать (иначе кадр ImGui пропускается целиком)
/** Рисует HUD и (если открыто) меню поверх кадра. Вызывать между NewFrame и Render. */
void render(int width, int height, float dt, bool gameFocused, bool cursorCaptured);

}  // namespace uc
