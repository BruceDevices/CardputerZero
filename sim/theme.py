"""Theme: colors, accents and layout — ported 1:1 from src/view/ui_const.h
and src/view/theme.h. Pure data, no pygame, so the catalog and the renderer
share one source of truth (same split as the C++)."""

SCREEN_W, SCREEN_H = 320, 170
TITLEBAR_H = 30
NAVBAR_H = 30

def rgb(hexval):
    return ((hexval >> 16) & 0xFF, (hexval >> 8) & 0xFF, hexval & 0xFF)

# src/view/ui_const.h :: color::light / color::dark
LIGHT = {
    "primary": rgb(0x8a2be2), "info": rgb(0xb026ff), "body": rgb(0xf5f2fb),
    "card": rgb(0xffffff), "action": rgb(0xece4fb), "button": rgb(0xf0eaf9),
    "border": rgb(0xd8c7f2), "text": rgb(0x1c0f2b), "disabled": rgb(0xb3a6c9),
}
DARK = {
    "primary": rgb(0xb026ff), "info": rgb(0xd48bff), "body": rgb(0x0a0612),
    "card": rgb(0x150c24), "action": rgb(0x0d0818), "button": rgb(0x1c1030),
    "border": rgb(0x3a2a52), "text": rgb(0xe9defc), "disabled": rgb(0x5c4e73),
}

# src/model/tool_catalog.cpp :: accent_presets()  (name, primary, info)
ACCENTS = [
    ("Purple (Bruce)", 0xB026FF, 0xD48BFF), ("Violet", 0x7C4DFF, 0xB39DFF),
    ("Blue", 0x2B6BFF, 0x7AA6FF), ("Cyan", 0x00C8D4, 0x6FF0F7),
    ("Teal", 0x1BC4A6, 0x79E8D4), ("Green", 0x24D17A, 0x7FF0B5),
    ("Lime", 0x9CCC00, 0xCDF06A), ("Yellow", 0xF2C200, 0xFFE066),
    ("Orange", 0xFF8A1F, 0xFFB366), ("Red", 0xFF3B30, 0xFF8A82),
    ("Pink", 0xFF3EA5, 0xFF9FD0), ("White", 0xD7D7E4, 0xFFFFFF),
]


class Theme:
    """Live palette: a light/dark base plus the selected accent (the accent
    overrides primary/info, exactly like view::set_accent)."""

    def __init__(self, dark=True, accent=0):
        self.dark = dark
        self.accent = accent

    @property
    def c(self):
        base = dict(DARK if self.dark else LIGHT)
        name, primary, info = ACCENTS[self.accent]
        base["primary"] = rgb(primary)
        base["info"] = rgb(info)
        return base
