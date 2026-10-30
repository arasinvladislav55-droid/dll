#include "state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace uc {

std::vector<Module> g_modules;
Palette g_launcherPalette = {0xffb454, 0xff8a3d, 0xffd98a};
std::wstring g_dir;

Setting* Module::find(const char* sid) {
    for (auto& s : settings) if (s.id == sid) return &s;
    return nullptr;
}

Module* findModule(const char* id) {
    for (auto& m : g_modules) if (m.id == id) return &m;
    return nullptr;
}

// Те же цвета, что в Theme.java лаунчера (индекс 0 в меню — «как в лаунчере»)
static const Palette kThemes[] = {
    {0xffb454, 0xff8a3d, 0xffd98a},  // Янтарь
    {0x8b5cf6, 0x6366f1, 0x22d3ee},  // Фиолетовый
    {0x0ea5e9, 0x3b82f6, 0xa78bfa},  // Океан
    {0xec4899, 0xf43f5e, 0xfb923c},  // Роза
    {0x10b981, 0x14b8a6, 0xa3e635},  // Изумруд
    {0xf97316, 0xef4444, 0xfacc15},  // Закат
};

Palette currentPalette() {
    Module* ui = findModule("interface");
    Setting* t = ui ? ui->find("theme") : nullptr;
    if (!t || t->idx <= 0 || t->idx > 6) return g_launcherPalette;
    return kThemes[t->idx - 1];
}

static Setting boolS(const char* id, const char* label, const char* group, bool def) {
    Setting s; s.type = Setting::Bool; s.id = id; s.label = label; s.group = group; s.b = def; s.anim = def ? 1.f : 0.f;
    return s;
}
static Setting sliderS(const char* id, const char* label, const char* group, float def, float mn, float mx, float step, const char* unit) {
    Setting s; s.type = Setting::Slider; s.id = id; s.label = label; s.group = group;
    s.f = def; s.mn = mn; s.mx = mx; s.step = step; s.unit = unit;
    return s;
}
static Setting modeS(const char* id, const char* label, const char* group, std::vector<std::string> opts, int def) {
    Setting s; s.type = Setting::Mode; s.id = id; s.label = label; s.group = group; s.opts = std::move(opts); s.idx = def;
    return s;
}

static Module& addModule(const char* id, const char* name, const char* desc, int category, bool enabled) {
    Module m; m.id = id; m.name = name; m.desc = desc; m.category = category; m.enabled = enabled; m.anim = enabled ? 1.f : 0.f;
    g_modules.push_back(std::move(m));
    return g_modules.back();
}

// Модули-плашки: угол экрана и прозрачность фона есть у каждой
static Module& hud(const char* id, const char* name, const char* desc, bool enabled, int corner) {
    Module& m = addModule(id, name, desc, 0, enabled);
    m.settings.push_back(modeS("pos", "Угол экрана", "ОТОБРАЖЕНИЕ",
                               {"Верх слева", "Верх справа", "Низ слева", "Низ справа"}, corner));
    m.settings.push_back(sliderS("bg", "Прозрачность фона", "ОТОБРАЖЕНИЕ", 70, 0, 100, 5, "%"));
    return m;
}

