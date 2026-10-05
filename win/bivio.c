// Bivio for Windows: a notification-area (tray) app that switches the monitor's input over DDC/CI.
//
//   click on the tray icon    menu with the inputs, Settings…, Start with Windows, Quit
//   global shortcut           switches to the input set as target (default Ctrl+Alt+Win+M)
//   Bivio.exe --input <code>  switches once and exits (exit code 0 = command sent)
//   Bivio.exe --list          message box with what the app sees for each monitor (diagnostics)
//
// DDC/CI goes through the Monitor Configuration API (dxva2.dll): every HMONITOR that matches the
// `match` setting is resolved to its physical monitor(s), which receive SetVCPFeature(0x60, code).
// The settings window lives in settings.c.

#include "app.h"

#include <stdarg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"  // per-user "start at sign-in"
#define WM_TRAY (WM_APP + 1)  // callback message of the tray icon
#define HOTKEY_ID 1
#define IDM_INPUT 100  // + index into cfg.inputs
#define IDM_SETTINGS 200
#define IDM_AUTOSTART 201
#define IDM_EXIT 202
#define VCP_INPUT_SOURCE 0x60  // MCCS "Input Source"

Config cfg;
BOOL italian;
wchar_t iniPath[MAX_PATH];
static wchar_t bakPath[MAX_PATH];
static HWND mainWnd;  // hidden window that owns the tray icon, the menu and the hotkey
static NOTIFYICONDATAW nid;
static UINT taskbarCreatedMsg;
static wchar_t appliedHotkeySpec[64] = L"\x01";  // spec of the last registration attempt; never a real spec
static BOOL hotkeyRegistered;

// ---------------------------------------------------------------- settings file text
// The file documents itself: these blocks are its comments, in Italian and English, and RenderConfig()
// puts the values between them. A file whose comments differ from these (written by an older version,
// or in the other language) is rewritten keeping its values. Keep them in sync with mac/Config.swift.

#define IT_TITLE L"; Bivio - Impostazioni"
#define EN_TITLE L"; Bivio - Settings"

#define IT_INTRO \
    L"; =============================================================================\r\n" \
    L";\r\n" \
    L"; Bivio cambia l'ingresso video del monitor inviandogli un comando DDC/CI:\r\n" \
    L"; lo stesso effetto di scegliere l'ingresso dal menu del monitor.\r\n" \
    L";\r\n" \
    L"; COME MODIFICARE QUESTO FILE\r\n" \
    L";   - Le righe che iniziano con \";\" sono commenti e vengono ignorate.\r\n" \
    L";   - Modifica solo il testo dopo il segno \"=\" (e i nomi in [inputs]).\r\n" \
    L";   - Salva il file: le modifiche valgono dalla prossima apertura del menu\r\n" \
    L";     di Bivio, senza riavviare l'app.\r\n" \
    L";   - Per tornare alle impostazioni iniziali cancella questo file:\r\n" \
    L";     verrà ricreato al prossimo avvio.\r\n" \
    L";\r\n" \
    L"; Posizione del file: %APPDATA%\\Bivio\\Bivio.ini\r\n" \
    L"\r\n\r\n" \
    L"[general]\r\n\r\n" \
    L"; Lingua dell'app e di questo file.\r\n" \
    L";   auto = lingua del sistema     it = italiano     en = English\r\n"

#define EN_INTRO \
    L"; =============================================================================\r\n" \
    L";\r\n" \
    L"; Bivio switches the monitor's video input by sending it a DDC/CI command:\r\n" \
    L"; the same as picking the input from the monitor's own menu.\r\n" \
    L";\r\n" \
    L"; HOW TO EDIT THIS FILE\r\n" \
    L";   - Lines starting with \";\" are comments and are ignored.\r\n" \
    L";   - Only change the text after the \"=\" sign (and the names in [inputs]).\r\n" \
    L";   - Save the file: changes apply the next time you open the Bivio\r\n" \
    L";     menu, no need to restart the app.\r\n" \
    L";   - To go back to the default settings, delete this file:\r\n" \
    L";     it is recreated on the next launch.\r\n" \
    L";\r\n" \
    L"; File location: %APPDATA%\\Bivio\\Bivio.ini\r\n" \
    L"\r\n\r\n" \
    L"[general]\r\n\r\n" \
    L"; Language of the app and of this file.\r\n" \
    L";   auto = system language     it = italiano     en = English\r\n"

