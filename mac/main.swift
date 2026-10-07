import AppKit
import Carbon.HIToolbox
import ServiceManagement
import SwiftUI

// Bivio for macOS: a menu bar app that switches the monitor's input over DDC/CI.
//
//   click on the icon        popover with the inputs (and the settings behind the gear)
//   right click / ⌃ click    quick menu: inputs, Settings…, Quit
//   global shortcut          switches to the input set as target (default ⌃⌥⌘M)
//   Bivio --list | --input … command line, see runCommandLine()
//
// LSUIElement in Info.plist keeps the app out of the Dock and the app switcher.

final class AppDelegate: NSObject, NSApplicationDelegate, NSPopoverDelegate {
    /// For the Carbon hotkey callback, a C function pointer that cannot capture `self`.
    private static var shared: AppDelegate?

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let popover = NSPopover()
    private let model = PopoverModel()
    private var outsideClickMonitor: Any?
    /// The app that was frontmost when the popover opened, to give the focus back to when it closes.
    private var previousApp: NSRunningApplication?
    private var config = Config.load()
    private var hotKeyRef: EventHotKeyRef?
    private var registeredHotkey: Hotkey?
    private var hotkeyError = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        AppDelegate.shared = self
        let image = NSImage(systemSymbolName: "display", accessibilityDescription: "Bivio")
        image?.isTemplate = true  // drawn in the menu bar's colour, light or dark
        statusItem.button?.image = image
        statusItem.button?.target = self
        statusItem.button?.action = #selector(statusItemClicked)
        statusItem.button?.sendAction(on: [.leftMouseUp, .rightMouseUp])

        popover.behavior = .transient  // closes when the user interacts outside it (within the app)
        popover.delegate = self