void initModules() {
    if (!g_modules.empty()) return;

    hud("watermark", "Водяной знак", "Небольшая плашка с названием клиента в углу экрана", true, 0)
        .settings.push_back(boolS("fps", "Показывать FPS", "ВИД", true));
    {
        Module& m = hud("fps", "FPS", "Счётчик кадров. Полоса: зелёная — от 120, жёлто-зелёная — от 60, янтарная — от 30, красная — ниже", false, 0);
        m.settings.push_back(boolS("bar", "Цветная полоса", "ВИД", true));
    }
    {
        Module& m = hud("clock", "Часы", "Системное время на экране", false, 1);
        m.settings.push_back(boolS("h24", "24-часовой формат", "ВИД", true));
        m.settings.push_back(boolS("sec", "Показывать секунды", "ВИД", false));
    }
    {
        Module& m = hud("keys", "Клавиши и CPS", "Нажатые WASD, пробел и кнопки мыши со счётчиком кликов в секунду", false, 2);
        m.settings.push_back(boolS("space", "Пробел", "ВИД", true));
        m.settings.push_back(boolS("mouse", "Кнопки мыши и CPS", "ВИД", true));
    }
    {
        Module& m = addModule("crosshair", "Прицел", "Свой прицел по центру. Рисуется поверх ванильного", 0, false);
        m.settings.push_back(sliderS("len", "Длина", "РАЗМЕР", 6, 2, 20, 1, " px"));
        m.settings.push_back(sliderS("gap", "Зазор", "РАЗМЕР", 3, 0, 12, 1, " px"));
        m.settings.push_back(sliderS("thick", "Толщина", "РАЗМЕР", 2, 1, 4, 1, " px"));
        m.settings.push_back(boolS("dot", "Точка в центре", "ВИД", false));
        m.settings.push_back(modeS("color", "Цвет", "ВИД", {"Акцент", "Белый", "Зелёный", "Красный"}, 0));
    }
    {
        Module& m = addModule("vignette", "Виньетка", "Затемнение по краям экрана", 0, false);
        m.settings.push_back(sliderS("power", "Интенсивность", "ВИД", 45, 5, 100, 5, "%"));
    }
    {
        Module& m = addModule("edgeglow", "Свечение экрана", "Мягкое цветное свечение по краям экрана", 0, false);
        m.settings.push_back(sliderS("power", "Интенсивность", "ВИД", 40, 5, 100, 5, "%"));
    }

    {
        Module& m = addModule("interface", "Интерфейс", "Оформление меню под лаунчер: цвета, размер, анимации", 1, true);
        m.settings.push_back(modeS("theme", "Тема", "ЦВЕТА",
            {"Как в лаунчере", "Янтарь", "Фиолетовый", "Океан", "Роза", "Изумруд", "Закат"}, 0));
        m.settings.push_back(sliderS("scale", "Масштаб меню", "РАЗМЕР", 100, 70, 140, 5, "%"));
        m.settings.push_back(sliderS("opacity", "Непрозрачность", "РАЗМЕР", 96, 60, 100, 2, "%"));
        m.settings.push_back(boolS("anim", "Плавное открытие", "АНИМАЦИИ", true));
        m.settings.push_back(boolS("darken", "Затемнять игру", "АНИМАЦИИ", true));
        m.settings.push_back(boolS("glow", "Свечение вокруг меню", "АНИМАЦИИ", true));
    }
}

// ── цвета темы лаунчера: файл рядом с dll, его пишет лаунчер при каждом запуске ──
void loadTheme() {
    if (g_dir.empty()) return;
    std::ifstream f{std::filesystem::path(g_dir) / "uc_theme.txt"};
    if (!f) return;
    std::string tok[3];
    for (auto& t : tok) if (!(f >> t)) return;
    uint32_t v[3];
    for (int i = 0; i < 3; i++) {
        const char* p = tok[i].c_str();
        if (*p == '#') p++;
        char* end = nullptr;
        v[i] = (uint32_t)std::strtoul(p, &end, 16) & 0xFFFFFF;
        if (end == p) return;               // мусор — оставляем прежние цвета
    }
    g_launcherPalette = {v[0], v[1], v[2]};
}

// ── конфиг: простые строки «модуль.параметр=значение» ──
void saveConfig() {
    if (g_dir.empty()) return;
    std::ofstream f(std::filesystem::path(g_dir) / "menu.cfg", std::ios::trunc);
    if (!f) return;
    for (auto& m : g_modules) {
        f << m.id << ".enabled=" << (m.enabled ? 1 : 0) << "\n";
        for (auto& s : m.settings) {
            f << m.id << "." << s.id << "=";
            if (s.type == Setting::Bool) f << (s.b ? 1 : 0);
            else if (s.type == Setting::Slider) f << s.f;
            else f << s.idx;
            f << "\n";
        }
    }
}

void loadConfig() {
    if (g_dir.empty()) return;
    std::ifstream f{std::filesystem::path(g_dir) / "menu.cfg"};
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('='), dot = line.find('.');
        if (eq == std::string::npos || dot == std::string::npos || dot > eq) continue;
        Module* m = findModule(line.substr(0, dot).c_str());
        if (!m) continue;
        std::string key = line.substr(dot + 1, eq - dot - 1), val = line.substr(eq + 1);
        double v = std::atof(val.c_str());
        if (key == "enabled") { m->enabled = v != 0; m->anim = m->enabled ? 1.f : 0.f; continue; }
        Setting* s = m->find(key.c_str());
        if (!s) continue;
        if (s->type == Setting::Bool) { s->b = v != 0; s->anim = s->b ? 1.f : 0.f; }
        else if (s->type == Setting::Slider) s->f = (float)std::clamp(v, (double)s->mn, (double)s->mx);
        else s->idx = std::clamp((int)v, 0, (int)s->opts.size() - 1);
    }
}

}  // namespace uc