#define IT_MONITOR \
    L"\r\n\r\n[monitor]\r\n\r\n" \
    L"; Il monitor da comandare. Puoi indicare:\r\n" \
    L";   - il suo ID (consigliato)                  es.  match=MSI4FA8\r\n" \
    L";   - una parte del nome                       es.  match=491CQPX\r\n" \
    L";   - niente, per comandare tutti i monitor:          match=\r\n" \
    L"; Nome e ID del monitor collegato compaiono in cima al menu di Bivio.\r\n"

#define EN_MONITOR \
    L"\r\n\r\n[monitor]\r\n\r\n" \
    L"; Which monitor to control. You can write:\r\n" \
    L";   - its ID (recommended)                     e.g.  match=MSI4FA8\r\n" \
    L";   - part of its name                         e.g.  match=491CQPX\r\n" \
    L";   - nothing, to control every monitor:              match=\r\n" \
    L"; The name and ID of the connected monitor are shown at the top of the menu.\r\n"

#define IT_INPUTS \
    L"\r\n\r\n[inputs]\r\n\r\n" \
    L"; Le voci del menu, una per riga, nella forma:\r\n" \
    L";   Nome da mostrare=codice dell'ingresso\r\n" \
    L"; Il nome è libero; il codice dice al monitor quale ingresso mostrare.\r\n" \
    L"; Puoi rinominare, riordinare, aggiungere o cancellare righe.\r\n" \
    L";\r\n" \
    L"; Codici più comuni (standard DDC/CI):\r\n" \
    L";   15 = DisplayPort 1              17 = HDMI 1\r\n" \
    L";   16 = DisplayPort 2 / USB-C      18 = HDMI 2\r\n" \
    L"; MSI MPG 491CQPX: DisplayPort 15, USB-C 16, HDMI 1 17, HDMI 2 18.\r\n"

#define EN_INPUTS \
    L"\r\n\r\n[inputs]\r\n\r\n" \
    L"; The menu entries, one per line, written as:\r\n" \
    L";   Name to show=input code\r\n" \
    L"; The name is up to you; the code tells the monitor which input to show.\r\n" \
    L"; You can rename, reorder, add or delete lines.\r\n" \
    L";\r\n" \
    L"; Most common codes (DDC/CI standard):\r\n" \
    L";   15 = DisplayPort 1              17 = HDMI 1\r\n" \
    L";   16 = DisplayPort 2 / USB-C      18 = HDMI 2\r\n" \
    L"; MSI MPG 491CQPX: DisplayPort 15, USB-C 16, HDMI 1 17, HDMI 2 18.\r\n"

#define IT_HOTKEY \
    L"\r\n\r\n[hotkey]\r\n\r\n" \
    L"; Scorciatoia da tastiera che cambia ingresso da qualsiasi app.\r\n" \
    L"; Uno o più modificatori più un tasto, separati da \"+\":\r\n" \
    L";   modificatori:  ctrl   alt   shift   win (= tasto Windows; cmd sul Mac)\r\n" \
    L";   tasto:         una lettera A-Z, una cifra 0-9, oppure F1 ... F12\r\n" \
    L"; Esempi:  ctrl+alt+win+m     ctrl+shift+f12\r\n" \
    L"; Lascia vuoto (keys=) per disattivare la scorciatoia.\r\n"

#define EN_HOTKEY \
    L"\r\n\r\n[hotkey]\r\n\r\n" \
    L"; Keyboard shortcut that switches the input from any app.\r\n" \
    L"; One or more modifiers plus one key, separated by \"+\":\r\n" \
    L";   modifiers:  ctrl   alt   shift   win (= Windows key; cmd on the Mac)\r\n" \
    L";   key:        a letter A-Z, a digit 0-9, or F1 ... F12\r\n" \
    L"; Examples:  ctrl+alt+win+m     ctrl+shift+f12\r\n" \
    L"; Leave empty (keys=) to disable the shortcut.\r\n"

#define IT_TARGET \
    L"\r\n; Codice dell'ingresso scelto dalla scorciatoia: uno dei codici in [inputs].\r\n" \
    L"; Di solito è l'ingresso dell'altro computer.\r\n"

#define EN_TARGET \
    L"\r\n; Code of the input selected by the shortcut: one of the codes in [inputs].\r\n" \
    L"; Usually the other computer's input.\r\n"

// ---------------------------------------------------------------- settings

/// Trims spaces and tabs (and a stray CR) in place; returns the first non-blank character.
wchar_t *Trim(wchar_t *s) {
    while (*s == L' ' || *s == L'\t') s++;
    wchar_t *end = s + lstrlenW(s);
    while (end > s && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'\r')) *--end = 0;
    return s;
}

