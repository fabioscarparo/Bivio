import Foundation

// Interface strings in Italian and English. The app is small enough that a table in code is simpler
// than .strings files, and it keeps the Mac and Windows apps (which uses the same approach) aligned.

enum Language {
    case it, en

    /// "it", "en" or "auto": the system language, falling back to English unless it is Italian.
    init(setting: String) {
        switch setting.lowercased() {
        case "it": self = .it
        case "en": self = .en
        default: self = (Locale.preferredLanguages.first ?? "en").hasPrefix("it") ? .it : .en
        }
    }
}

/// Localized strings. `language` is set by `Config.load()` from the `language` setting; views read it
/// when they render, so a language change shows up on the next render.
enum L {
    static var language = Language(setting: "auto")

    private static func t(_ it: String, _ en: String) -> String { language == .it ? it : en }

    // Errors and status
    static func noDisplay(_ match: String) -> String {
        match.isEmpty ? t("Nessun monitor esterno", "No external display")
                      : t("Monitor \"\(match)\" non trovato", "Display \"\(match)\" not found")
    }
    static var writeFailed: String { t("Il monitor non ha accettato il comando DDC", "The display did not accept the DDC command") }
    static var noInputs: String { t("Nessun ingresso configurato", "No inputs configured") }
    static func shortcut(_ keys: String, _ input: String) -> String { t("Scorciatoia \(keys) → \(input)", "Shortcut \(keys) → \(input)") }
    static func shortcutUnavailable(_ spec: String) -> String {
        t("Scorciatoia \"\(spec)\" non valida o già in uso", "Shortcut \"\(spec)\" is invalid or already in use")
    }
    static func loginError(_ error: String) -> String {
        t("Impossibile modificare l'avvio al login: \(error)", "Could not change launch at login: \(error)")
    }

    // Quick menu (right click on the menu bar icon)
    static var settings: String { t("Impostazioni…", "Settings…") }
    static var quitApp: String { t("Esci da Bivio", "Quit Bivio") }

    // Popover: settings page
    static var settingsShort: String { t("Impostazioni", "Settings") }
    static var back: String { t("Indietro", "Back") }
    static var sectionMonitor: String { t("Monitor", "Monitor") }
    static var monitorToControl: String { t("Monitor da comandare", "Monitor to control") }
    static var allMonitors: String { t("Tutti i monitor esterni", "All external monitors") }
    static func notConnected(_ id: String) -> String { t("\(id) (non collegato)", "\(id) (not connected)") }
    static var sectionInputs: String { t("Ingressi nel menu", "Menu inputs") }
    static var inputName: String { t("Nome", "Name") }
    static var inputNamePrompt: String { t("Nome nel menu", "Name in the menu") }
    static var inputCode: String { t("Ingresso", "Input") }
    static var inputsFooter: String {
        t("Per aggiungere un ingresso compila la riga vuota; per toglierlo cancellane il nome.",
          "To add an input, fill in the empty row; to remove one, clear its name.")
    }
    static func customCode(_ code: UInt16) -> String { t("Codice \(code)", "Code \(code)") }
    static var sectionShortcut: String { t("Scorciatoia da tastiera", "Keyboard shortcut") }
    static var shortcutKey: String { t("Tasto", "Key") }
    static var noKey: String { t("Nessuna", "None") }
    static var shortcutTarget: String { t("Passa a", "Switch to") }
    static var sectionGeneral: String { t("Generale", "General") }
    static var languageLabel: String { t("Lingua", "Language") }
    static var languageAuto: String { t("Automatica (sistema)", "Automatic (system)") }
    static var launchAtLogin: String { t("Avvia al login", "Launch at login") }
    static var openFile: String { t("Apri il file…", "Open file…") }

    // Command line
    static var usage: String {
        t("Uso: Bivio --list | --input <codice> | --vcp <codice> <valore>",
          "Usage: Bivio --list | --input <code> | --vcp <code> <value>")
    }
}
