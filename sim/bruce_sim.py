#!/usr/bin/env python3
"""Bruce CardputerZero — pygame simulator.

A desktop port of the LVGL app in ../src. Renders at the device's real
320x170 LCD resolution (scaled up for the desktop) and reproduces the UI:
boot splash, the purple/black category carousel, per-category tool lists, the
live tool-run log, and the About page.

Controls (same mapping as src/platform/linux_input.cpp):
    ESC                     slot 0  — Back (quits at the root menu)
    F / Z / Up / Left       slot 1  — Prev / scroll up
    Enter                   slot 2  — Select / Run-Stop
    X / C / Down / Right     slot 3  — Next / scroll down
    T                       slot 4  — Light/Dark toggle (or Clear on the run log)
You can also click the five on-screen nav buttons.

Nothing here touches real hardware or networks: tool output is simulated.
Pass --real to subprocess the plain tool rows (curl, df, date, ...) on this
machine; device-only rows (radios, CAP, Evil Portal) stay simulated regardless.
"""

import os
import sys
import time

try:
    import pygame
except ModuleNotFoundError:
    sys.exit("pygame is not installed. Easiest: run  ./sim/run.sh  (sets up a "
             "venv for you).\nOr:  python3 -m venv sim/.venv && sim/.venv/bin/pip "
             "install pygame && sim/.venv/bin/python sim/bruce_sim.py")

import theme as T
import catalog as C
import status
from runner import ProcessRunner

SCALE = 3
FONTS_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "fonts")


def _font(name, size):
    path = os.path.join(FONTS_DIR, name)
    if os.path.exists(path):
        return pygame.font.Font(path, size)
    return pygame.font.SysFont("sans", size)       # fallback if assets missing