/// Parses a spec such as "ctrl+alt+win+m" into RegisterHotKey's modifiers and virtual key. "cmd" is
/// accepted as a synonym of "win", so the same file works on the Mac. Needs at least one modifier and
/// exactly one key (A-Z, 0-9, F1-F12).
static BOOL ParseHotkey(const wchar_t *spec, UINT *mods, UINT *vk) {
    wchar_t buf[64];
    lstrcpynW(buf, spec, ARRAYSIZE(buf));
    *mods = 0;
    *vk = 0;
    for (wchar_t *part = buf; part;) {
        wchar_t *plus = wcschr(part, L'+');
        if (plus) *plus = 0;
        wchar_t *tok = Trim(part);
        int len = lstrlenW(tok), fn = 0;
        UINT key = 0;
        if (!lstrcmpiW(tok, L"ctrl") || !lstrcmpiW(tok, L"control")) *mods |= MOD_CONTROL;
        else if (!lstrcmpiW(tok, L"alt") || !lstrcmpiW(tok, L"option") || !lstrcmpiW(tok, L"opt")) *mods |= MOD_ALT;
        else if (!lstrcmpiW(tok, L"shift")) *mods |= MOD_SHIFT;
        else if (!lstrcmpiW(tok, L"win") || !lstrcmpiW(tok, L"cmd") || !lstrcmpiW(tok, L"command")) *mods |= MOD_WIN;
        else if (len == 1 && ((tok[0] >= L'a' && tok[0] <= L'z') || (tok[0] >= L'A' && tok[0] <= L'Z')))
            key = tok[0] & ~0x20;  // the virtual key code of a letter is its uppercase ASCII code
        else if (len == 1 && tok[0] >= L'0' && tok[0] <= L'9') key = tok[0];  // same for digits
        else if (len >= 2 && (tok[0] == L'f' || tok[0] == L'F') && (fn = _wtoi(tok + 1)) >= 1 && fn <= 12)
            key = VK_F1 + fn - 1;
        else return FALSE;
        if (key) {
            if (*vk) return FALSE;  // a second key
            *vk = key;
        }
        part = plus ? plus + 1 : NULL;
    }
    return *mods && *vk;
}

/// "Ctrl+Alt+Win+M" for the menu.
static void HotkeyLabel(wchar_t *out) {
    wchar_t key[8];
    if (cfg.hotkeyVk >= VK_F1 && cfg.hotkeyVk <= VK_F12) wsprintfW(key, L"F%u", cfg.hotkeyVk - VK_F1 + 1);
    else wsprintfW(key, L"%c", (wchar_t)cfg.hotkeyVk);
    wsprintfW(out, L"%s%s%s%s%s",
              cfg.hotkeyMods & MOD_CONTROL ? L"Ctrl+" : L"", cfg.hotkeyMods & MOD_ALT ? L"Alt+" : L"",
              cfg.hotkeyMods & MOD_SHIFT ? L"Shift+" : L"", cfg.hotkeyMods & MOD_WIN ? L"Win+" : L"", key);
}

static const wchar_t *InputName(DWORD code) {
    for (int i = 0; i < cfg.inputCount; i++)
        if (cfg.inputs[i].code == code) return cfg.inputs[i].name;
    return L"?";
}

/// "it", "en" or "auto": the Windows display language, falling back to English unless it is Italian.
static BOOL IsItalian(const wchar_t *setting) {
    if (!lstrcmpiW(setting, L"it")) return TRUE;
    if (!lstrcmpiW(setting, L"en")) return FALSE;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ITALIAN;
}

/// First-run settings, written for the MSI MPG 491CQPX the app was built for (see README).
static void SetDefaults(void) {
    static const InputSource defaults[] = {
        {L"PC (DisplayPort)", 15}, {L"Mac (USB-C)", 16}, {L"HDMI 1", 17}, {L"HDMI 2", 18}};
    lstrcpyW(cfg.language, L"auto");
    lstrcpyW(cfg.match, L"MSI4FA8");
    cfg.inputCount = ARRAYSIZE(defaults);
    CopyMemory(cfg.inputs, defaults, sizeof(defaults));
    lstrcpyW(cfg.hotkeySpec, L"ctrl+alt+win+m");
    lstrcpyW(cfg.targetSpec, L"16");
}

/// A fixed-capacity text buffer for building the file; appends that don't fit are dropped (the
/// buffers are sized well above the largest possible file).
typedef struct {
    wchar_t *buf;
    int len, cap;
} Text;

static void Cat(Text *t, const wchar_t *s) {
    int n = lstrlenW(s);
    if (t->len + n >= t->cap) return;
    CopyMemory(t->buf + t->len, s, (n + 1) * sizeof(wchar_t));
    t->len += n;
}

