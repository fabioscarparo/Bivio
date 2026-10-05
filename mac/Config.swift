import Carbon.HIToolbox
import Foundation

// Settings: an INI file in ~/Library/Application Support/Bivio, in the same format as the Windows app.
//
// The file documents itself: `render()` writes every setting with comments explaining it, in the app's
// language. Users can edit it by hand; the popover edits the same values. Unknown keys are ignored.
//
//     [general]  language = auto | it | en
//     [monitor]  match    = monitor ID or part of its name (empty: every external monitor)
//     [inputs]   <name shown in the menu> = <input code>, one line per input
//     [hotkey]   keys     = e.g. ctrl+alt+cmd+m (empty: no shortcut)
//                target   = input code selected by the shortcut

/// A menu entry: the name the user chose and the VCP 0x60 value that selects that input.
struct InputSource: Equatable {
    let name: String
    let code: UInt16
}

/// A global shortcut, parsed into what Carbon's RegisterEventHotKey expects.
struct Hotkey: Equatable {
    let keyCode: UInt32    // virtual key code (physical key position)
    let modifiers: UInt32  // Carbon modifier mask (cmdKey, optionKey, …)
    let label: String      // for display, e.g. "⌃⌥⌘M"
}

/// The settings as written in the file. Values are kept as text (`hotkeySpec`, `targetSpec`) so a
/// value the app cannot parse is preserved, not lost, when the file is rewritten.
struct Config: Equatable {
    var languageSetting = "auto"
    var match = ""
    var inputs: [InputSource] = []
    var hotkeySpec = ""
    var targetSpec = ""

    var language: Language { Language(setting: languageSetting) }
    var hotkey: Hotkey? { Hotkey(parsing: hotkeySpec) }
    var hotkeyTarget: UInt16? { Config.parseNumber(targetSpec) }

    /// First-run settings, written for the MSI MPG 491CQPX the app was built for (see README).
    static let defaults = Config(
        match: "MSI4FA8",
        inputs: [InputSource(name: "PC (DisplayPort)", code: 15), InputSource(name: "Mac (USB-C)", code: 16),
                 InputSource(name: "HDMI 1", code: 17), InputSource(name: "HDMI 2", code: 18)],
        hotkeySpec: "ctrl+alt+cmd+m",
        targetSpec: "15")

    static let directory = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/Bivio", isDirectory: true)
    static let file = directory.appendingPathComponent("Bivio.ini")
    static let backup = directory.appendingPathComponent("Bivio.ini.bak")

    // MARK: Reading and writing

