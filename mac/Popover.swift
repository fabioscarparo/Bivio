import AppKit
import SwiftUI

// The menu bar popover, in SwiftUI. Two pages in the same popover: the inputs as tiles (click one to
// switch) and the settings (behind the gear button). Settings apply as soon as they change, there is no
// Save button. On macOS 26+ the popover and the standard controls pick up Liquid Glass by themselves.
//
// The views never touch the settings file or DDC directly: they edit a `PopoverModel` and call the
// `PopoverActions` closures, which the AppDelegate implements.

/// Input codes offered in the pickers (MCCS VCP 0x60). Any other code can still be set in the file.
let commonInputCodes: [(code: UInt16, label: String)] = [
    (15, "DisplayPort 1"), (16, "DisplayPort 2 / USB-C"), (17, "HDMI 1"), (18, "HDMI 2"),
    (27, "USB-C"), (3, "DVI"), (1, "VGA"),
]

func inputCodeLabel(_ code: UInt16) -> String {
    commonInputCodes.first { $0.code == code }?.label ?? L.customCode(code)
}

/// Keys offered for the shortcut: the same set `Hotkey.keyCodes` can register.
let hotkeyKeys: [String] = (65...90).map { String(UnicodeScalar($0)!) } + (0...9).map(String.init)
    + (1...12).map { "F\($0)" }

/// What the popover shows and edits. Filled from the settings file each time the popover opens.
@Observable
final class PopoverModel {
    /// An editable input row. `id` keeps SwiftUI's identity stable while the name is being typed.
    struct Row: Identifiable {
        let id = UUID()
        var name: String
        var code: UInt16
    }

    enum Page { case inputs, settings }

    static let maxRows = 8

    var page = Page.inputs
    var language = "auto"
    var match = ""
    var rows: [Row] = []
    // The shortcut, split into what the controls edit; `makeConfig()` joins it back into a spec.
    var ctrl = false
    var alt = false
    var shift = false
    var cmd = false
    var key = ""  // empty = shortcut disabled
    var target: UInt16 = 15
    var launchAtLogin = false
    var monitors: [(name: String, id: String)] = []  // connected external displays
    var hotkeyUnavailable = false  // the shortcut could not be registered (invalid, or taken by another app)
    var error: String?             // last switch error, shown under the tiles
    var hoveredTile: UUID?  // kept here: @State needs the SwiftUI macro plugin, which ships only with Xcode

    func load(config: Config, monitors: [(name: String, id: String)], launchAtLogin: Bool, hotkeyUnavailable: Bool) {
        language = ["auto", "it", "en"].contains(config.languageSetting.lowercased()) ? config.languageSetting.lowercased() : "auto"
        match = config.match
        rows = config.inputs.map { Row(name: $0.name, code: $0.code) }
        target = config.hotkeyTarget ?? config.inputs.first?.code ?? 15
        self.launchAtLogin = launchAtLogin
        self.monitors = monitors
        self.hotkeyUnavailable = hotkeyUnavailable
        error = nil
        (ctrl, alt, shift, cmd, key) = (false, false, false, false, "")
        for part in config.hotkeySpec.lowercased().split(separator: "+").map({ $0.trimmingCharacters(in: .whitespaces) }) {
            switch part {
            case "ctrl", "control": ctrl = true
            case "alt", "option", "opt": alt = true
            case "shift": shift = true
            case "cmd", "command", "win": cmd = true
            default: key = hotkeyKeys.contains(part.uppercased()) ? part.uppercased() : ""
            }
        }
        keepOneEmptyRow()
    }

    /// Keeps an empty row at the end to add an input with (up to `maxRows`): typing in it adds a new one.
    func keepOneEmptyRow() {
        if rows.count < Self.maxRows, rows.last.map({ !$0.name.isEmpty }) ?? true {
            rows.append(Row(name: "", code: 15))
        }
    }

    /// Rows that are actual inputs. A row whose name is cleared stays on screen until the popover
    /// reopens, but is no longer saved.
    var namedRows: [Row] { rows.filter { !$0.name.trimmingCharacters(in: .whitespaces).isEmpty } }

    /// "Name · ID" of the displays the `match` setting selects, for the header.
    var monitorSubtitle: String {
        let connected = monitors.filter { match.isEmpty || $0.id.localizedCaseInsensitiveContains(match)
            || $0.name.localizedCaseInsensitiveContains(match) }
        return connected.isEmpty ? L.noDisplay(match) : connected.map { "\($0.name) · \($0.id)" }.joined(separator: ", ")
    }

