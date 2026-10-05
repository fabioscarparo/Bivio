// The settings window, in the Windows 11 style: a navigation pane on the left, one page per section
// (General, Inputs, Shortcut) made of setting cards (icon, title, description, control on the right),
// Mica title bar, light/dark theme and accent colour from the system.
//
// Everything is drawn by hand with GDI rather than shipping the WinUI runtime:
//   - the window background, cards, labels and text-box frames are painted in WM_PAINT (Paint);
//   - buttons, dropdowns, toggles, modifier chips and navigation items are owner-drawn BUTTONs
//     (DrawControl), with hover tracked by a subclass (ControlProc);
//   - dropdowns open native popup menus, which Windows 11 already draws in its own style;
//   - text boxes are borderless EDIT controls placed inside a painted frame.
//
// There is no Save button: every change is written to Bivio.ini and applied at once (Apply), the way
// Windows 11 settings work. Text edits are applied when typing pauses or the box loses focus.

#include "app.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#define BACKDROP_MICA 2

#define APP_VERSION L"1.0"
#define MAX_ROWS 8
#define MAX_MONITORS 8
#define MAX_CARDS 8
#define APPLY_TIMER 1

// Glyphs of Segoe Fluent Icons (Windows 11), at the same code points in Segoe MDL2 Assets (Windows 10)
#define ICON_SETTINGS L"\xE713"
#define ICON_MONITOR L"\xE7F4"
#define ICON_KEYBOARD L"\xE765"
#define ICON_GLOBE L"\xE774"
#define ICON_POWER L"\xE7E8"
#define ICON_FILE L"\xE8E5"
#define ICON_SWITCH L"\xE8AB"
#define ICON_CHEVRON L"\xE70D"

enum { PAGE_GENERAL, PAGE_INPUTS, PAGE_SHORTCUT, PAGE_COUNT };

// Control IDs (WM_COMMAND). IDOK and IDCANCEL come from the dialog manager: Enter and Esc.
enum {
    ID_NAV = 900,        // + page
    ID_MONITOR = 1000,
    ID_ROW_CODE = 1100,  // + row
    ID_ROW_NAME = 1200,  // + row
    ID_CHIP = 1300,      // + modifier
    ID_KEY = 1400,
    ID_TARGET,
    ID_LANGUAGE,
    ID_AUTOSTART,
    ID_OPEN_FILE,
};

/// Input codes offered in the dropdowns (MCCS VCP 0x60). Any other code can still be set in the file.
static const struct {
    DWORD code;
    const wchar_t *label;
} commonInputs[] = {
    {15, L"DisplayPort 1"}, {16, L"DisplayPort 2 / USB-C"}, {17, L"HDMI 1"}, {18, L"HDMI 2"},
    {27, L"USB-C"},         {3, L"DVI"},                    {1, L"VGA"},
};

/// The shortcut's modifiers: RegisterHotKey flag, chip label, name in the settings file.
static const struct {
    UINT mod;
    const wchar_t *label, *spec;
} modifiers[] = {{MOD_CONTROL, L"Ctrl", L"ctrl"}, {MOD_ALT, L"Alt", L"alt"}, {MOD_SHIFT, L"Shift", L"shift"}, {MOD_WIN, L"Win", L"win"}};

// ---------------------------------------------------------------- theme

static struct {
    BOOL dark;
    COLORREF bg, layer, layerBorder, card, cardBorder, text, textSecondary, navHover, navSelected, control,
        controlHover, controlPressed, controlBorder, strongStroke, editFocus, accent, accentHover, accentPressed,
        onAccent, warning;
} th;

typedef int(WINAPI *SetPreferredAppModeFn)(int);
typedef BOOL(WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
typedef void(WINAPI *FlushMenuThemesFn)(void);
static AllowDarkModeForWindowFn allowDarkModeForWindow;

/// Lets popup menus (tray menu, dropdowns) follow the system light/dark mode.
///
/// Win32 menus have no public dark mode. uxtheme.dll exports, by ordinal only, the functions Explorer
/// uses: SetPreferredAppMode (135), AllowDarkModeForWindow (133) and FlushMenuThemes (136). They exist
/// since Windows 10 1903 (build 18362), so older builds are skipped; missing exports are a no-op.
/// Called at startup and again when the system theme changes.
void InitDarkMode(void) {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn getVersion = (RtlGetVersionFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW version = {.dwOSVersionInfoSize = sizeof(version)};
    if (!getVersion || getVersion(&version) || version.dwBuildNumber < 18362) return;
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    SetPreferredAppModeFn setPreferredAppMode = (SetPreferredAppModeFn)(void (*)(void))GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flushMenuThemes = (FlushMenuThemesFn)(void (*)(void))GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    allowDarkModeForWindow = (AllowDarkModeForWindowFn)(void (*)(void))GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
    if (setPreferredAppMode) setPreferredAppMode(1);  // AllowDark
    if (flushMenuThemes) flushMenuThemes();
}

/// Opts a window in to the dark menus set up by InitDarkMode().
void AllowDarkMode(HWND wnd) {
    if (allowDarkModeForWindow) allowDarkModeForWindow(wnd, TRUE);
}

/// `percentA`% of colour a over b: how WinUI's translucent fills look over a solid background.
static COLORREF Mix(COLORREF a, COLORREF b, int percentA) {
    return RGB((GetRValue(a) * percentA + GetRValue(b) * (100 - percentA)) / 100,
               (GetGValue(a) * percentA + GetGValue(b) * (100 - percentA)) / 100,
               (GetBValue(a) * percentA + GetBValue(b) * (100 - percentA)) / 100);
}

/// Windows 11 accent shades: AccentPalette holds Light3..Dark3 as RGBA.
static COLORREF AccentShade(int index, COLORREF fallback) {
    BYTE palette[32];
    DWORD size = sizeof(palette);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, NULL, palette, &size) != ERROR_SUCCESS || size < 32)
        return fallback;
    return RGB(palette[index * 4], palette[index * 4 + 1], palette[index * 4 + 2]);
}

