#pragma once
// Модули, настройки, палитра и сохранение конфига. Никакой графики здесь нет.
#include <cstdint>
#include <string>
#include <vector>

namespace uc {

struct Setting {
    enum Type { Bool, Slider, Mode } type = Bool;
    std::string id, label, group;
    bool b = false;                        // Bool
    float f = 0, mn = 0, mx = 1, step = 1; // Slider
    std::string unit;
    int idx = 0;                           // Mode
    std::vector<std::string> opts;
    float anim = 0;                        // 0..1 — плавное переключение в UI
};

struct Module {
    std::string id, name, desc;
    int category = 0;                      // 0 — Визуалы, 1 — Клиент
    bool enabled = false;
    float anim = 0;
    std::vector<Setting> settings;
    Setting* find(const char* sid);
    bool on(const char* sid) { Setting* s = find(sid); return s && s->b; }
    float val(const char* sid, float def = 0) { Setting* s = find(sid); return s ? s->f : def; }
    int mode(const char* sid) { Setting* s = find(sid); return s ? s->idx : 0; }
};

struct Palette { uint32_t a, b, c; };      // 0xRRGGBB

extern std::vector<Module> g_modules;
extern Palette g_launcherPalette;          // цвета темы лаунчера (из uc_theme.txt)
extern std::wstring g_dir;                 // папка с dll: рядом лежат menu.cfg и uc_theme.txt

void initModules();
Module* findModule(const char* id);
Palette currentPalette();                  // с учётом выбранной в меню темы
void loadTheme();                          // читает uc_theme.txt: «ffb454 ff8a3d ffd98a»
void loadConfig();
void saveConfig();

}  // namespace uc
