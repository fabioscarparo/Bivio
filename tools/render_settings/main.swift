// Dev tool: renders the macOS popover pages (light and dark) to PNG to check the layout.
// Build: make preview
import AppKit
import SwiftUI

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "build"
let actions = PopoverActions(switchInput: { _ in }, apply: { _ in }, setLaunchAtLogin: { _ in }, openFile: {}, quit: {})

for (appearanceName, appearance) in [("light", NSAppearance.Name.aqua), ("dark", .darkAqua)] {
    for (pageName, page) in [("inputs", PopoverModel.Page.inputs), ("settings", .settings)] {
        NSApp.appearance = NSAppearance(named: appearance)
        let model = PopoverModel()
        model.load(config: Config.defaults, monitors: [("MPG491CX OLED", "MSI4FA8")], launchAtLogin: true,
                   hotkeyUnavailable: false)
        model.page = page
        let controller = NSHostingController(rootView: PopoverView(model: model, actions: actions))
        let window = NSWindow(contentViewController: controller)
        window.styleMask = [.borderless]
        window.backgroundColor = .windowBackgroundColor
        window.setFrameOrigin(NSPoint(x: -20000, y: -20000))
        window.orderFront(nil)
        RunLoop.main.run(until: Date().addingTimeInterval(1))
        let view = window.contentView!
        let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds)!
        view.cacheDisplay(in: view.bounds, to: rep)
        try! rep.representation(using: .png, properties: [:])!
            .write(to: URL(fileURLWithPath: "\(out)/popover-\(pageName)-\(appearanceName).png"))
        window.close()
    }
}