/// Colours of the Windows 11 (Fluent) light and dark themes: the Mica fallback colour for the
/// navigation pane, a lighter layer for the page, cards on top of it. WinUI defines most of these as
/// translucent whites/blacks; here they are pre-blended over their background, since GDI paints opaque.
static void LoadTheme(void) {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    th.dark = light == 0;
    if (th.dark) {
        th.bg = RGB(0x20, 0x20, 0x20);
        th.layer = RGB(0x27, 0x27, 0x27);
        th.layerBorder = RGB(0x1B, 0x1B, 0x1B);
        th.card = RGB(0x2E, 0x2E, 0x2E);
        th.cardBorder = RGB(0x22, 0x22, 0x22);
        th.text = RGB(0xFF, 0xFF, 0xFF);
        th.textSecondary = RGB(0xCE, 0xCE, 0xCE);
        th.control = RGB(0x37, 0x37, 0x37);
        th.controlHover = RGB(0x3C, 0x3C, 0x3C);
        th.controlPressed = RGB(0x31, 0x31, 0x31);
        th.controlBorder = RGB(0x40, 0x40, 0x40);
        th.strongStroke = RGB(0x9F, 0x9F, 0x9F);
        th.editFocus = RGB(0x1F, 0x1F, 0x1F);
        th.accent = AccentShade(1, RGB(0x60, 0xCD, 0xFF));  // Light2
        th.onAccent = RGB(0, 0, 0);
        th.warning = RGB(0xFC, 0xE1, 0x00);
    } else {
        th.bg = RGB(0xF3, 0xF3, 0xF3);
        th.layer = RGB(0xF9, 0xF9, 0xF9);
        th.layerBorder = RGB(0xE5, 0xE5, 0xE5);
        th.card = RGB(0xFE, 0xFE, 0xFE);
        th.cardBorder = RGB(0xE5, 0xE5, 0xE5);
        th.text = RGB(0x1B, 0x1B, 0x1B);
        th.textSecondary = RGB(0x61, 0x61, 0x61);
        th.control = RGB(0xFB, 0xFB, 0xFB);
        th.controlHover = RGB(0xF5, 0xF5, 0xF5);
        th.controlPressed = RGB(0xF0, 0xF0, 0xF0);
        th.controlBorder = RGB(0xE0, 0xE0, 0xE0);
        th.strongStroke = RGB(0x8B, 0x8B, 0x8B);
        th.editFocus = RGB(0xFF, 0xFF, 0xFF);
        th.accent = AccentShade(4, RGB(0x00, 0x5F, 0xB8));  // Dark1
        th.onAccent = RGB(0xFF, 0xFF, 0xFF);
        th.warning = RGB(0x9D, 0x5D, 0x00);
    }
    th.navHover = Mix(th.text, th.bg, 5);
    th.navSelected = Mix(th.text, th.bg, 8);
    th.accentHover = Mix(th.accent, th.card, 90);
    th.accentPressed = Mix(th.accent, th.card, 80);
}

// ---------------------------------------------------------------- drawing

// The process is "system DPI aware" (see bivio.manifest): layout is in device-independent pixels at
// 96 DPI, scaled by S() to the system DPI read when the window opens.
static int dpi = 96;
static int S(int dip) { return MulDiv(dip, dpi, 96); }

static HFONT fontBody, fontStrong, fontCaption, fontTitle, fontIcon, fontNavIcon, fontCardIcon;

/// EnumFontFamiliesEx callback: any call means the font is installed.
static int CALLBACK FontFound(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found) {
    (void)lf;
    (void)tm;
    (void)type;
    *(BOOL *)found = TRUE;
    return 0;
}

static BOOL FontExists(const wchar_t *face) {
    LOGFONTW lf = {.lfCharSet = DEFAULT_CHARSET};
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    BOOL found = FALSE;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, FontFound, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static HFONT MakeFont(const wchar_t *face, int px, int weight) {
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

/// Windows 11 type ramp: Segoe UI Variable (Segoe UI before Windows 11), Segoe Fluent Icons for glyphs.
static void LoadFonts(void) {
    HDC dc = GetDC(NULL);
    dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    BOOL variable = FontExists(L"Segoe UI Variable Text");
    const wchar_t *text = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t *display = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t *icons = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    HFONT *fonts[] = {&fontBody, &fontStrong, &fontCaption, &fontTitle, &fontIcon, &fontNavIcon, &fontCardIcon};
    for (int i = 0; i < (int)ARRAYSIZE(fonts); i++)
        if (*fonts[i]) DeleteObject(*fonts[i]);
    fontBody = MakeFont(text, 14, FW_NORMAL);
    fontStrong = MakeFont(text, 14, FW_SEMIBOLD);
    fontCaption = MakeFont(text, 12, FW_NORMAL);
    fontTitle = MakeFont(display, 28, FW_SEMIBOLD);
    fontIcon = MakeFont(icons, 12, FW_NORMAL);
    fontNavIcon = MakeFont(icons, 16, FW_NORMAL);
    fontCardIcon = MakeFont(icons, 20, FW_NORMAL);
}

static void FillSolid(HDC dc, RECT rc, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
}

static BOOL InRoundRect(float x, float y, float l, float t, float r, float b, float radius) {
    if (x < l || x > r || y < t || y > b) return FALSE;
    float cx = x < l + radius ? l + radius : (x > r - radius ? r - radius : x);
    float cy = y < t + radius ? t + radius : (y > b - radius ? b - radius : y);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}

static DWORD Pixel(COLORREF c) { return (DWORD)(GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c)); }

/// Anti-aliased rounded rectangle with a `border`-pixel stroke, composited over a solid `backdrop`
/// (GDI's own RoundRect has jagged corners and no anti-aliasing).
///
/// Rendered into a 32-bit DIB and blitted. Straight edges are pixel-aligned, so only the four corner
/// squares need anti-aliasing: there each pixel is supersampled 3x3 and blended by how many samples
/// fall inside the outer and inner shapes. Everywhere else the pixel is plainly stroke or fill.
static void RoundRectAA(HDC dc, RECT rc, int radius, int border, COLORREF fill, COLORREF stroke, COLORREF backdrop) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = w, .biHeight = -h, .biPlanes = 1,
                                   .biBitCount = 32, .biCompression = BI_RGB}};
    DWORD *px;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp) return;
    const int ss = 3;  // supersampling per axis
    const DWORD fillPx = Pixel(fill), strokePx = Pixel(stroke);
    float inner = radius - border > 0 ? (float)(radius - border) : 0;
    for (int y = 0; y < h; y++) {
        BOOL cornerRow = y <= radius || y >= h - radius - 1;
        for (int x = 0; x < w; x++) {
            if (!cornerRow || (x > radius && x < w - radius - 1)) {
                BOOL edge = x < border || y < border || x >= w - border || y >= h - border;
                px[y * w + x] = edge ? strokePx : fillPx;
                continue;
            }
            int outer = 0, in = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    float fx = x + (sx + 0.5f) / ss, fy = y + (sy + 0.5f) / ss;
                    outer += InRoundRect(fx, fy, 0, 0, (float)w, (float)h, (float)radius);
                    in += InRoundRect(fx, fy, (float)border, (float)border, (float)(w - border), (float)(h - border), inner);
                }
            }
            int n = ss * ss, bgW = n - outer, strokeW = outer - in;
            int r = (GetRValue(backdrop) * bgW + GetRValue(stroke) * strokeW + GetRValue(fill) * in) / n;
            int g = (GetGValue(backdrop) * bgW + GetGValue(stroke) * strokeW + GetGValue(fill) * in) / n;
            int b = (GetBValue(backdrop) * bgW + GetBValue(stroke) * strokeW + GetBValue(fill) * in) / n;
            px[y * w + x] = (DWORD)((r << 16) | (g << 8) | b);
        }
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(dc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
}