    /// Reads the settings and switches the UI to their language. Creates the file with the defaults on
    /// first run (or when it is empty).
    ///
    /// A file whose comments differ from the current ones (written by an older version, or documented
    /// in another language) is rewritten with the current documentation, keeping its values; the
    /// previous file is saved as Bivio.ini.bak. Comments added by the user are therefore not kept.
    static func load() -> Config {
        let fm = FileManager.default
        migrateFromOldName()
        // Latin-1 decodes any byte sequence, so a file saved in a legacy encoding is still readable.
        let text = fm.fileExists(atPath: file.path)
            ? (try? String(contentsOf: file, encoding: .utf8)) ?? (try? String(contentsOf: file, encoding: .isoLatin1))
            : nil
        guard let text, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            defaults.save()
            L.language = defaults.language
            return defaults
        }
        let config = parse(text)
        L.language = config.language
        if comments(text) != comments(config.render()) {
            try? fm.removeItem(at: backup)
            try? fm.copyItem(at: file, to: backup)
            config.save()
        }
        return config
    }

    /// The app used to be called MSwitchIO: its settings move to the new folder the first time Bivio runs.
    private static func migrateFromOldName() {
        let fm = FileManager.default
        let oldDirectory = directory.deletingLastPathComponent().appendingPathComponent("MSwitchIO", isDirectory: true)
        let oldFile = oldDirectory.appendingPathComponent("MSwitchIO.ini")
        guard !fm.fileExists(atPath: file.path), fm.fileExists(atPath: oldFile.path) else { return }
        try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
        try? fm.moveItem(at: oldFile, to: file)
        try? fm.moveItem(at: oldDirectory.appendingPathComponent("MSwitchIO.ini.bak"), to: backup)
        if (try? fm.contentsOfDirectory(atPath: oldDirectory.path))?.isEmpty == true {
            try? fm.removeItem(at: oldDirectory)
        }
    }

    /// Writes the settings (atomically, so a crash never leaves a half-written file). Errors are
    /// ignored: the settings stay in memory and the next save tries again.
    func save() {
        try? FileManager.default.createDirectory(at: Config.directory, withIntermediateDirectories: true)
        try? render().write(to: Config.file, atomically: true, encoding: .utf8)
    }

    /// Parses the INI text. Lenient on purpose: blank lines, comments (";" or "#"), unknown sections and
    /// keys are skipped, and an input whose code is not a number is dropped.
    static func parse(_ text: String) -> Config {
        var config = Config()
        var section = ""
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix(";") || line.hasPrefix("#") { continue }
            if line.hasPrefix("["), line.hasSuffix("]") {
                section = line.dropFirst().dropLast().lowercased()
                continue
            }
            guard let eq = line.firstIndex(of: "=") else { continue }  // the first "=" splits key and value
            let key = line[..<eq].trimmingCharacters(in: .whitespaces)
            let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            switch (section, key.lowercased()) {
            case ("general", "language"): config.languageSetting = value
            case ("monitor", "match"): config.match = value
            case ("inputs", _): if let code = parseNumber(value) { config.inputs.append(InputSource(name: key, code: code)) }
            case ("hotkey", "keys"): config.hotkeySpec = value
            case ("hotkey", "target"): config.targetSpec = value
            default: break
            }
        }
        return config
    }

    /// A number in decimal or 0x-prefixed hex, within the 16 bits of a VCP value.
    static func parseNumber(_ s: String) -> UInt16? {
        s.lowercased().hasPrefix("0x") ? UInt16(s.dropFirst(2), radix: 16) : UInt16(s)
    }

    /// An input name made safe for an `[inputs]` line: in "name=code" the name cannot contain "=" (the
    /// first one ends the key), and a line starting with ";", "#" or "[" would read as a comment or a
    /// section, so such an input would vanish the next time the file is read.
    static func iniSafeName(_ name: String) -> String {
        var safe = name.replacingOccurrences(of: "=", with: "-").trimmingCharacters(in: .whitespaces)
        while let first = safe.first, ";#[".contains(first) { safe.removeFirst() }
        return safe.trimmingCharacters(in: .whitespaces)
    }

    // MARK: The documented file

    /// First line of the file.
    var title: String { language == .it ? "; Bivio - Impostazioni" : "; Bivio - Settings" }

    /// The documentation of a settings file: its comment lines. Comparing them tells whether the file
    /// was written by this version of the app, in this language.
    private static func comments(_ text: String) -> [Substring] {
        text.split(whereSeparator: \.isNewline).filter { $0.hasPrefix(";") }
    }

    /// The whole file: every value with comments explaining it, in the settings' language.
    func render() -> String {
        let inputLines = inputs.compactMap { input -> String? in
            let name = Config.iniSafeName(input.name)
            return name.isEmpty ? nil : "\(name)=\(input.code)"
        }.joined(separator: "\n")
        switch language {
        case .it:
            return """
                \(title)
                ; =============================================================================
                ;
                ; Bivio cambia l'ingresso video del monitor inviandogli un comando DDC/CI:
                ; lo stesso effetto di scegliere l'ingresso dal menu del monitor.
                ;
                ; COME MODIFICARE QUESTO FILE
                ;   - Le righe che iniziano con ";" sono commenti e vengono ignorate.
                ;   - Modifica solo il testo dopo il segno "=" (e i nomi in [inputs]).
                ;   - Salva il file: le modifiche valgono dalla prossima apertura del menu
                ;     di Bivio, senza riavviare l'app.
                ;   - Per tornare alle impostazioni iniziali cancella questo file:
                ;     verrà ricreato al prossimo avvio.
                ;
                ; Posizione del file: ~/Library/Application Support/Bivio/Bivio.ini


                [general]

                ; Lingua dell'app e di questo file.
                ;   auto = lingua del sistema     it = italiano     en = English
                language=\(languageSetting)


                [monitor]

                ; Il monitor da comandare. Puoi indicare:
                ;   - il suo ID (consigliato)                  es.  match=MSI4FA8
                ;   - una parte del nome                       es.  match=MPG491CX
                ;   - niente, per comandare tutti i monitor esterni:  match=
                ; Nome e ID del monitor collegato compaiono in cima al menu di Bivio.
                match=\(match)


                [inputs]

                ; Le voci del menu, una per riga, nella forma:
                ;   Nome da mostrare=codice dell'ingresso
                ; Il nome è libero; il codice dice al monitor quale ingresso mostrare.
                ; Puoi rinominare, riordinare, aggiungere o cancellare righe.
                ;
                ; Codici più comuni (standard DDC/CI):
                ;   15 = DisplayPort 1              17 = HDMI 1
                ;   16 = DisplayPort 2 / USB-C      18 = HDMI 2
                ; MSI MPG 491CQPX: DisplayPort 15, USB-C 16, HDMI 1 17, HDMI 2 18.
                \(inputLines)


                [hotkey]

                ; Scorciatoia da tastiera che cambia ingresso da qualsiasi app.
                ; Uno o più modificatori più un tasto, separati da "+":
                ;   modificatori:  ctrl   alt (= ⌥ Option)   shift   cmd (= ⌘ Command)
                ;   tasto:         una lettera A-Z, una cifra 0-9, oppure F1 ... F12
                ; Esempi:  ctrl+alt+cmd+m     shift+cmd+f12
                ; Lascia vuoto (keys=) per disattivare la scorciatoia.
                keys=\(hotkeySpec)

                ; Codice dell'ingresso scelto dalla scorciatoia: uno dei codici in [inputs].
                ; Di solito è l'ingresso dell'altro computer.
                target=\(targetSpec)

                """
        case .en:
            return """
                \(title)
                ; =============================================================================
                ;
                ; Bivio switches the monitor's video input by sending it a DDC/CI command:
                ; the same as picking the input from the monitor's own menu.
                ;
                ; HOW TO EDIT THIS FILE
                ;   - Lines starting with ";" are comments and are ignored.
                ;   - Only change the text after the "=" sign (and the names in [inputs]).
                ;   - Save the file: changes apply the next time you open the Bivio
                ;     menu, no need to restart the app.
                ;   - To go back to the default settings, delete this file:
                ;     it is recreated on the next launch.
                ;
                ; File location: ~/Library/Application Support/Bivio/Bivio.ini


                [general]

                ; Language of the app and of this file.
                ;   auto = system language     it = italiano     en = English
                language=\(languageSetting)


                [monitor]

                ; Which monitor to control. You can write:
                ;   - its ID (recommended)                     e.g.  match=MSI4FA8
                ;   - part of its name                         e.g.  match=MPG491CX
                ;   - nothing, to control every external monitor:  match=
                ; The name and ID of the connected monitor are shown at the top of the menu.
                match=\(match)


                [inputs]

                ; The menu entries, one per line, written as:
                ;   Name to show=input code
                ; The name is up to you; the code tells the monitor which input to show.
                ; You can rename, reorder, add or delete lines.
                ;
                ; Most common codes (DDC/CI standard):
                ;   15 = DisplayPort 1              17 = HDMI 1
                ;   16 = DisplayPort 2 / USB-C      18 = HDMI 2
                ; MSI MPG 491CQPX: DisplayPort 15, USB-C 16, HDMI 1 17, HDMI 2 18.
                \(inputLines)


                [hotkey]

                ; Keyboard shortcut that switches the input from any app.
                ; One or more modifiers plus one key, separated by "+":
                ;   modifiers:  ctrl   alt (= ⌥ Option)   shift   cmd (= ⌘ Command)
                ;   key:        a letter A-Z, a digit 0-9, or F1 ... F12
                ; Examples:  ctrl+alt+cmd+m     shift+cmd+f12
                ; Leave empty (keys=) to disable the shortcut.
                keys=\(hotkeySpec)

                ; Code of the input selected by the shortcut: one of the codes in [inputs].
                ; Usually the other computer's input.
                target=\(targetSpec)

                """
        }
    }
}