class App:
    def __init__(self, real=False):
        pygame.init()
        pygame.display.set_caption("Bruce CardputerZero (simulator)")
        self.surface = pygame.Surface((T.SCREEN_W, T.SCREEN_H))
        self.window = pygame.display.set_mode((T.SCREEN_W * SCALE, T.SCREEN_H * SCALE))
        self.clock = pygame.time.Clock()

        self.f_title = _font("inter-semibold.ttf", 14)
        self.f_body = _font("inter-medium.ttf", 11)
        self.f_small = _font("inter-regular.ttf", 9)
        self.f_icon = _font("Phosphor-Fill.ttf", 52)
        self.f_statusicon = _font("Phosphor-Fill.ttf", 13)
        self.f_clock = _font("inter-bold.ttf", 54)

        self.theme = T.Theme(dark=True, accent=0)
        self.runner = ProcessRunner(real=real)

        self.screen = "boot"
        self.boot_t = time.time()
        self.cat = 0            # carousel index
        self.item = 0           # tool-list index
        self.scroll = 0         # list / log scroll offset
        self.log = []           # run-log lines
        self.action = None      # currently viewed action
        self.opt_stack = []     # interactive options menu: [[title, items, idx], ...]
        self.detail = None      # (title, lines) for the detail screen

    # ── navbar: each screen declares its 5 slots (label, handler|None) ──────
    def nav_slots(self):
        tog = ("Dark" if not self.theme.dark else "Light", self.toggle_theme)
        if self.screen == "menu":
            return [("", None), ("Prev", self.prev), ("Select", self.select),
                    ("Next", self.next), tog]
        if self.screen == "tools":
            return [("Back", self.back), ("Up", self.prev), ("Select", self.select),
                    ("Down", self.next), tog]
        if self.screen == "options":
            return [("Back", self.opt_back), ("Up", self.prev), ("Select", self.select),
                    ("Down", self.next), tog]
        if self.screen == "detail":
            return [("Back", self.detail_back), ("Up", self.scroll_up), ("", None),
                    ("Down", self.scroll_down), tog]
        if self.screen == "run":
            label = "Stop" if self.runner.running() else "Run"
            return [("Back", self.back), ("Up", self.scroll_up), (label, self.run_stop),
                    ("Down", self.scroll_down), ("Clear", self.clear_log)]
        if self.screen == "clock":
            return [("Back", self.back), ("", None), ("", None), ("", None), tog]
        if self.screen == "about":
            return [("Back", self.back), ("", None), ("", None), ("", None), tog]
        return [("", None)] * 5

    # ── actions ─────────────────────────────────────────────────────────────
    def toggle_theme(self):
        self.theme.dark = not self.theme.dark

    def prev(self):
        if self.screen == "menu":
            self.cat = (self.cat - 1) % len(C.CATALOG)
        elif self.screen == "tools":
            self.item = max(0, self.item - 1)
        elif self.screen == "options":
            f = self.opt_stack[-1]
            f[2] = max(0, f[2] - 1)

    def next(self):
        if self.screen == "menu":
            self.cat = (self.cat + 1) % len(C.CATALOG)
        elif self.screen == "tools":
            self.item = min(len(C.CATALOG[self.cat].actions) - 1, self.item + 1)
        elif self.screen == "options":
            f = self.opt_stack[-1]
            f[2] = min(len(f[1]) - 1, f[2] + 1)

    def opt_back(self):
        self.opt_stack.pop()
        if not self.opt_stack:
            self.screen = "tools"

    def detail_back(self):
        self.screen = "options"

    def select(self):
        cat = C.CATALOG[self.cat]
        if self.screen == "menu":
            if cat.info_only:
                self.screen = "about"
            else:
                self.item, self.scroll, self.screen = 0, 0, "tools"
        elif self.screen == "tools":
            action = cat.actions[self.item]
            if action.nav:                           # Theme accents: apply, stay
                if self.cat == C.THEME_INDEX:
                    self.theme.accent = self.item
                return
            self.action = action
            if action.view == "clock":               # full-screen clock widget
                self.screen = "clock"
                return
            if action.items:                         # interactive drill-down
                self.opt_stack = [[action.label, action.items, 0]]
                self.screen = "options"
                return
            self.log = []
            self.scroll = 0
            self.screen = "run"
            if action.needs_root:
                self.log.append("[sim] this row runs as root on the device")
            self.runner.start(action)
        elif self.screen == "options":
            title, items, idx = self.opt_stack[-1]
            item = items[idx]
            if item.children:
                self.opt_stack.append([item.label, item.children, 0])
            else:
                self.detail = (item.label, item.lines)
                self.scroll = 0
                self.screen = "detail"

    def back(self):
        self.runner.stop()
        self.screen = "tools" if self.screen in ("run", "clock") else "menu"

    def run_stop(self):
        if self.runner.running():
            self.runner.stop()
        else:
            self.log = []
            self.scroll = 0
            self.runner.start(self.action)

    def clear_log(self):
        self.log = []
        self.scroll = 0

    def scroll_up(self):
        self.scroll = max(0, self.scroll - 1)

    def scroll_down(self):
        self.scroll += 1

    # ── input ────────────────────────────────────────────────────────────────
    KEYMAP = {                                       # linux_input.cpp mapping
        pygame.K_ESCAPE: 0, pygame.K_f: 1, pygame.K_z: 1, pygame.K_UP: 1, pygame.K_LEFT: 1,
        pygame.K_RETURN: 2, pygame.K_KP_ENTER: 2,
        pygame.K_x: 3, pygame.K_c: 3, pygame.K_DOWN: 3, pygame.K_RIGHT: 3,
        pygame.K_t: 4,
    }

    def handle_key(self, key):
        if self.screen == "boot":
            return
        slot = self.KEYMAP.get(key)
        if slot is None:
            return
        if slot == 0 and self.screen == "menu":      # Back at root quits
            self.quit = True
            return
        handler = self.nav_slots()[slot][1]
        if handler:
            handler()

    def handle_click(self, pos):
        if self.screen == "boot":
            return
        x, y = pos[0] // SCALE, pos[1] // SCALE
        if y < T.SCREEN_H - T.NAVBAR_H:
            return
        slot = x // (T.SCREEN_W // 5)
        if slot == 0 and self.screen == "menu":
            self.quit = True
            return
        handler = self.nav_slots()[min(slot, 4)][1]
        if handler:
            handler()

    # ── drawing ──────────────────────────────────────────────────────────────
    def _text(self, surf, text, font, color, pos, center=False, right=False):
        img = font.render(text, True, color)
        r = img.get_rect()
        if center:
            r.midtop = pos
        elif right:
            r.topright = pos
        else:
            r.topleft = pos
        surf.blit(img, r)
        return r

    def draw(self):
        c = self.theme.c
        s = self.surface
        s.fill(c["body"])
        if self.screen == "boot":
            self.draw_boot(s, c)
        else:
            self.draw_titlebar(s, c)
            {"menu": self.draw_menu, "tools": self.draw_tools,
             "run": self.draw_run, "about": self.draw_about, "clock": self.draw_clock,
             "options": self.draw_options, "detail": self.draw_detail}[self.screen](s, c)
            self.draw_navbar(s, c)
        pygame.transform.scale(s, self.window.get_size(), self.window)
        pygame.display.flip()

    def draw_boot(self, s, c):
        age = time.time() - self.boot_t
        alpha = min(255, int(age / 0.6 * 255))
        img = _font("inter-bold.ttf", 34).render("BRUCE", True, c["primary"])
        img.set_alpha(alpha)
        s.blit(img, img.get_rect(center=(T.SCREEN_W // 2, T.SCREEN_H // 2 - 8)))
        self._text(s, "CardputerZero  ·  simulator", self.f_small, c["info"],
                   (T.SCREEN_W // 2, T.SCREEN_H // 2 + 22), center=True)
        if age > 1.4:
            self.screen = "menu"

    def draw_titlebar(self, s, c):
        # Bruce drawStatusBar(): clock left, status icons center, battery right.
        w = (255, 255, 255)
        pygame.draw.rect(s, c["primary"], (0, 0, T.SCREEN_W, T.TITLEBAR_H))
        hhmm, ddmm = status.clock()
        self._text(s, hhmm, self.f_title, w, (8, 2))
        self._text(s, ddmm, self.f_small, w, (8, 18))
        left = 8 + max(self.f_title.size(hhmm)[0], self.f_small.size(ddmm)[0]) + 8

        wifi, ble, bat = status.status()
        right = T.SCREEN_W - 8
        if bat is not None:
            right = self.draw_battery(s, bat, right) - 6
        for on, glyph in ((ble, C.IC["bluetooth"]), (wifi, C.IC["wifi"])):
            if not on:
                continue
            img = self.f_statusicon.render(glyph, True, w)
            r = img.get_rect(midright=(right, T.TITLEBAR_H // 2))
            s.blit(img, r)
            right = r.left - 5

        titles = {"menu": "Bruce", "tools": C.CATALOG[self.cat].label,
                  "run": self.action.label if self.action else "", "about": "About",
                  "clock": "Clock",
                  "options": self.opt_stack[-1][0] if self.opt_stack else "",
                  "detail": self.detail[0] if self.detail else ""}
        self._fit_center(s, titles.get(self.screen, ""), self.f_title, w,
                         (left + right) // 2, 8, right - left)

    def draw_battery(self, s, bat, right):
        """Bruce's battery box: outline + proportional fill, red under 16%,
        charging shown as '+'. Returns the left edge of the % text."""
        pct, charging = bat
        bw, bh = 22, 11
        x, y = right - bw, 9
        col = (255, 70, 60) if pct < 16 else (255, 255, 255)
        pygame.draw.rect(s, col, (x, y, bw, bh), 1)
        pygame.draw.rect(s, col, (x + bw, y + 3, 2, bh - 6))              # nub
        pygame.draw.rect(s, col, (x + 1, y + 1, max(1, (bw - 2) * pct // 100), bh - 2))
        txt = ("+" if charging else "") + f"{pct}%"
        r = self._text(s, txt, self.f_small, col, (x - 4, 10), right=True)
        return r.left

    def _fit_center(self, s, text, font, color, cx, y, maxw):
        if not text or maxw <= 0:
            return
        if font.size(text)[0] <= maxw:
            self._text(s, text, font, color, (cx, y), center=True)
            return
        while text and font.size(text + "…")[0] > maxw:
            text = text[:-1]
        self._text(s, text + "…", font, color, (cx, y), center=True)

    def draw_navbar(self, s, c):
        y0 = T.SCREEN_H - T.NAVBAR_H
        pygame.draw.rect(s, c["button"], (0, y0, T.SCREEN_W, T.NAVBAR_H))
        pygame.draw.line(s, c["border"], (0, y0), (T.SCREEN_W, y0))
        w = T.SCREEN_W // 5
        for i, (label, handler) in enumerate(self.nav_slots()):
            if i:
                pygame.draw.line(s, c["border"], (i * w, y0 + 5), (i * w, T.SCREEN_H - 5))
            col = c["text"] if handler else c["disabled"]
            self._text(s, label, self.f_body, col, (i * w + w // 2, y0 + 9), center=True)

    def draw_menu(self, s, c):
        cat = C.CATALOG[self.cat]
        cx = T.SCREEN_W // 2
        self._text(s, cat.icon, self.f_icon, c["primary"], (cx, 36), center=True)
        self._text(s, cat.label, self.f_title, c["text"], (cx, 90), center=True)
        yend = self._wrap(s, cat.desc, self.f_small, c["disabled"], 108, cx, T.SCREEN_W - 40)
        self._text(s, f"{self.cat + 1} / {len(C.CATALOG)}", self.f_small, c["info"],
                   (cx, min(yend + 1, T.SCREEN_H - T.NAVBAR_H - 11)), center=True)
        self._text(s, "‹", self.f_title, c["info"], (14, 52))
        self._text(s, "›", self.f_title, c["info"], (T.SCREEN_W - 22, 52))

    def draw_tools(self, s, c):
        cat = C.CATALOG[self.cat]
        top, row_h = T.TITLEBAR_H + 4, 15
        rows = (T.SCREEN_H - top - T.NAVBAR_H - 16) // row_h
        if self.item < self.scroll:
            self.scroll = self.item
        elif self.item >= self.scroll + rows:
            self.scroll = self.item - rows + 1
        for i in range(self.scroll, min(len(cat.actions), self.scroll + rows)):
            a = cat.actions[i]
            y = top + (i - self.scroll) * row_h
            sel = i == self.item
            if sel:
                pygame.draw.rect(s, c["action"], (4, y - 1, T.SCREEN_W - 8, row_h))
            mark = ("● " if self.cat == C.THEME_INDEX and i == self.theme.accent else "")
            root = "  (root)" if a.needs_root else ""
            self._text(s, mark + a.label + root, self.f_body,
                       c["primary"] if sel else c["text"], (10, y))
        sel = cat.actions[self.item]
        pygame.draw.line(s, c["border"], (4, T.SCREEN_H - T.NAVBAR_H - 15),
                         (T.SCREEN_W - 4, T.SCREEN_H - T.NAVBAR_H - 15))
        self._text(s, sel.desc[:52], self.f_small, c["disabled"],
                   (10, T.SCREEN_H - T.NAVBAR_H - 12))

    def draw_options(self, s, c):
        title, items, idx = self.opt_stack[-1]
        top, row_h = T.TITLEBAR_H + 4, 15
        rows = (T.SCREEN_H - top - T.NAVBAR_H - 16) // row_h
        start = 0 if idx < rows else idx - rows + 1
        for i in range(start, min(len(items), start + rows)):
            it = items[i]
            y = top + (i - start) * row_h
            sel = i == idx
            if sel:
                pygame.draw.rect(s, c["action"], (4, y - 1, T.SCREEN_W - 8, row_h))
            arrow = "  ›" if it.children else ""
            self._text(s, it.label + arrow, self.f_body, c["primary"] if sel else c["text"], (10, y))
        cur = items[idx]
        hint = f"{len(cur.children)} options  ›" if cur.children else "Select to view details"
        pygame.draw.line(s, c["border"], (4, T.SCREEN_H - T.NAVBAR_H - 15),
                         (T.SCREEN_W - 4, T.SCREEN_H - T.NAVBAR_H - 15))
        self._text(s, f"{idx + 1}/{len(items)}   {hint}", self.f_small, c["disabled"],
                   (10, T.SCREEN_H - T.NAVBAR_H - 12))

    def draw_detail(self, s, c):
        _title, lines = self.detail
        top, row_h = T.TITLEBAR_H + 4, 12
        rows = (T.SCREEN_H - top - T.NAVBAR_H - 2) // row_h
        self.scroll = max(0, min(self.scroll, max(0, len(lines) - rows)))
        for i, line in enumerate(lines[self.scroll:self.scroll + rows]):
            col = c["info"] if line.startswith("[sim]") else c["text"]
            self._text(s, line[:60], self.f_small, col, (8, top + i * row_h))

    def draw_run(self, s, c):
        top = T.TITLEBAR_H + 3
        row_h = 11
        rows = (T.SCREEN_H - top - T.NAVBAR_H - 2) // row_h
        if self.runner.running():                    # autoscroll to tail
            self.scroll = max(0, len(self.log) - rows)
        self.scroll = max(0, min(self.scroll, max(0, len(self.log) - rows)))
        view = self.log[self.scroll:self.scroll + rows]
        for i, line in enumerate(view):
            col = c["info"] if line.startswith("[sim]") else c["text"]
            self._text(s, line[:60], self.f_small, col, (8, top + i * row_h))
        if self.runner.running():
            self._text(s, "● running", self.f_small, c["primary"],
                       (T.SCREEN_W - 8, top), right=True)

    def draw_clock(self, s, c):
        # Full-screen clock: big HH:MM in the accent, blinking colon, seconds
        # trailing small, weekday + date under it. Fills the body between bars.
        now = time.localtime()
        colon = ":" if int(time.time()) % 2 else " "
        hhmm = time.strftime(f"%H{colon}%M", now)
        area_h = T.SCREEN_H - T.TITLEBAR_H - T.NAVBAR_H
        cx, cy = T.SCREEN_W // 2, T.TITLEBAR_H + area_h // 2
        big = self.f_clock.render(hhmm, True, c["primary"])
        sec = self.f_body.render(time.strftime("%S", now), True, c["info"])
        gap, by = 4, cy - 12
        total = big.get_width() + gap + sec.get_width()
        br = big.get_rect(topleft=(cx - total // 2, by - big.get_height() // 2))
        s.blit(big, br)
        s.blit(sec, sec.get_rect(bottomleft=(br.right + gap, br.bottom - 6)))
        self._text(s, time.strftime("%A", now), self.f_body, c["info"],
                   (cx, br.bottom + 2), center=True)
        self._text(s, time.strftime("%d %B %Y", now), self.f_small, c["text"],
                   (cx, br.bottom + 16), center=True)

    def draw_about(self, s, c):
        y = T.TITLEBAR_H + 8
        for para in C.ABOUT.text.split("\n"):
            if not para:
                y += 6
                continue
            y = self._wrap(s, para, self.f_small, c["text"], y, T.SCREEN_W // 2,
                           T.SCREEN_W - 24, left=12) + 2

    def _wrap(self, s, text, font, color, y, cx, maxw, left=None):
        words, line = text.split(), ""
        for w in words:
            trial = (line + " " + w).strip()
            if font.size(trial)[0] > maxw and line:
                self._blit_line(s, line, font, color, y, cx, left)
                y += font.get_height()
                line = w
            else:
                line = trial
        if line:
            self._blit_line(s, line, font, color, y, cx, left)
            y += font.get_height()
        return y

    def _blit_line(self, s, line, font, color, y, cx, left):
        if left is None:
            self._text(s, line, font, color, (cx, y), center=True)
        else:
            self._text(s, line, font, color, (left, y))

    # ── main loop ────────────────────────────────────────────────────────────
    def run(self):
        self.quit = False
        while not self.quit:
            for e in pygame.event.get():
                if e.type == pygame.QUIT:
                    self.quit = True
                elif e.type == pygame.KEYDOWN:
                    self.handle_key(e.key)
                elif e.type == pygame.MOUSEBUTTONDOWN and e.button == 1:
                    self.handle_click(e.pos)
            if self.screen == "run":
                lines, _ = self.runner.drain()
                self.log += lines
            self.draw()
            self.clock.tick(60)
        self.runner.stop()
        pygame.quit()


if __name__ == "__main__":
    App(real="--real" in sys.argv).run()