    var hotkey: Hotkey? { Hotkey(parsing: makeConfig().hotkeySpec) }

    /// The settings as the popover currently shows them.
    func makeConfig() -> Config {
        var config = Config()
        config.languageSetting = language
        config.match = match
        config.inputs = namedRows.map { InputSource(name: $0.name.trimmingCharacters(in: .whitespaces), code: $0.code) }
        let modifiers = [(ctrl, "ctrl"), (alt, "alt"), (shift, "shift"), (cmd, "cmd")].filter(\.0).map(\.1)
        config.hotkeySpec = key.isEmpty || modifiers.isEmpty ? "" : (modifiers + [key.lowercased()]).joined(separator: "+")
        config.targetSpec = String(target)
        return config
    }
}

/// What the popover asks the app to do.
struct PopoverActions {
    let switchInput: (UInt16) -> Void
    let apply: (Config) -> Void           // save and apply new settings
    let setLaunchAtLogin: (Bool) -> Void
    let openFile: () -> Void              // open the INI file in the text editor
    let quit: () -> Void
}

struct PopoverView: View {
    @Bindable var model: PopoverModel
    let actions: PopoverActions

    var body: some View {
        Group {
            switch model.page {
            case .inputs: InputsPage(model: model, actions: actions)
            case .settings: SettingsPage(model: model, actions: actions)
            }
        }
        .frame(width: 400)
        // Settings apply as they change. Config is Equatable, so unrelated updates (hover, page) don't save.
        .onChange(of: model.makeConfig()) { actions.apply(model.makeConfig()) }
        .onChange(of: model.launchAtLogin) { actions.setLaunchAtLogin(model.launchAtLogin) }
    }
}

// MARK: Inputs page

private struct InputsPage: View {
    let model: PopoverModel
    let actions: PopoverActions

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .center) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Bivio").font(.headline)
                    Text(model.monitorSubtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer()
                Button { model.page = .settings } label: {
                    Image(systemName: "gearshape").font(.body).frame(width: 22, height: 22)
                }
                .buttonStyle(.borderless)
                .help(L.settingsShort)
                .accessibilityLabel(L.settingsShort)
            }

            if model.namedRows.isEmpty {
                Text(L.noInputs).foregroundStyle(.secondary).frame(maxWidth: .infinity).padding(.vertical, 20)
            } else {
                LazyVGrid(columns: [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)], spacing: 8) {
                    ForEach(model.namedRows) { row in
                        InputTile(name: row.name, detail: inputCodeLabel(row.code),
                                  isShortcutTarget: model.hotkey != nil && row.code == model.target,
                                  hover: model.hoveredTile == row.id,
                                  onHover: { model.hoveredTile = $0 ? row.id : nil }) {
                            actions.switchInput(row.code)
                        }
                    }
                }
            }

            if let error = model.error {
                Label(error, systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.red)
            }

            // Reminder of the shortcut, or why it doesn't work.
            if let hotkey = model.hotkey, !model.hotkeyUnavailable {
                Label(L.shortcut(hotkey.label, model.namedRows.first { $0.code == model.target }?.name
                        ?? inputCodeLabel(model.target)), systemImage: "keyboard")
                    .font(.caption).foregroundStyle(.secondary)
            } else if model.hotkeyUnavailable {
                Label(L.shortcutUnavailable(model.makeConfig().hotkeySpec), systemImage: "keyboard")
                    .font(.caption).foregroundStyle(.orange)
            }
        }
        .padding(16)
    }
}

/// A Control Center–style tile: rounded fill that brightens on hover. A keyboard badge marks the
/// input the shortcut switches to.
private struct InputTile: View {
    let name: String
    let detail: String
    let isShortcutTarget: Bool
    let hover: Bool
    let onHover: (Bool) -> Void
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Image(systemName: "display").font(.title3)
                    Spacer()
                    if isShortcutTarget { Image(systemName: "keyboard").font(.caption).foregroundStyle(.secondary) }
                }
                Text(name).font(.body.weight(.medium)).lineLimit(1)
                Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(12)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(hover ? .quaternary : .quinary))
            .contentShape(RoundedRectangle(cornerRadius: 12, style: .continuous))  // whole tile is clickable
        }
        .buttonStyle(.plain)
        .onHover(perform: onHover)
        .accessibilityLabel(name)
    }
}

// MARK: Settings page