static void Catf(Text *t, const wchar_t *format, ...) {
    wchar_t line[1024];  // wvsprintf's own limit
    va_list args;
    va_start(args, format);
    wvsprintfW(line, format, args);
    va_end(args);
    Cat(t, line);
}

/// Copies an input name made safe for an [inputs] line: in "name=code" the name cannot contain "=" (the
/// first one ends the key), and a line starting with ";", "#" or "[" would read as a comment or a
/// section, so the input would vanish the next time the file is read.
static void IniSafeName(const wchar_t *name, wchar_t *out, int size) {
    while (*name == L' ' || *name == L'\t' || *name == L';' || *name == L'#' || *name == L'[') name++;
    int n = 0;
    for (; *name && n < size - 1; name++) out[n++] = *name == L'=' ? L'-' : *name;
    out[n] = 0;
    Trim(out);
}

/// The whole file: the documentation blocks with the current values between them.
static void RenderConfig(Text *t) {
    Cat(t, T(IT_TITLE L"\r\n" IT_INTRO, EN_TITLE L"\r\n" EN_INTRO));
    Catf(t, L"language=%s\r\n", cfg.language);
    Cat(t, T(IT_MONITOR, EN_MONITOR));
    Catf(t, L"match=%s\r\n", cfg.match);
    Cat(t, T(IT_INPUTS, EN_INPUTS));
    for (int i = 0; i < cfg.inputCount; i++) {
        wchar_t name[64];
        IniSafeName(cfg.inputs[i].name, name, ARRAYSIZE(name));
        if (name[0]) Catf(t, L"%s=%lu\r\n", name, cfg.inputs[i].code);
    }
    Cat(t, T(IT_HOTKEY, EN_HOTKEY));
    Catf(t, L"keys=%s\r\n", cfg.hotkeySpec);
    Cat(t, T(IT_TARGET, EN_TARGET));
    Catf(t, L"target=%s\r\n", cfg.targetSpec);
}