extension Hotkey {
    /// Parses a spec such as "ctrl+alt+cmd+m". "win" is accepted as a synonym of "cmd", so the same file
    /// works on both systems. Needs at least one modifier and exactly one key; nil if it is invalid.
    init?(parsing spec: String) {
        var modifiers: UInt32 = 0
        var key: (code: Int, label: String)?
        for part in spec.lowercased().split(separator: "+").map({ $0.trimmingCharacters(in: .whitespaces) }) {
            switch part {
            case "ctrl", "control": modifiers |= UInt32(controlKey)
            case "alt", "option", "opt": modifiers |= UInt32(optionKey)
            case "shift": modifiers |= UInt32(shiftKey)
            case "cmd", "command", "win": modifiers |= UInt32(cmdKey)
            default:
                guard key == nil, let code = Hotkey.keyCodes[part.uppercased()] else { return nil }
                key = (code, part.uppercased())
            }
        }
        guard let key, modifiers != 0 else { return nil }
        let symbols: [(UInt32, String)] = [(UInt32(controlKey), "⌃"), (UInt32(optionKey), "⌥"),
                                           (UInt32(shiftKey), "⇧"), (UInt32(cmdKey), "⌘")]
        let label = symbols.filter { modifiers & $0.0 != 0 }.map(\.1).joined() + key.label
        self.init(keyCode: UInt32(key.code), modifiers: modifiers, label: label)
    }