        installHotKeyHandler()
        applyHotkey()
    }

    // MARK: Status item

    @objc private func statusItemClicked() {
        let event = NSApp.currentEvent
        if event?.type == .rightMouseUp || event?.modifierFlags.contains(.control) == true {
            showQuickMenu()
        } else if popover.isShown {
            popover.performClose(nil)
        } else {
            showPopover(page: .inputs)
        }
    }

    /// Opens the popover on `page`, with fresh settings and the displays connected right now.
    private func showPopover(page: PopoverModel.Page) {
        if popover.isShown {  // e.g. Settings… chosen from the quick menu while the popover is open
            model.page = page
            return
        }
        guard let button = statusItem.button else { return }
        if popover.contentViewController == nil {  // created on first use: the idle app stays smaller
            popover.contentViewController = NSHostingController(rootView: PopoverView(model: model, actions: PopoverActions(
                switchInput: { [weak self] in self?.switchFromPopover($0) },
                apply: { [weak self] in self?.apply($0) },
                setLaunchAtLogin: { [weak self] in self?.setLaunchAtLogin($0) },
                openFile: { [weak self] in
                    self?.popover.performClose(nil)
                    self?.openConfigFile()
                },
                quit: { NSApp.terminate(nil) })))
        }
        config = Config.load()
        applyHotkey()
        model.load(config: config, monitors: ExternalDisplay.all().map { ($0.name, $0.pnpID) },
                   launchAtLogin: SMAppService.mainApp.status == .enabled, hotkeyUnavailable: hotkeyError)
        model.page = page
        let frontmost = NSWorkspace.shared.frontmostApplication
        previousApp = frontmost?.processIdentifier == ProcessInfo.processInfo.processIdentifier ? nil : frontmost
        NSApp.activate()  // a menu bar app must be active for the settings text fields to take keyboard focus
        popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
        // .transient only sees clicks inside this app: also close on clicks in other apps or the desktop.
        outsideClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown]) {
            [weak self] _ in self?.popover.performClose(nil)
        }
    }

    func popoverDidClose(_ notification: Notification) {
        if let outsideClickMonitor { NSEvent.removeMonitor(outsideClickMonitor) }
        outsideClickMonitor = nil
        model.page = .inputs
        // Showing or clicking the popover makes Bivio the active app: give the focus back to the app the user
        // was in. Not with NSApp.hide: from a hidden app, the next popover closes at the second click in it.
        // If the popover closed because the user clicked another app, that app is already active.
        if NSApp.isActive, let previousApp, !previousApp.isTerminated {
            previousApp.activate(from: .current, options: [])
        }
        previousApp = nil
    }

    /// Shows a native menu under the icon. Assigning `statusItem.menu` makes the next click open it,
    /// so it is set just for this click and removed again, leaving left clicks to the popover.
    private func showQuickMenu() {
        config = Config.load()
        let menu = NSMenu()
        for input in config.inputs {
            let item = NSMenuItem(title: input.name, action: #selector(selectInput(_:)), keyEquivalent: "")
            item.target = self
            item.tag = Int(input.code)
            menu.addItem(item)
        }
        if !config.inputs.isEmpty { menu.addItem(.separator()) }
        let settings = NSMenuItem(title: L.settings, action: #selector(openSettings), keyEquivalent: ",")
        settings.target = self
        menu.addItem(settings)
        menu.addItem(NSMenuItem(title: L.quitApp, action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        statusItem.menu = menu
        statusItem.button?.performClick(nil)  // returns once the menu is dismissed
        statusItem.menu = nil
    }

    @objc private func selectInput(_ sender: NSMenuItem) {
        if let error = switchInput(UInt16(sender.tag)) {
            NSSound.beep()
            NSLog("Bivio: \(error)")
        }
    }

    @objc private func openSettings() {
        showPopover(page: .settings)
    }

    // MARK: Actions from the popover

    /// Closes the popover once the command is sent (the screen is about to show another computer);
    /// keeps it open with the error otherwise.
    private func switchFromPopover(_ code: UInt16) {
        if let error = switchInput(code) {
            model.error = error
            NSSound.beep()
        } else {
            popover.performClose(nil)
        }
    }

    private func apply(_ newConfig: Config) {
        let languageChanged = newConfig.language != config.language
        newConfig.save()
        config = Config.load()  // also switches L.language
        applyHotkey()
        model.hotkeyUnavailable = hotkeyError
        // The views read the static L.language, which SwiftUI doesn't observe: setting an observed
        // property (even to the same value) makes them render again in the new language.
        if languageChanged { model.page = model.page }
    }

    private func setLaunchAtLogin(_ enabled: Bool) {
        guard enabled != (SMAppService.mainApp.status == .enabled) else { return }
        do {
            if enabled { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            model.error = L.loginError(error.localizedDescription)
            model.launchAtLogin = SMAppService.mainApp.status == .enabled  // put the toggle back
        }
    }

    /// Opens the INI file in the default text editor (`open -t`).
    private func openConfigFile() {
        _ = Config.load()  // make sure the file exists and is up to date
        let open = Process()
        open.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        open.arguments = ["-t", Config.file.path]
        try? open.run()
    }

    /// Returns an error message, or nil once the command was sent.
    private func switchInput(_ code: UInt16) -> String? {
        do {
            try setVCP(ExternalDisplay.inputSourceVCP, code, match: config.match)
            return nil
        } catch {
            return "\(error)"
        }
    }

    // MARK: Global hotkey
    // Carbon's RegisterEventHotKey still works on current macOS and, unlike an event tap, needs no
    // Accessibility permission. The handler runs on the main thread.

    private func installHotKeyHandler() {
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, _, _ in
            DispatchQueue.main.async { AppDelegate.shared?.hotkeyPressed() }
            return noErr
        }, 1, &spec, nil, nil)
    }

    /// Registers the shortcut from the settings, if it changed. A failed registration (invalid spec, or
    /// a combination another app already took) is retried on the next call and shown in the popover.
    private func applyHotkey() {
        guard config.hotkey != registeredHotkey || hotKeyRef == nil else { return }
        if let ref = hotKeyRef { UnregisterEventHotKey(ref) }
        hotKeyRef = nil
        registeredHotkey = config.hotkey
        hotkeyError = false
        guard let hotkey = config.hotkey else { return }
        let id = EventHotKeyID(signature: OSType(0x4249_564F), id: 1)  // 'BIVO'; one hotkey, so no id check
        hotkeyError = RegisterEventHotKey(hotkey.keyCode, hotkey.modifiers, id, GetApplicationEventTarget(), 0,
                                          &hotKeyRef) != noErr
    }

    private func hotkeyPressed() {
        config = Config.load()  // pick up edits made to the file since the last load
        if let target = config.hotkeyTarget, let error = switchInput(target) {
            NSSound.beep()
            NSLog("Bivio: \(error)")
        }
    }
}

// MARK: Command line
// `Bivio.app/Contents/MacOS/Bivio --list | --input <code> | --vcp <code> <value>`, for scripts and
// Shortcuts. Exit status: 0 done, 1 DDC error, 2 usage error.

func runCommandLine(_ args: [String]) -> Int32 {
    let config = Config.load()
    do {
        switch (args.first, args.count) {
        case ("--list", 1):
            for display in ExternalDisplay.all() {
                print("\(display.name) (\(display.pnpID))\(display.matches(config.match) ? "  ← match" : "")")
            }
            return 0
        case ("--input", 2):
            if let code = Config.parseNumber(args[1]) {
                try setVCP(ExternalDisplay.inputSourceVCP, code, match: config.match)
                return 0
            }
        case ("--vcp", 3):
            if let code = Config.parseNumber(args[1]), code <= 0xFF, let value = Config.parseNumber(args[2]) {
                try setVCP(UInt8(code), value, match: config.match)
                return 0
            }
        default:
            break
        }
    } catch {
        FileHandle.standardError.write("\(error)\n".data(using: .utf8)!)
        return 1
    }
    print(L.usage)
    return 2
}

// Entry point. Arguments starting with "--" run the command line and exit; anything else (macOS may
// pass its own arguments when launching an app) starts the menu bar app.
let arguments = Array(CommandLine.arguments.dropFirst())
if arguments.first?.hasPrefix("--") == true {
    exit(runCommandLine(arguments))
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)  // menu bar only, as LSUIElement
app.run()