/// Writes the settings as UTF-16 with a byte order mark: the Win32 INI functions read Unicode only from
/// such files (anything else is read in the ANSI code page), and Notepad keeps the encoding on save.
void WriteConfig(void) {
    static wchar_t buf[12288];
    Text t = {buf, 0, ARRAYSIZE(buf)};
    buf[0] = 0;
    RenderConfig(&t);

    HANDLE f = CreateFileW(iniPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;  // settings stay in memory; the next write tries again
    const wchar_t bom = 0xFEFF;
    DWORD written;
    WriteFile(f, &bom, sizeof(bom), &written, NULL);
    WriteFile(f, buf, t.len * sizeof(wchar_t), &written, NULL);
    CloseHandle(f);
}

/// Appends the comment lines of `text` to `out`, one per line: the documentation of a file.
static void Comments(const wchar_t *text, Text *out) {
    while (*text) {
        const wchar_t *eol = wcspbrk(text, L"\r\n");
        int len = eol ? (int)(eol - text) : lstrlenW(text);
        if (*text == L';' && out->len + len + 1 < out->cap) {
            CopyMemory(out->buf + out->len, text, len * sizeof(wchar_t));
            out->len += len;
            out->buf[out->len++] = L'\n';
            out->buf[out->len] = 0;
        }
        text += len;
        while (*text == L'\r' || *text == L'\n') text++;
    }
}

/// TRUE if the file on disk is UTF-16 and carries the current documentation (same comment lines as
/// RenderConfig() would write now).
static BOOL DocumentationIsCurrent(void) {
    static wchar_t file[12288], rendered[12288], fileComments[12288], renderedComments[12288];
    HANDLE f = CreateFileW(iniPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD read = 0;
    ReadFile(f, file, sizeof(file) - sizeof(wchar_t), &read, NULL);
    CloseHandle(f);
    file[read / sizeof(wchar_t)] = 0;
    if (read < sizeof(wchar_t) || file[0] != 0xFEFF) return FALSE;

    Text r = {rendered, 0, ARRAYSIZE(rendered)}, a = {fileComments, 0, ARRAYSIZE(fileComments)},
         b = {renderedComments, 0, ARRAYSIZE(renderedComments)};
    rendered[0] = fileComments[0] = renderedComments[0] = 0;
    RenderConfig(&r);
    Comments(file + 1, &a);  // + 1: skip the byte order mark
    Comments(rendered, &b);
    return !lstrcmpW(fileComments, renderedComments);
}

/// %APPDATA%\Bivio\Bivio.ini (roaming profile: the settings follow the user).
static void InitPaths(void) {
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, iniPath);
    PathAppendW(iniPath, APP_NAME);
    CreateDirectoryW(iniPath, NULL);
    PathAppendW(iniPath, L"Bivio.ini");
    wsprintfW(bakPath, L"%s.bak", iniPath);
}

/// The app used to be called MSwitchIO: its settings and its start-with-Windows entry move to the new
/// name the first time Bivio runs.
static void MigrateFromOldName(void) {
    wchar_t oldDir[MAX_PATH], oldIni[MAX_PATH], oldBak[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, oldDir);
    PathAppendW(oldDir, L"MSwitchIO");
    wsprintfW(oldIni, L"%s\\MSwitchIO.ini", oldDir);
    wsprintfW(oldBak, L"%s.bak", oldIni);
    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES && MoveFileW(oldIni, iniPath)) {
        MoveFileW(oldBak, bakPath);
        RemoveDirectoryW(oldDir);  // fails, harmlessly, if something else is still in there
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    BOOL hadOldEntry = RegQueryValueExW(key, L"MSwitchIO", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    if (hadOldEntry) RegDeleteValueW(key, L"MSwitchIO");
    RegCloseKey(key);
    if (hadOldEntry) SetAutostart(TRUE);
}

/// Reads the settings into `cfg`. Creates the file with the defaults on first run (or when it is empty),
/// and rewrites it when its documentation is out of date, keeping the values; the previous file is
/// saved as Bivio.ini.bak. Called often (menu, hotkey, settings window), so edits made in a text editor
/// apply without restarting.
void LoadConfig(void) {
    // Windows may cache INI files: this call (all NULL but the file) flushes the cache for the file.
    WritePrivateProfileStringW(NULL, NULL, NULL, iniPath);
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(iniPath, GetFileExInfoStandard, &attributes) ||
        (!attributes.nFileSizeHigh && attributes.nFileSizeLow < 4)) {
        SetDefaults();
        italian = IsItalian(cfg.language);
        WriteConfig();
    } else {
        GetPrivateProfileStringW(L"general", L"language", L"auto", cfg.language, ARRAYSIZE(cfg.language), iniPath);
        GetPrivateProfileStringW(L"monitor", L"match", L"", cfg.match, ARRAYSIZE(cfg.match), iniPath);
        GetPrivateProfileStringW(L"hotkey", L"keys", L"", cfg.hotkeySpec, ARRAYSIZE(cfg.hotkeySpec), iniPath);
        GetPrivateProfileStringW(L"hotkey", L"target", L"", cfg.targetSpec, ARRAYSIZE(cfg.targetSpec), iniPath);

        // [inputs] has free-form keys, so read the whole section: "key=value\0key=value\0\0".
        // Comment lines come back too and are skipped. Codes are decimal or 0x-hex, within 16 bits.
        static wchar_t section[4096];
        cfg.inputCount = 0;
        GetPrivateProfileSectionW(L"inputs", section, ARRAYSIZE(section), iniPath);
        for (wchar_t *line = section; *line && cfg.inputCount < MAX_INPUTS;) {
            wchar_t *next = line + lstrlenW(line) + 1;  // computed before the line is cut at "="
            wchar_t *eq = wcschr(line, L'=');
            if (*line != L';' && *line != L'#' && eq) {
                *eq = 0;
                wchar_t *value = Trim(eq + 1), *end;
                DWORD code = wcstoul(value, &end, 0);
                if (code && code <= 0xFFFF && *value != L'-' && !*end) {
                    lstrcpynW(cfg.inputs[cfg.inputCount].name, Trim(line), ARRAYSIZE(cfg.inputs[0].name));
                    cfg.inputs[cfg.inputCount++].code = code;
                }
            }
            line = next;
        }

        italian = IsItalian(cfg.language);
        if (!DocumentationIsCurrent()) {
            CopyFileW(iniPath, bakPath, FALSE);
            WriteConfig();
        }
    }
    cfg.hotkeyValid = ParseHotkey(cfg.hotkeySpec, &cfg.hotkeyMods, &cfg.hotkeyVk);
    cfg.hotkeyTarget = wcstoul(cfg.targetSpec, NULL, 0);
    if (cfg.hotkeyTarget > 0xFFFF || Trim(cfg.targetSpec)[0] == L'-') cfg.hotkeyTarget = 0;  // not a VCP value
}

// ---------------------------------------------------------------- DDC/CI

/// PnP ID (e.g. "MSI4FA8") and driver name of a monitor. The ID comes from the device interface path,
/// "\\?\DISPLAY#MSI4FA8#5&1234&0&UID4353#{...}", and is the same the Mac app shows. The name is the
/// one of the monitor driver: "Generic PnP Monitor" unless the manufacturer's driver is installed.
static BOOL MonitorIdentity(HMONITOR monitor, wchar_t id[16], wchar_t name[128]) {
    MONITORINFOEXW info = {.cbSize = sizeof(info)};
    DISPLAY_DEVICEW device = {.cb = sizeof(device)};
    id[0] = name[0] = 0;
    // info.szDevice is the adapter output ("\\.\DISPLAY1"); device 0 on it is the monitor.
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info) ||
        !EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME))
        return FALSE;
    wchar_t *start = wcschr(device.DeviceID, L'#');
    if (start) {
        wchar_t *end = wcschr(++start, L'#');
        int len = end ? (int)(end - start) : lstrlenW(start);
        lstrcpynW(id, start, (len < 15 ? len : 15) + 1);
    }
    lstrcpynW(name, device.DeviceString, 128);
    return TRUE;
}