    /// Virtual key codes by key name. They are physical positions on an ANSI (US) keyboard: on other
    /// layouts a letter key may print a different character, but the shortcut follows the key's position.
    static let keyCodes: [String: Int] = [
        "A": kVK_ANSI_A, "B": kVK_ANSI_B, "C": kVK_ANSI_C, "D": kVK_ANSI_D, "E": kVK_ANSI_E,
        "F": kVK_ANSI_F, "G": kVK_ANSI_G, "H": kVK_ANSI_H, "I": kVK_ANSI_I, "J": kVK_ANSI_J,
        "K": kVK_ANSI_K, "L": kVK_ANSI_L, "M": kVK_ANSI_M, "N": kVK_ANSI_N, "O": kVK_ANSI_O,
        "P": kVK_ANSI_P, "Q": kVK_ANSI_Q, "R": kVK_ANSI_R, "S": kVK_ANSI_S, "T": kVK_ANSI_T,
        "U": kVK_ANSI_U, "V": kVK_ANSI_V, "W": kVK_ANSI_W, "X": kVK_ANSI_X, "Y": kVK_ANSI_Y,
        "Z": kVK_ANSI_Z,
        "0": kVK_ANSI_0, "1": kVK_ANSI_1, "2": kVK_ANSI_2, "3": kVK_ANSI_3, "4": kVK_ANSI_4,
        "5": kVK_ANSI_5, "6": kVK_ANSI_6, "7": kVK_ANSI_7, "8": kVK_ANSI_8, "9": kVK_ANSI_9,
        "F1": kVK_F1, "F2": kVK_F2, "F3": kVK_F3, "F4": kVK_F4, "F5": kVK_F5, "F6": kVK_F6,
        "F7": kVK_F7, "F8": kVK_F8, "F9": kVK_F9, "F10": kVK_F10, "F11": kVK_F11, "F12": kVK_F12,
    ]
}