static void DrawLabel(HDC dc, const wchar_t *text, RECT rc, HFONT font, COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | format);
    SelectObject(dc, old);
}

// ---------------------------------------------------------------- controls

typedef enum {
    K_PUSH,      // standard button
    K_CHIP,      // toggle button (shortcut modifier): accent-filled when on
    K_DROPDOWN,  // text + chevron; opens a popup menu
    K_TOGGLE,    // "On/Off" label + switch
    K_NAV,       // navigation pane item: icon + label, accent pill when selected
} Kind;

/// An owner-drawn BUTTON styled like a WinUI control. Its state lives here; the window's
/// GWLP_USERDATA points back to it.
typedef struct {
    HWND hwnd;
    Kind kind;
    BOOL on, hover;
    COLORREF *backdrop;  // colour painted behind it (card or pane), a pointer so theme changes apply
    const wchar_t *icon;  // navigation items only
    wchar_t text[96];
} Control;

/// A setting card: icon, title and description on the left, controls placed on its right.
typedef struct {
    RECT rect;
    int textRight;  // where the controls start
    const wchar_t *icon, *title, *description;
} Card;

/// Everything the window shows: its controls, the values being edited, and the layout of the page.
static struct {
    HWND wnd;  // NULL when the window is closed
    int page;
    Control nav[PAGE_COUNT];
    Control monitor, rowCode[MAX_ROWS], chips[4], key, target, language, autostart, openFile;
    HWND rowName[MAX_ROWS];
    HWND focusedEdit;
    HBRUSH editBrush, editFocusBrush;
    int visibleRows;

    // values being edited
    wchar_t match[64];
    DWORD rowCodes[MAX_ROWS], targetCode;
    UINT keyVk;  // 0 = no shortcut
    int languageIndex;  // 0 auto, 1 it, 2 en
    wchar_t monitorIds[MAX_MONITORS][16], monitorNames[MAX_MONITORS][128];
    int monitorCount;

    // layout of the current page, in pixels
    int paneWidth;
    RECT title, description, editFrames[MAX_ROWS];
    Card cards[MAX_CARDS];
    int cardCount;
} ui;