/// Whether the `match` setting selects this monitor (part of its ID or name, case-insensitive; an
/// empty setting selects every monitor). Also returns the monitor's ID and name.
static BOOL MonitorMatches(HMONITOR monitor, wchar_t id[16], wchar_t name[128]) {
    if (!MonitorIdentity(monitor, id, name)) return !cfg.match[0];
    return !cfg.match[0] || StrStrIW(id, cfg.match) || StrStrIW(name, cfg.match);
}

typedef struct {
    wchar_t (*ids)[16];
    wchar_t (*names)[128];
    int count, max;
} MonitorArray;

static BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param) {
    (void)dc;
    (void)rect;
    MonitorArray *a = (MonitorArray *)param;
    if (a->count < a->max && MonitorIdentity(monitor, a->ids[a->count], a->names[a->count]) && a->ids[a->count][0])
        a->count++;
    return TRUE;
}

/// Every connected monitor (all of them, ignoring "match"), for the settings window.
int ListMonitors(wchar_t ids[][16], wchar_t names[][128], int max) {
    MonitorArray a = {ids, names, 0, max};
    EnumDisplayMonitors(NULL, NULL, CollectMonitor, (LPARAM)&a);
    return a.count;
}

/// "Name (ID), Name (ID)" of the monitors the menu header shows.
typedef struct {
    wchar_t text[256];
    int count;
} MonitorList;

static BOOL CALLBACK ListMonitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param) {
    (void)dc;
    (void)rect;
    MonitorList *list = (MonitorList *)param;
    wchar_t id[16], name[128], entry[160];
    if (!MonitorMatches(monitor, id, name)) return TRUE;
    wsprintfW(entry, list->count++ ? L", %s (%s)" : L"%s (%s)", name, id);
    if (lstrlenW(list->text) + lstrlenW(entry) < (int)ARRAYSIZE(list->text)) lstrcatW(list->text, entry);
    return TRUE;
}

typedef struct {
    BYTE vcp;
    DWORD value;
    int matched, sent;  // monitors that match, and physical monitors that accepted the command
} SetVcpRequest;

/// Sends the request to the physical monitors behind one HMONITOR (one, unless displays are cloned).
static BOOL CALLBACK SetVcpOnMonitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param) {
    (void)dc;
    (void)rect;
    SetVcpRequest *req = (SetVcpRequest *)param;
    wchar_t id[16], name[128];
    if (!MonitorMatches(monitor, id, name)) return TRUE;
    req->matched++;
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || !count) return TRUE;
    PHYSICAL_MONITOR *physical = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*physical));
    if (!physical) return TRUE;
    if (GetPhysicalMonitorsFromHMONITOR(monitor, count, physical)) {
        for (DWORD i = 0; i < count; i++)
            if (SetVCPFeature(physical[i].hPhysicalMonitor, req->vcp, req->value)) req->sent++;
        DestroyPhysicalMonitors(count, physical);
    }
    HeapFree(GetProcessHeap(), 0, physical);
    return TRUE;
}

/// Switches every matching monitor to `code`. Returns NULL on success, otherwise the error to show.
/// DDC/CI has no reliable acknowledgment: success means the command went out, not that the monitor
/// acted on it.
static const wchar_t *SwitchInput(DWORD code) {
    SetVcpRequest req = {VCP_INPUT_SOURCE, code, 0, 0};
    EnumDisplayMonitors(NULL, NULL, SetVcpOnMonitor, (LPARAM)&req);
    if (!req.matched) return T(L"Monitor non trovato", L"Display not found");
    if (!req.sent) return T(L"Il monitor non ha accettato il comando DDC/CI", L"The display did not accept the DDC/CI command");
    return NULL;
}

