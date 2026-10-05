// Declarations shared by the two source files of the Windows app:
//
//   bivio.c     tray icon and menu, global hotkey, settings file, DDC/CI, entry point
//   settings.c  the settings window (and the dark-mode helpers both files use)
//
// Plain C and Win32 only, so the executable needs no runtime and stays around 100 KB.
#pragma once

#ifndef UNICODE  // wide-character (UTF-16) Win32 APIs throughout
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00  // Windows 10 and later
#include <windows.h>

#define APP_NAME L"Bivio"  // also the window class, the tray tooltip and the Run registry value
#define MAX_INPUTS 16

/// A menu entry: the name the user chose and the VCP 0x60 value that selects that input.
typedef struct {
    wchar_t name[64];
    DWORD code;
} InputSource;

/// The settings as written in %APPDATA%\Bivio\Bivio.ini (same format as the Mac app), plus the
/// values derived from them. Text values are kept as written, so a value the app cannot parse is
/// preserved when the file is rewritten.
typedef struct {
    wchar_t language[16];    // auto | it | en
    wchar_t match[64];       // monitor ID or part of its name; empty = every monitor
    InputSource inputs[MAX_INPUTS];
    int inputCount;
    wchar_t hotkeySpec[64];  // e.g. "ctrl+alt+win+m"; empty = no shortcut
    wchar_t targetSpec[16];  // input code selected by the shortcut, as text

    // Derived by LoadConfig()
    UINT hotkeyMods, hotkeyVk;  // for RegisterHotKey
    BOOL hotkeyValid;
    DWORD hotkeyTarget;
} Config;

extern Config cfg;                 // the current settings, loaded by LoadConfig()
extern BOOL italian;               // interface language, from the `language` setting
extern wchar_t iniPath[MAX_PATH];  // full path of Bivio.ini

/// Picks the Italian or the English string according to the current language.
#define T(it, en) (italian ? (it) : (en))

// bivio.c
wchar_t *Trim(wchar_t *s);
void LoadConfig(void);
void WriteConfig(void);
void ApplyHotkey(void);
BOOL HotkeyRegistered(void);
BOOL IsAutostart(void);
void SetAutostart(BOOL enable);
int ListMonitors(wchar_t ids[][16], wchar_t names[][128], int max);

// settings.c
void InitDarkMode(void);
void AllowDarkMode(HWND wnd);
void ShowSettings(void);
HWND SettingsWindow(void);  // the settings window, or NULL when it is closed