/// A grouped form in the style of System Settings, with a back button and, at the bottom, the INI file
/// and Quit. Fixed height: the form scrolls if the inputs don't fit.
private struct SettingsPage: View {
    @Bindable var model: PopoverModel
    let actions: PopoverActions

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                Button { model.page = .inputs } label: {
                    Image(systemName: "chevron.left").font(.body.weight(.semibold)).frame(width: 22, height: 22)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(L.back)
                Text(L.settingsShort).font(.headline)
                Spacer()
            }
            .padding(.horizontal, 16)
            .padding(.top, 14)

            Form {
                Section(L.sectionMonitor) {
                    Picker(L.monitorToControl, selection: $model.match) {
                        Text(L.allMonitors).tag("")
                        ForEach(model.monitors, id: \.id) { Text("\($0.name) (\($0.id))").tag($0.id) }
                        // Keep a configured monitor that is not connected right now selectable.
                        if !model.match.isEmpty, !model.monitors.contains(where: { $0.id == model.match }) {
                            Text(L.notConnected(model.match)).tag(model.match)
                        }
                    }
                    .labelsHidden()
                }

                Section {
                    ForEach($model.rows) { $row in
                        HStack(spacing: 8) {
                            TextField(L.inputName, text: $row.name, prompt: Text(L.inputNamePrompt))
                                .labelsHidden()
                                .textFieldStyle(.roundedBorder)
                                .onChange(of: row.name) { model.keepOneEmptyRow() }
                            Picker(L.inputCode, selection: $row.code) {
                                ForEach(commonInputCodes, id: \.code) { Text("\($0.label) (\($0.code))").tag($0.code) }
                                if !commonInputCodes.contains(where: { $0.code == row.code }) {
                                    Text(L.customCode(row.code)).tag(row.code)  // a code set in the file
                                }
                            }
                            .labelsHidden()
                            .frame(width: 205, alignment: .trailing)
                        }
                    }
                } header: {
                    Text(L.sectionInputs)
                } footer: {
                    Text(L.inputsFooter).foregroundStyle(.secondary).font(.caption)
                }

                Section(L.sectionShortcut) {
                    HStack(spacing: 6) {
                        ModifierToggle(symbol: "⌃", name: "Control", isOn: $model.ctrl)
                        ModifierToggle(symbol: "⌥", name: "Option", isOn: $model.alt)
                        ModifierToggle(symbol: "⇧", name: "Shift", isOn: $model.shift)
                        ModifierToggle(symbol: "⌘", name: "Command", isOn: $model.cmd)
                        Spacer()
                        Picker(L.shortcutKey, selection: $model.key) {
                            Text(L.noKey).tag("")
                            ForEach(hotkeyKeys, id: \.self) { Text($0).tag($0) }
                        }
                        .labelsHidden()
                        .frame(width: 90)
                    }
                    Picker(L.shortcutTarget, selection: $model.target) {
                        ForEach(model.namedRows) { Text($0.name).tag($0.code) }
                        if !model.namedRows.contains(where: { $0.code == model.target }) {
                            Text(L.customCode(model.target)).tag(model.target)
                        }
                    }
                    .disabled(model.key.isEmpty)
                }

                Section(L.sectionGeneral) {
                    Picker(L.languageLabel, selection: $model.language) {
                        Text(L.languageAuto).tag("auto")
                        Text("Italiano").tag("it")
                        Text("English").tag("en")
                    }
                    Toggle(L.launchAtLogin, isOn: $model.launchAtLogin)
                }
            }
            .formStyle(.grouped)

            HStack {
                Button(L.openFile, action: actions.openFile).buttonStyle(.link)
                Spacer()
                Button(L.quitApp, action: actions.quit)
            }
            .font(.callout)
            .padding(.horizontal, 20)
            .padding(.bottom, 14)
        }
        .frame(height: 640)
    }
}

/// A modifier key of the shortcut: accent-filled when it is part of it.
private struct ModifierToggle: View {
    let symbol: String
    let name: String
    @Binding var isOn: Bool

    var body: some View {
        Button { isOn.toggle() } label: { Text(symbol).frame(width: 16) }
            .modifier(GlassButton(prominent: isOn))
            .help(name)
            .accessibilityLabel(name)
            .accessibilityAddTraits(isOn ? .isSelected : [])
    }
}

/// Liquid Glass button styles on macOS 26+, the standard bordered styles before.
private struct GlassButton: ViewModifier {
    let prominent: Bool

    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            if prominent { content.buttonStyle(.glassProminent) } else { content.buttonStyle(.glass) }
        } else {
            if prominent { content.buttonStyle(.borderedProminent) } else { content.buttonStyle(.bordered) }
        }
    }
}