/// One monitor of the "--list" report: identity, whether it matches, how many physical monitors DDC
/// reaches, the input the monitor reports (if it answers DDC reads) and the raw device path.
static BOOL CALLBACK DescribeMonitor(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM param) {
    (void)dc;
    (void)rect;
    wchar_t *report = (wchar_t *)param, line[512], id[16], name[128], current[48] = L"-";
    MONITORINFOEXW info = {.cbSize = sizeof(info)};
    DISPLAY_DEVICEW device = {.cb = sizeof(device)};
    GetMonitorInfoW(monitor, (MONITORINFO *)&info);
    EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME);
    BOOL matches = MonitorMatches(monitor, id, name);
    DWORD count = 0;
    GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count);
    PHYSICAL_MONITOR physical[4];
    if (count && count <= 4 && GetPhysicalMonitorsFromHMONITOR(monitor, count, physical)) {
        DWORD value = 0, maximum = 0;
        if (GetVCPFeatureAndVCPFeatureReply(physical[0].hPhysicalMonitor, VCP_INPUT_SOURCE, NULL, &value, &maximum))
            wsprintfW(current, L"%lu", value);
        DestroyPhysicalMonitors(count, physical);
    }
    wsprintfW(line, L"%s: %s (%s)\n  match: %s, DDC: %lu, %s: %s\n  %s\n\n", info.szDevice, name, id,
              matches ? L"\u2714" : L"\u2716", count, T(L"ingresso", L"input"), current, device.DeviceID);
    if (lstrlenW(report) + lstrlenW(line) < 4000) lstrcatW(report, line);
    return TRUE;
}

static void ShowDiagnostics(void) {
    static wchar_t report[4096];
    wsprintfW(report, L"%s\nmatch = \"%s\"\n\n", iniPath, cfg.match);
    EnumDisplayMonitors(NULL, NULL, DescribeMonitor, (LPARAM)report);
    MessageBoxW(NULL, report, APP_NAME, MB_ICONINFORMATION);
}

// ---------------------------------------------------------------- tray

/// A balloon notification from the tray icon (used for errors).
static void Notify(const wchar_t *text) {
    NOTIFYICONDATAW n = nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(n.szInfoTitle, APP_NAME, ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, text, ARRAYSIZE(n.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void DoSwitch(DWORD code) {
    const wchar_t *error = SwitchInput(code);
    if (error) Notify(error);
}

/// Adds the tray icon; called again when Explorer restarts (the icon is loaded only once).
static void AddTrayIcon(void) {
    nid.cbSize = sizeof(nid);
    nid.hWnd = mainWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    if (!nid.hIcon)  // icon resource 1, at the small-icon size of the system DPI
        nid.hIcon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpynW(nid.szTip, APP_NAME, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);
}

/// Registers the shortcut from the settings. Nothing to do when it is unchanged and registered; a failed
/// registration (invalid, or a combination another app already took) is retried on the next call,
/// but reported only once per spec, and not while the settings window shows the problem itself.
void ApplyHotkey(void) {
    BOOL specChanged = lstrcmpW(appliedHotkeySpec, cfg.hotkeySpec) != 0;
    if (!specChanged && (hotkeyRegistered || !cfg.hotkeyValid)) return;
    lstrcpynW(appliedHotkeySpec, cfg.hotkeySpec, ARRAYSIZE(appliedHotkeySpec));
    if (hotkeyRegistered) UnregisterHotKey(mainWnd, HOTKEY_ID);
    // MOD_NOREPEAT: holding the keys down switches once, not repeatedly.
    hotkeyRegistered = cfg.hotkeyValid &&
                       RegisterHotKey(mainWnd, HOTKEY_ID, cfg.hotkeyMods | MOD_NOREPEAT, cfg.hotkeyVk);
    if (specChanged && cfg.hotkeySpec[0] && !hotkeyRegistered && !SettingsWindow()) {
        wchar_t text[160];
        wsprintfW(text, T(L"Scorciatoia \"%s\" non valida o già in uso", L"Shortcut \"%s\" is invalid or already in use"),
                  cfg.hotkeySpec);
        Notify(text);
    }
}

BOOL HotkeyRegistered(void) { return hotkeyRegistered; }

/// The command line Windows runs at sign-in: this executable, quoted.
static void AutostartCommand(wchar_t *out) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wsprintfW(out, L"\"%s\"", exe);
}

/// TRUE if Windows starts this very executable at sign-in. An entry left by a copy that was moved or
/// deleted does not count, so turning the option on again fixes it.
BOOL IsAutostart(void) {
    wchar_t value[MAX_PATH + 2], expected[MAX_PATH + 2];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, value, &size) != ERROR_SUCCESS)
        return FALSE;
    AutostartCommand(expected);
    return !lstrcmpiW(value, expected);
}

