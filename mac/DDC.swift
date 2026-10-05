import Foundation
import IOKit

// DDC/CI on macOS (Apple Silicon): finding the external displays and sending them VCP commands.
//
// DDC/CI lets a computer send commands to a monitor over the I²C lines of the video cable. Each
// command sets a "VCP feature" (MCCS standard) to a 16-bit value; feature 0x60 is the input source.

/// An external display reachable over DDC/CI through its IOAVService.
struct ExternalDisplay {
    /// MCCS VCP feature "Input Source": its value selects the monitor input (15 = DisplayPort 1, …).
    static let inputSourceVCP: UInt8 = 0x60

    let name: String   // EDID product name, e.g. "MPG491CX OLED"
    let pnpID: String  // EDID manufacturer + product code, e.g. "MSI4FA8" (same ID Windows shows)
    let service: CFTypeRef  // IOAVService used for the I²C writes

    /// Whether the display is selected by the `match` setting: part of its ID or name, case-insensitive.
    /// An empty pattern matches every external display.
    func matches(_ pattern: String) -> Bool {
        pattern.isEmpty
            || pnpID.localizedCaseInsensitiveContains(pattern)
            || name.localizedCaseInsensitiveContains(pattern)
    }

    /// Sends a DDC/CI "Set VCP Feature" command. Returns false if every I²C write failed.
    ///
    /// Packet sent to I²C address 0x37 (the host address 0x51 goes first, as `dataAddress`):
    ///
    ///     0x84        length byte: 0x80 | 4 bytes of payload
    ///     0x03        opcode "Set VCP Feature"
    ///     code        VCP feature code
    ///     value >> 8  value, high byte
    ///     value & ff  value, low byte
    ///     checksum    XOR of the destination address (0x6E = 0x37 << 1), 0x51 and every byte above
    ///
    /// The command is sent twice with a short pause: some monitors drop the first packet after idling,
    /// and setting the same value twice is harmless.
    func setVCP(_ code: UInt8, _ value: UInt16) -> Bool {
        var packet: [UInt8] = [0x84, 0x03, code, UInt8(value >> 8), UInt8(value & 0xFF), 0]
        packet[5] = packet[0..<5].reduce(0x6E ^ 0x51, ^)
        var ok = false
        for _ in 0..<2 {
            usleep(10_000)  // short pause before each write (the same timing m1ddc uses)
            if IOAVServiceWriteI2C(service, 0x37, 0x51, &packet, UInt32(packet.count)) == kIOReturnSuccess {
                ok = true
            }
        }
        return ok
    }

    /// Every external display with an IOAVService, in IORegistry order.
    ///
    /// The registry has no direct link from a display's EDID to its IOAVService, so the walk relies on
    /// their order (as m1ddc does): each framebuffer (AppleCLCD2, or IOMobileFramebufferShim on older
    /// systems) carries the EDID attributes of its display and is followed, further down its subtree, by
    /// the DCPAVServiceProxy used to talk to that display. The built-in panel has a proxy too, marked
    /// "Embedded", and is skipped.
    static func all() -> [ExternalDisplay] {
        var iterator = io_iterator_t()
        let root = IORegistryGetRootEntry(kIOMainPortDefault)
        defer { IOObjectRelease(root) }
        guard IORegistryEntryCreateIterator(root, kIOServicePlane, IOOptionBits(kIORegistryIterateRecursively),
                                            &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }

        var displays: [ExternalDisplay] = []
        var name = "", pnpID = ""  // attributes of the last framebuffer seen
        while case let entry = IOIteratorNext(iterator), entry != 0 {
            defer { IOObjectRelease(entry) }
            switch IOObjectCopyClass(entry)?.takeRetainedValue() as String? {
            case "AppleCLCD2", "IOMobileFramebufferShim":
                let attributes = property(entry, "DisplayAttributes") as? [String: Any]
                let product = attributes?["ProductAttributes"] as? [String: Any]
                name = product?["ProductName"] as? String ?? ""
                // ManufacturerID is the 3-letter PnP code ("MSI"), ProductID the EDID product code (0x4FA8).
                pnpID = (product?["ManufacturerID"] as? String ?? "")
                    + String(format: "%04X", product?["ProductID"] as? Int ?? 0)
            case "DCPAVServiceProxy":
                guard property(entry, "Location") as? String == "External",
                      let service = IOAVServiceCreateWithService(kCFAllocatorDefault, entry)?.takeRetainedValue()
                else { continue }
                displays.append(ExternalDisplay(name: name, pnpID: pnpID, service: service))
            default:
                break
            }
        }
        return displays
    }

    private static func property(_ entry: io_registry_entry_t, _ key: String) -> Any? {
        IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }
}

enum DDCError: Error, CustomStringConvertible {
    case noDisplay(String)  // no external display matches the `match` setting
    case writeFailed        // displays found, but no I²C write succeeded

    var description: String {
        switch self {
        case .noDisplay(let match): L.noDisplay(match)
        case .writeFailed: L.writeFailed
        }
    }
}

/// Sends a VCP feature to every external display matching `match`.
/// Succeeds if at least one display accepted the command: DDC/CI has no reliable acknowledgment, so
/// "accepted" means the I²C write went through, not that the monitor acted on it.
func setVCP(_ code: UInt8, _ value: UInt16, match: String) throws {
    let targets = ExternalDisplay.all().filter { $0.matches(match) }
    if targets.isEmpty { throw DDCError.noDisplay(match) }
    // `map` before `contains` so every display gets the command, not just the first that accepts it.
    if !targets.map({ $0.setVCP(code, value) }).contains(true) { throw DDCError.writeFailed }
}