/// Subclass of every owner-drawn control: tracks hover (buttons don't report it), and skips the
/// background erase, since DrawControl paints every pixel (no flicker).
static LRESULT CALLBACK ControlProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)data;
    Control *c = (Control *)GetWindowLongPtrW(wnd, GWLP_USERDATA);
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!c->hover) {
            c->hover = TRUE;
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, wnd, 0};
            TrackMouseEvent(&track);
            InvalidateRect(wnd, NULL, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        c->hover = FALSE;
        InvalidateRect(wnd, NULL, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(wnd, ControlProc, id);
        break;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

/// Creates a hidden owner-drawn control (Layout() shows the ones of the current page).
static void CreateControl(Control *c, int id, Kind kind, const wchar_t *text, COLORREF *backdrop) {
    c->kind = kind;
    c->on = c->hover = FALSE;  // the struct is static: clear state left by a previous window
    c->backdrop = backdrop;
    lstrcpynW(c->text, text, ARRAYSIZE(c->text));
    c->hwnd = CreateWindowExW(0, L"BUTTON", c->text, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, ui.wnd,
                              (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(c->hwnd, GWLP_USERDATA, (LONG_PTR)c);
    SetWindowSubclass(c->hwnd, ControlProc, 0, 0);
}

static void SetControlText(Control *c, const wchar_t *text) {
    lstrcpynW(c->text, text, ARRAYSIZE(c->text));
    SetWindowTextW(c->hwnd, c->text);  // for screen readers
    InvalidateRect(c->hwnd, NULL, FALSE);
}

/// WM_DRAWITEM: paints a control into an off-screen bitmap, then blits it in one go.
/// ODS_NOFOCUSRECT is set while keyboard cues are hidden, so the focus ring shows only for keyboard use.
static void DrawControl(const DRAWITEMSTRUCT *d) {
    Control *c = (Control *)GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA);
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    RECT rc = {0, 0, w, h};
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    COLORREF backdrop = *c->backdrop;
    FillSolid(dc, rc, backdrop);

    BOOL pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED;
    BOOL focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    COLORREF text = disabled ? th.textSecondary : th.text;

    switch (c->kind) {
    case K_PUSH:
    case K_CHIP:
    case K_DROPDOWN: {
        BOOL accent = c->kind == K_CHIP && c->on;
        COLORREF fill = accent ? (pressed ? th.accentPressed : c->hover ? th.accentHover : th.accent)
                               : (pressed ? th.controlPressed : c->hover && !disabled ? th.controlHover : th.control);
        COLORREF stroke = focus ? th.text : accent ? fill : th.controlBorder;
        RoundRectAA(dc, rc, S(4), focus ? S(2) : 1, fill, stroke, backdrop);
        if (accent) text = th.onAccent;
        if (c->kind == K_DROPDOWN) {
            RECT label = {S(11), 0, w - S(34), h}, chevron = {w - S(30), 0, w - S(11), h};
            DrawLabel(dc, c->text, label, fontBody, text, DT_LEFT);
            DrawLabel(dc, ICON_CHEVRON, chevron, fontIcon, th.textSecondary, DT_RIGHT);
        } else {
            DrawLabel(dc, c->text, rc, fontBody, pressed && !accent ? th.textSecondary : text, DT_CENTER);
        }
        break;
    }
    case K_TOGGLE: {
        // "On"/"Off" text, then a 40x20 switch at the right edge.
        RECT sw = {w - S(40), (h - S(20)) / 2, w, (h - S(20)) / 2 + S(20)};
        RECT label = {0, 0, sw.left - S(12), h};
        DrawLabel(dc, c->on ? T(L"Attivato", L"On") : T(L"Disattivato", L"Off"), label, fontBody, text, DT_RIGHT);
        int knob = c->hover ? S(14) : S(12), cy = (sw.top + sw.bottom) / 2;
        if (c->on) {
            COLORREF fill = c->hover ? th.accentHover : th.accent;
            RoundRectAA(dc, sw, S(10), 1, fill, focus ? th.text : fill, backdrop);
            RECT k = {sw.right - S(4) - knob, cy - knob / 2, sw.right - S(4), cy - knob / 2 + knob};
            RoundRectAA(dc, k, knob / 2, 1, th.onAccent, th.onAccent, fill);
        } else {
            COLORREF fill = c->hover ? Mix(th.text, backdrop, 6) : backdrop;
            RoundRectAA(dc, sw, S(10), 1, fill, focus ? th.text : th.strongStroke, backdrop);
            RECT k = {sw.left + S(4), cy - knob / 2, sw.left + S(4) + knob, cy - knob / 2 + knob};
            RoundRectAA(dc, k, knob / 2, 1, th.strongStroke, th.strongStroke, fill);
        }
        break;
    }
    case K_NAV: {
        // Selected: subtle fill plus the accent pill on the left, as in WinUI NavigationView.
        COLORREF fill = c->on || pressed ? th.navSelected : c->hover ? th.navHover : backdrop;
        RoundRectAA(dc, rc, S(4), 1, fill, focus ? th.text : fill, backdrop);
        if (c->on) {
            RECT pill = {0, (h - S(16)) / 2, S(3), (h - S(16)) / 2 + S(16)};
            RoundRectAA(dc, pill, S(2), 1, th.accent, th.accent, fill);
        }
        RECT icon = {S(12), 0, S(36), h}, label = {S(44), 0, w - S(8), h};
        DrawLabel(dc, c->icon, icon, fontNavIcon, th.text, DT_CENTER);
        DrawLabel(dc, c->text, label, fontBody, th.text, DT_LEFT);
        break;
    }
    }

    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------------------------------------------------------------- values shown on the controls

static void CodeLabel(DWORD code, wchar_t *out) {
    for (int i = 0; i < (int)ARRAYSIZE(commonInputs); i++)
        if (commonInputs[i].code == code) {
            wsprintfW(out, L"%s (%lu)", commonInputs[i].label, code);
            return;
        }
    wsprintfW(out, T(L"Codice %lu", L"Code %lu"), code);
}

static void KeyLabel(UINT vk, wchar_t *out) {
    if (!vk) lstrcpyW(out, T(L"Nessuno", L"None"));
    else if (vk >= VK_F1 && vk <= VK_F12) wsprintfW(out, L"F%u", vk - VK_F1 + 1);
    else wsprintfW(out, L"%c", (wchar_t)vk);
}

static const wchar_t *LanguageLabel(int index) {
    return index == 1 ? L"Italiano" : index == 2 ? L"English" : T(L"Automatica (sistema)", L"Automatic (system)");
}

static const wchar_t *PageTitle(int page) {
    return page == PAGE_GENERAL ? T(L"Generale", L"General")
         : page == PAGE_INPUTS  ? T(L"Ingressi", L"Inputs")
                                : T(L"Scorciatoia", L"Shortcut");
}

static void RowName(int row, wchar_t *out, int size) {
    GetWindowTextW(ui.rowName[row], out, size);
    wchar_t *trimmed = Trim(out);
    if (trimmed != out) MoveMemory(out, trimmed, (lstrlenW(trimmed) + 1) * sizeof(wchar_t));
}

/// Updates the texts and states of the controls from the values being edited.
static void RefreshValues(void) {
    wchar_t text[160];
    if (!ui.match[0]) lstrcpyW(text, T(L"Tutti i monitor", L"All monitors"));
    else {
        wsprintfW(text, T(L"%s (non collegato)", L"%s (not connected)"), ui.match);
        for (int i = 0; i < ui.monitorCount; i++)
            if (!lstrcmpiW(ui.monitorIds[i], ui.match)) wsprintfW(text, L"%s (%s)", ui.monitorNames[i], ui.monitorIds[i]);
    }
    SetControlText(&ui.monitor, text);

    for (int i = 0; i < MAX_ROWS; i++) {
        CodeLabel(ui.rowCodes[i], text);
        SetControlText(&ui.rowCode[i], text);
    }

    KeyLabel(ui.keyVk, text);
    SetControlText(&ui.key, text);

    CodeLabel(ui.targetCode, text);
    for (int i = 0; i < ui.visibleRows; i++) {
        wchar_t name[64];
        RowName(i, name, ARRAYSIZE(name));
        if (name[0] && ui.rowCodes[i] == ui.targetCode) {
            lstrcpyW(text, name);
            break;
        }
    }
    SetControlText(&ui.target, text);
    EnableWindow(ui.target.hwnd, ui.keyVk != 0);

    SetControlText(&ui.language, LanguageLabel(ui.languageIndex));
    for (int i = 0; i < 4; i++) InvalidateRect(ui.chips[i].hwnd, NULL, FALSE);
    for (int i = 0; i < PAGE_COUNT; i++) InvalidateRect(ui.nav[i].hwnd, NULL, FALSE);
    InvalidateRect(ui.autostart.hwnd, NULL, FALSE);
    InvalidateRect(ui.wnd, NULL, FALSE);  // the shortcut warning lives on the page
}

/// Texts set once on the controls, refreshed when the language changes.
static void RefreshTexts(void) {
    SetWindowTextW(ui.wnd, T(L"Bivio - Impostazioni", L"Bivio - Settings"));
    for (int i = 0; i < PAGE_COUNT; i++) SetControlText(&ui.nav[i], PageTitle(i));
    SetControlText(&ui.openFile, T(L"Apri", L"Open"));
    for (int i = 0; i < MAX_ROWS; i++)
        SendMessageW(ui.rowName[i], EM_SETCUEBANNER, TRUE, (LPARAM)T(L"Nome nel menu", L"Name in the menu"));
}

// ---------------------------------------------------------------- layout

static void Show(HWND wnd, BOOL visible) { ShowWindow(wnd, visible ? SW_SHOWNA : SW_HIDE); }  // no activation

static void Place(HWND wnd, int x, int y, int w, int h) {
    SetWindowPos(wnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    Show(wnd, TRUE);
}

/// Adds a card and returns its rectangle.
static RECT AddCard(int top, int height, const wchar_t *icon, const wchar_t *title, const wchar_t *description) {
    RECT client;
    GetClientRect(ui.wnd, &client);
    Card *c = &ui.cards[ui.cardCount++];
    c->rect = (RECT){ui.paneWidth + S(40), top, client.right - S(40), top + height};
    c->textRight = c->rect.right - S(16);
    c->icon = icon;
    c->title = title;
    c->description = description;
    return c->rect;
}

/// Places a control at the right edge of the last card, vertically centered, and narrows its text.
static void PlaceInCard(HWND wnd, int width) {
    Card *c = &ui.cards[ui.cardCount - 1];
    int height = S(32), x = c->textRight - width;
    Place(wnd, x, (c->rect.top + c->rect.bottom - height) / 2, width, height);
    c->textRight = x - S(16);
}

/// Positions the controls of the current page (others are hidden), Windows 11 Settings proportions.
static void Layout(void) {
    const int cardH = S(68), rowH = S(60), gap = S(4), ctrlH = S(32);
    RECT client;
    GetClientRect(ui.wnd, &client);

    // Navigation pane
    for (int i = 0; i < PAGE_COUNT; i++) {
        ui.nav[i].on = i == ui.page;
        Place(ui.nav[i].hwnd, S(4), S(8) + i * S(40), ui.paneWidth - S(8), S(36));
    }

    // Hide every page control, then show those of the current page.
    HWND pageControls[] = {ui.monitor.hwnd, ui.language.hwnd, ui.autostart.hwnd, ui.openFile.hwnd, ui.key.hwnd,
                           ui.target.hwnd, ui.chips[0].hwnd, ui.chips[1].hwnd, ui.chips[2].hwnd, ui.chips[3].hwnd};
    for (int i = 0; i < (int)ARRAYSIZE(pageControls); i++) Show(pageControls[i], FALSE);
    for (int i = 0; i < MAX_ROWS; i++) {
        Show(ui.rowName[i], FALSE);
        Show(ui.rowCode[i].hwnd, FALSE);
    }

    int left = ui.paneWidth + S(40);
    ui.title = (RECT){left, S(24), client.right - S(40), S(64)};
    ui.description = (RECT){left, S(66), client.right - S(40), S(88)};
    ui.cardCount = 0;
    int y = S(104);

    TEXTMETRICW tm;
    HDC dc = GetDC(ui.wnd);
    HGDIOBJ old = SelectObject(dc, fontBody);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(ui.wnd, dc);

    if (ui.page == PAGE_GENERAL) {
        RECT r = AddCard(y, cardH, ICON_MONITOR, T(L"Monitor", L"Monitor"),
                         T(L"Il monitor che riceve i comandi", L"The monitor that receives the commands"));
        PlaceInCard(ui.monitor.hwnd, S(280));
        y = r.bottom + gap;
        r = AddCard(y, cardH, ICON_GLOBE, T(L"Lingua", L"Language"),
                    T(L"Lingua dell'app e del file delle impostazioni", L"Language of the app and of the settings file"));
        PlaceInCard(ui.language.hwnd, S(220));
        y = r.bottom + gap;
        r = AddCard(y, cardH, ICON_POWER, T(L"Avvia con Windows", L"Start with Windows"),
                    T(L"Avvia Bivio quando accedi", L"Start Bivio when you sign in"));
        PlaceInCard(ui.autostart.hwnd, S(160));
        y = r.bottom + gap;
        AddCard(y, cardH, ICON_FILE, T(L"File delle impostazioni", L"Settings file"),
                T(L"Per le opzioni avanzate, come codici d'ingresso insoliti",
                  L"For advanced options, such as unusual input codes"));
        PlaceInCard(ui.openFile.hwnd, S(100));
    } else if (ui.page == PAGE_INPUTS) {
        for (int i = 0; i < ui.visibleRows; i++) {
            RECT r = AddCard(y, rowH, ICON_MONITOR, NULL, NULL);
            int top = r.top + (rowH - ctrlH) / 2;
            Place(ui.rowCode[i].hwnd, r.right - S(16) - S(240), top, S(240), ctrlH);
            ui.editFrames[i] = (RECT){r.left + S(56), top, r.right - S(16) - S(240) - S(12), top + ctrlH};
            Place(ui.rowName[i], ui.editFrames[i].left + S(11), top + (ctrlH - tm.tmHeight) / 2,
                  ui.editFrames[i].right - ui.editFrames[i].left - S(22), tm.tmHeight);
            y = r.bottom + gap;
        }
    } else {
        RECT r = AddCard(y, cardH, ICON_KEYBOARD, T(L"Combinazione", L"Keys"),
                         T(L"Funziona da qualsiasi app", L"Works from any app"));
        PlaceInCard(ui.key.hwnd, S(80));
        ui.cards[ui.cardCount - 1].textRight += S(4);  // chips sit 12 px from the key, 4 px apart
        for (int i = 3; i >= 0; i--) {
            PlaceInCard(ui.chips[i].hwnd, S(56));
            ui.cards[ui.cardCount - 1].textRight += S(12);
        }
        ui.cards[ui.cardCount - 1].textRight -= S(12);
        y = r.bottom + gap;
        AddCard(y, cardH, ICON_SWITCH, T(L"Passa a", L"Switch to"),
                T(L"L'ingresso scelto dalla scorciatoia", L"The input selected by the shortcut"));
        PlaceInCard(ui.target.hwnd, S(240));
    }
    InvalidateRect(ui.wnd, NULL, FALSE);
}

/// WM_PAINT: everything that is not a child control, drawn off-screen and blitted at once.
static void Paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(wnd, &ps);
    RECT client;
    GetClientRect(wnd, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    // Navigation pane on the window background; the page on a lighter layer with a rounded corner.
    FillSolid(dc, client, th.bg);
    RECT layer = {ui.paneWidth, 0, client.right + S(16), client.bottom + S(16)};
    RoundRectAA(dc, layer, S(8), 1, th.layer, th.layerBorder, th.bg);
    RECT version = {S(16), client.bottom - S(40), ui.paneWidth - S(8), client.bottom - S(16)};
    DrawLabel(dc, L"Bivio " APP_VERSION, version, fontCaption, th.textSecondary, DT_LEFT);

    DrawLabel(dc, PageTitle(ui.page), ui.title, fontTitle, th.text, DT_LEFT);
    const wchar_t *description = NULL;
    COLORREF descriptionColor = th.textSecondary;
    if (ui.page == PAGE_GENERAL) {
        description = T(L"Il monitor da comandare, la lingua e l'avvio automatico.",
                        L"The monitor to control, the language and automatic start.");
    } else if (ui.page == PAGE_INPUTS) {
        description = T(L"Le voci del menu. Per aggiungerne una compila la riga vuota; per toglierla cancellane il nome.",
                        L"The menu entries. To add one, fill in the empty row; to remove one, clear its name.");
    } else if (cfg.hotkeySpec[0] && !HotkeyRegistered()) {
        description = T(L"Questa combinazione non è valida o è già usata da un'altra app.",
                        L"This combination is invalid or already used by another app.");
        descriptionColor = th.warning;
    } else {
        description = T(L"Cambia ingresso con la tastiera, anche quando Bivio è nascosto.",
                        L"Switch input from the keyboard, even when Bivio is hidden.");
    }
    DrawLabel(dc, description, ui.description, fontBody, descriptionColor, DT_LEFT);

    for (int i = 0; i < ui.cardCount; i++) {
        Card *c = &ui.cards[i];
        RECT r = c->rect;
        RoundRectAA(dc, r, S(6), 1, th.card, th.cardBorder, th.layer);
        RECT icon = {r.left + S(16), r.top, r.left + S(40), r.bottom};
        DrawLabel(dc, c->icon, icon, fontCardIcon, th.text, DT_CENTER);
        if (c->title) {
            int mid = (r.top + r.bottom) / 2;
            DrawLabel(dc, c->title, (RECT){r.left + S(56), mid - S(20), c->textRight, mid}, fontBody, th.text, DT_LEFT);
            DrawLabel(dc, c->description, (RECT){r.left + S(56), mid + S(1), c->textRight, mid + S(18)}, fontCaption,
                      th.textSecondary, DT_LEFT);
        }
    }

    // Text boxes: rounded frame, darker bottom edge, accent underline when focused.
    if (ui.page == PAGE_INPUTS) {
        for (int i = 0; i < ui.visibleRows; i++) {
            RECT f = ui.editFrames[i];
            BOOL focused = ui.focusedEdit == ui.rowName[i];
            RoundRectAA(dc, f, S(4), 1, focused ? th.editFocus : th.control, th.controlBorder, th.card);
            int line = focused ? S(2) : 1;
            FillSolid(dc, (RECT){f.left + S(3), f.bottom - line, f.right - S(3), f.bottom},
                      focused ? th.accent : Mix(th.strongStroke, th.controlBorder, 50));
        }
    }

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(wnd, &ps);
}

// ---------------------------------------------------------------- dropdown menus (Windows 11 popup menus)

/// Shows `menu` under the control and returns the chosen item's id (0 if dismissed).
static UINT Popup(Control *c, HMENU menu) {
    RECT r;
    GetWindowRect(c->hwnd, &r);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, r.left,
                                r.bottom + S(2), ui.wnd, NULL);
    DestroyMenu(menu);
    return cmd;
}

/// Adds a menu item; `newColumn` starts a new column (used to lay out the key list).
static void Append(HMENU menu, UINT id, const wchar_t *text, BOOL checked, BOOL newColumn) {
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (newColumn ? MF_MENUBARBREAK : 0), id, text);
}

static void ChooseMonitor(void) {
    HMENU m = CreatePopupMenu();
    wchar_t text[160];
    BOOL known = !ui.match[0];
    Append(m, 1, T(L"Tutti i monitor", L"All monitors"), !ui.match[0], FALSE);
    for (int i = 0; i < ui.monitorCount; i++) {
        BOOL current = !lstrcmpiW(ui.monitorIds[i], ui.match);
        known |= current;
        wsprintfW(text, L"%s (%s)", ui.monitorNames[i], ui.monitorIds[i]);
        Append(m, 2 + i, text, current, FALSE);
    }
    if (!known) {
        wsprintfW(text, T(L"%s (non collegato)", L"%s (not connected)"), ui.match);
        Append(m, 100, text, TRUE, FALSE);
    }
    UINT cmd = Popup(&ui.monitor, m);
    if (cmd == 1) ui.match[0] = 0;
    else if (cmd >= 2 && cmd < 2 + (UINT)ui.monitorCount) lstrcpyW(ui.match, ui.monitorIds[cmd - 2]);
}

static void ChooseCode(int row) {
    HMENU m = CreatePopupMenu();
    wchar_t text[96];
    BOOL known = FALSE;
    for (int i = 0; i < (int)ARRAYSIZE(commonInputs); i++) {
        BOOL current = commonInputs[i].code == ui.rowCodes[row];
        known |= current;
        CodeLabel(commonInputs[i].code, text);
        Append(m, 1 + i, text, current, FALSE);
    }
    if (!known) {
        CodeLabel(ui.rowCodes[row], text);
        Append(m, 100, text, TRUE, FALSE);
    }
    UINT cmd = Popup(&ui.rowCode[row], m);
    if (cmd >= 1 && cmd <= ARRAYSIZE(commonInputs)) ui.rowCodes[row] = commonInputs[cmd - 1].code;
}

static void ChooseKey(void) {
    HMENU m = CreatePopupMenu();
    wchar_t text[8];
    Append(m, 1, T(L"Nessuno", L"None"), !ui.keyVk, FALSE);
    // Columns: A-M, N-Z, 0-9, F1-F12 (ids are 100 + virtual key)
    for (UINT vk = 'A'; vk <= 'Z'; vk++) {
        KeyLabel(vk, text);
        Append(m, 100 + vk, text, vk == ui.keyVk, vk == 'N');
    }
    for (UINT vk = '0'; vk <= '9'; vk++) {
        KeyLabel(vk, text);
        Append(m, 100 + vk, text, vk == ui.keyVk, vk == '0');
    }
    for (UINT vk = VK_F1; vk <= VK_F12; vk++) {
        KeyLabel(vk, text);
        Append(m, 100 + vk, text, vk == ui.keyVk, vk == VK_F1);
    }
    UINT cmd = Popup(&ui.key, m);
    if (cmd == 1) ui.keyVk = 0;
    else if (cmd >= 100) ui.keyVk = cmd - 100;
}

static void ChooseTarget(void) {
    HMENU m = CreatePopupMenu();
    BOOL known = FALSE;
    for (int i = 0; i < ui.visibleRows; i++) {
        wchar_t name[64];
        RowName(i, name, ARRAYSIZE(name));
        if (!name[0]) continue;
        BOOL current = ui.rowCodes[i] == ui.targetCode && !known;
        known |= current;
        Append(m, 1 + i, name, current, FALSE);
    }
    if (!known) {
        wchar_t text[96];
        CodeLabel(ui.targetCode, text);
        Append(m, 100, text, TRUE, FALSE);
    }
    UINT cmd = Popup(&ui.target, m);
    if (cmd >= 1 && cmd <= MAX_ROWS) ui.targetCode = ui.rowCodes[cmd - 1];
}

static void ChooseLanguage(void) {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < 3; i++) Append(m, 1 + i, LanguageLabel(i), i == ui.languageIndex, FALSE);
    UINT cmd = Popup(&ui.language, m);
    if (cmd) ui.languageIndex = cmd - 1;
}

// ---------------------------------------------------------------- window

/// Writes what the window shows to the settings file and applies it right away: hotkey, start with
/// Windows, and the language of the window itself.
static void Apply(void) {
    static const wchar_t *languages[] = {L"auto", L"it", L"en"};
    BOOL wasItalian = italian;
    lstrcpyW(cfg.language, languages[ui.languageIndex]);
    lstrcpynW(cfg.match, ui.match, ARRAYSIZE(cfg.match));
    cfg.inputCount = 0;
    for (int i = 0; i < ui.visibleRows && cfg.inputCount < MAX_INPUTS; i++) {
        RowName(i, cfg.inputs[cfg.inputCount].name, ARRAYSIZE(cfg.inputs[0].name));
        if (cfg.inputs[cfg.inputCount].name[0]) cfg.inputs[cfg.inputCount++].code = ui.rowCodes[i];
    }
    cfg.hotkeySpec[0] = 0;
    BOOL anyModifier = FALSE;
    for (int i = 0; i < 4; i++) anyModifier |= ui.chips[i].on;
    if (ui.keyVk && anyModifier) {
        for (int i = 0; i < 4; i++)
            if (ui.chips[i].on) {
                lstrcatW(cfg.hotkeySpec, modifiers[i].spec);
                lstrcatW(cfg.hotkeySpec, L"+");
            }
        wchar_t key[8];
        KeyLabel(ui.keyVk, key);
        CharLowerW(key);
        lstrcatW(cfg.hotkeySpec, key);
    }
    wsprintfW(cfg.targetSpec, L"%lu", ui.targetCode);

    WriteConfig();
    LoadConfig();
    ApplyHotkey();
    if (ui.autostart.on != IsAutostart()) SetAutostart(ui.autostart.on);
    if (italian != wasItalian) {
        RefreshTexts();
        Layout();
    }
    RefreshValues();
}

static void OnClick(int id) {
    if (id >= ID_NAV && id < ID_NAV + PAGE_COUNT) {
        ui.page = id - ID_NAV;
        Layout();
        RefreshValues();
        return;
    }
    if (id == ID_MONITOR) ChooseMonitor();
    else if (id >= ID_ROW_CODE && id < ID_ROW_CODE + MAX_ROWS) ChooseCode(id - ID_ROW_CODE);
    else if (id >= ID_CHIP && id < ID_CHIP + 4) ui.chips[id - ID_CHIP].on ^= TRUE;
    else if (id == ID_KEY) ChooseKey();
    else if (id == ID_TARGET) ChooseTarget();
    else if (id == ID_LANGUAGE) ChooseLanguage();
    else if (id == ID_AUTOSTART) ui.autostart.on ^= TRUE;
    else if (id == ID_OPEN_FILE) {
        // Close the window first: it would otherwise overwrite the user's edits with its own values on
        // the next change. Then open the file in Notepad.
        SendMessageW(ui.wnd, WM_CLOSE, 0, 0);
        ShellExecuteW(NULL, L"open", L"notepad.exe", iniPath, NULL, SW_SHOWNORMAL);
        return;
    } else if (id == IDCANCEL) {  // Esc
        SendMessageW(ui.wnd, WM_CLOSE, 0, 0);
        return;
    } else {
        return;  // Enter in a text box (IDOK): nothing to confirm, changes already apply
    }
    Apply();
}

/// Applies the theme to the window: dark title bar, Mica backdrop (visible in the title bar; the client
/// area is painted opaque), dark menus, and the brushes behind the text boxes.
static void ApplyWindowTheme(void) {
    BOOL dark = th.dark;
    int backdrop = BACKDROP_MICA;  // Windows 11 22H2+; ignored elsewhere
    DwmSetWindowAttribute(ui.wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(ui.wnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    AllowDarkMode(ui.wnd);
    if (ui.editBrush) {
        DeleteObject(ui.editBrush);
        DeleteObject(ui.editFocusBrush);
    }
    ui.editBrush = CreateSolidBrush(th.control);
    ui.editFocusBrush = CreateSolidBrush(th.editFocus);
}

static LRESULT CALLBACK SettingsProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        Paint(wnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        DrawControl((const DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLOREDIT: {  // colours of the borderless text boxes, to match their painted frame
        BOOL focused = (HWND)lp == ui.focusedEdit;
        SetTextColor((HDC)wp, th.text);
        SetBkColor((HDC)wp, focused ? th.editFocus : th.control);
        return (LRESULT)(focused ? ui.editFocusBrush : ui.editBrush);
    }
    case WM_COMMAND: {
        if (!ui.wnd) return 0;  // children notifying while the window is being destroyed
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_ROW_NAME && id < ID_ROW_NAME + MAX_ROWS) {
            int row = id - ID_ROW_NAME;
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
                ui.focusedEdit = code == EN_SETFOCUS ? (HWND)lp : NULL;
                InvalidateRect(wnd, &ui.editFrames[row], FALSE);
                InvalidateRect((HWND)lp, NULL, TRUE);
                if (code == EN_KILLFOCUS) {
                    KillTimer(wnd, APPLY_TIMER);
                    Apply();
                }
            } else if (code == EN_CHANGE) {
                wchar_t name[64];
                RowName(row, name, ARRAYSIZE(name));
                if (row == ui.visibleRows - 1 && name[0] && ui.visibleRows < MAX_ROWS) {  // keep one empty row
                    ui.visibleRows++;
                    Layout();
                }
                SetTimer(wnd, APPLY_TIMER, 600, NULL);  // apply once typing pauses
            }
        } else if (code == BN_CLICKED || code == BN_DOUBLECLICKED) {
            OnClick(id);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == APPLY_TIMER) {
            KillTimer(wnd, APPLY_TIMER);
            Apply();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpW((const wchar_t *)lp, L"ImmersiveColorSet")) {
            LoadTheme();
            ApplyWindowTheme();
            RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
        }
        return 0;
    case WM_CLOSE:
        if (KillTimer(wnd, APPLY_TIMER)) Apply();  // a text edit still waiting for the typing pause
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        ui.wnd = NULL;
        ui.focusedEdit = NULL;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

HWND SettingsWindow(void) { return ui.wnd; }

/// Opens the settings window, or brings it to the front if it is already open.
void ShowSettings(void) {
    if (ui.wnd) {
        ShowWindow(ui.wnd, SW_SHOWNORMAL);
        SetForegroundWindow(ui.wnd);
        return;
    }
    LoadConfig();
    LoadTheme();
    LoadFonts();

    HINSTANCE instance = GetModuleHandleW(NULL);
    static BOOL registered;
    if (!registered) {
        WNDCLASSEXW wc = {.cbSize = sizeof(wc), .lpfnWndProc = SettingsProc, .hInstance = instance,
                          .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)), .hCursor = LoadCursorW(NULL, IDC_ARROW),
                          .lpszClassName = L"BivioSettings"};
        RegisterClassExW(&wc);
        registered = TRUE;
    }
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT size = {0, 0, S(900), S(660)};
    AdjustWindowRectEx(&size, style, FALSE, 0);
    ui.wnd = CreateWindowExW(0, L"BivioSettings", L"", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             size.right - size.left, size.bottom - size.top, NULL, NULL, instance, NULL);
    ApplyWindowTheme();
    ui.paneWidth = S(240);
    ui.page = PAGE_GENERAL;

    // Current values
    ui.languageIndex = !lstrcmpiW(cfg.language, L"it") ? 1 : !lstrcmpiW(cfg.language, L"en") ? 2 : 0;
    lstrcpynW(ui.match, cfg.match, ARRAYSIZE(ui.match));
    ui.visibleRows = cfg.inputCount + 1 < MAX_ROWS ? cfg.inputCount + 1 : MAX_ROWS;
    ui.keyVk = cfg.hotkeyValid ? cfg.hotkeyVk : 0;
    ui.targetCode = cfg.hotkeyTarget ? cfg.hotkeyTarget : cfg.inputCount ? cfg.inputs[0].code : 15;
    ui.monitorCount = ListMonitors(ui.monitorIds, ui.monitorNames, MAX_MONITORS);

    // Controls
    static COLORREF *onCard = &th.card, *onPane = &th.bg;
    static const wchar_t *navIcons[PAGE_COUNT] = {ICON_SETTINGS, ICON_MONITOR, ICON_KEYBOARD};
    for (int i = 0; i < PAGE_COUNT; i++) {
        CreateControl(&ui.nav[i], ID_NAV + i, K_NAV, L"", onPane);
        ui.nav[i].icon = navIcons[i];
    }
    CreateControl(&ui.monitor, ID_MONITOR, K_DROPDOWN, L"", onCard);
    CreateControl(&ui.language, ID_LANGUAGE, K_DROPDOWN, L"", onCard);
    CreateControl(&ui.autostart, ID_AUTOSTART, K_TOGGLE, L"", onCard);
    ui.autostart.on = IsAutostart();
    CreateControl(&ui.openFile, ID_OPEN_FILE, K_PUSH, L"", onCard);
    for (int i = 0; i < MAX_ROWS; i++) {
        ui.rowName[i] = CreateWindowExW(0, L"EDIT", i < cfg.inputCount ? cfg.inputs[i].name : L"",
                                        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, ui.wnd,
                                        (HMENU)(INT_PTR)(ID_ROW_NAME + i), instance, NULL);
        SendMessageW(ui.rowName[i], WM_SETFONT, (WPARAM)fontBody, FALSE);
        SendMessageW(ui.rowName[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(ui.rowName[i], EM_LIMITTEXT, 63, 0);
        ui.rowCodes[i] = i < cfg.inputCount ? cfg.inputs[i].code : 15;
        CreateControl(&ui.rowCode[i], ID_ROW_CODE + i, K_DROPDOWN, L"", onCard);
    }
    for (int i = 0; i < 4; i++) {
        CreateControl(&ui.chips[i], ID_CHIP + i, K_CHIP, modifiers[i].label, onCard);
        ui.chips[i].on = cfg.hotkeyValid && (cfg.hotkeyMods & modifiers[i].mod);
    }
    CreateControl(&ui.key, ID_KEY, K_DROPDOWN, L"", onCard);
    CreateControl(&ui.target, ID_TARGET, K_DROPDOWN, L"", onCard);

    RefreshTexts();
    Layout();
    RefreshValues();

    // Centered on the monitor under the mouse
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    RECT w;
    GetWindowRect(ui.wnd, &w);
    SetWindowPos(ui.wnd, NULL, (mi.rcWork.left + mi.rcWork.right - (w.right - w.left)) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - (w.bottom - w.top)) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    ShowWindow(ui.wnd, SW_SHOWNORMAL);
    SetForegroundWindow(ui.wnd);
    SetFocus(ui.nav[0].hwnd);
}