/// Adds or removes the per-user Run entry (no administrator rights needed).
void SetAutostart(BOOL enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t command[MAX_PATH + 2];
        AutostartCommand(command);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)command, (lstrlenW(command) + 1) * sizeof(wchar_t));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}

/// The tray menu, built fresh each time from the settings file (so edits made by hand show up).
static void ShowMenu(void) {
    LoadConfig();
    ApplyHotkey();

    HMENU menu = CreatePopupMenu();
    wchar_t text[300], label[48];

    // Header: the monitor(s) that will receive the command, with the ID to use in "match".
    MonitorList monitors = {{0}, 0};
    EnumDisplayMonitors(NULL, NULL, ListMonitor, (LPARAM)&monitors);
    if (monitors.count) lstrcpynW(text, monitors.text, ARRAYSIZE(text));
    else if (cfg.match[0]) wsprintfW(text, T(L"Monitor \"%s\" non trovato", L"Display \"%s\" not found"), cfg.match);
    else lstrcpyW(text, T(L"Nessun monitor", L"No display"));
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    for (int i = 0; i < cfg.inputCount; i++) AppendMenuW(menu, MF_STRING, IDM_INPUT + i, cfg.inputs[i].name);
    if (!cfg.inputCount) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, T(L"Nessun ingresso configurato", L"No inputs configured"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    // Reminder of the shortcut, or why it doesn't work.
    if (hotkeyRegistered && cfg.hotkeyTarget) {
        HotkeyLabel(label);
        wsprintfW(text, T(L"Scorciatoia %s → %s", L"Shortcut %s → %s"), label, InputName(cfg.hotkeyTarget));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text);
    } else if (cfg.hotkeySpec[0]) {
        wsprintfW(text, T(L"Scorciatoia \"%s\" non valida o già in uso", L"Shortcut \"%s\" is invalid or already in use"),
                  cfg.hotkeySpec);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text);
    }
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, T(L"Impostazioni…", L"Settings…"));
    AppendMenuW(menu, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), IDM_AUTOSTART,
                T(L"Avvia con Windows", L"Start with Windows"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, T(L"Esci", L"Quit"));

    // The documented tray-menu dance: without SetForegroundWindow the menu doesn't close when the user
    // clicks elsewhere, and the WM_NULL afterwards lets the next click on the icon work right away.
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(mainWnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, mainWnd, NULL);
    PostMessageW(mainWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= IDM_INPUT && cmd < IDM_INPUT + (UINT)cfg.inputCount) DoSwitch(cfg.inputs[cmd - IDM_INPUT].code);
    else if (cmd == IDM_SETTINGS) ShowSettings();
    else if (cmd == IDM_AUTOSTART) SetAutostart(!IsAutostart());
    else if (cmd == IDM_EXIT) DestroyWindow(mainWnd);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:  // lp is the mouse message on the icon
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) ShowMenu();
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) {
            LoadConfig();  // pick up edits made to the file since the last load
            if (cfg.hotkeyTarget) DoSwitch(cfg.hotkeyTarget);
        }
        return 0;
    case WM_SETTINGCHANGE:  // light/dark switch: popup menus follow it after a refresh
        if (lp && !lstrcmpW((const wchar_t *)lp, L"ImmersiveColorSet")) InitDarkMode();
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == taskbarCreatedMsg) {  // Explorer restarted: tray icons must be added again
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmdLine, int show) {
    (void)prev;
    (void)cmdLine;
    (void)show;
    InitPaths();
    MigrateFromOldName();
    LoadConfig();

    // Command line: runs alongside the tray app, if one is running, and exits.
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) {
        int rc = 2;  // usage error
        if (argc == 2 && !lstrcmpiW(argv[1], L"--list")) {
            ShowDiagnostics();
            rc = 0;
        } else if (argc == 3 && !lstrcmpiW(argv[1], L"--input")) {
            DWORD code = wcstoul(argv[2], NULL, 0);
            rc = code && !SwitchInput(code) ? 0 : 1;
        }
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);

    // One tray app per user session: a second launch exits quietly.
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\Bivio");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    InitDarkMode();
    taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = {.lpfnWndProc = WndProc, .hInstance = instance, .lpszClassName = APP_NAME};
    RegisterClassW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    mainWnd = CreateWindowExW(0, APP_NAME, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    AllowDarkMode(mainWnd);
    AddTrayIcon();
    ApplyHotkey();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        HWND settings = SettingsWindow();
        if (settings && IsDialogMessageW(settings, &msg)) continue;  // Tab, Enter and Esc in the settings window
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(mutex);
    return 0;
}
