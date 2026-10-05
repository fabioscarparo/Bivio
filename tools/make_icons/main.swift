// Dev tool: renders the macOS icon set (from the tile version) and the Windows .ico (from the bare icon).
// Build and run: make icons
import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 5,
      let macImage = NSImage(contentsOf: URL(fileURLWithPath: arguments[1])),
      let windowsImage = NSImage(contentsOf: URL(fileURLWithPath: arguments[3])) else {
    print("usage: make_icons <icon-macos.svg> <AppIcon.iconset dir> <icon.svg> <icon.ico>")
    exit(2)
}

/// `zoom` enlarges the drawing around its center, to fill more of the canvas.
func render(_ image: NSImage, _ size: Int, zoom: CGFloat = 1) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: size * 4, bitsPerPixel: 32)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    NSGraphicsContext.current?.imageInterpolation = .high
    let side = CGFloat(size) * zoom, origin = (CGFloat(size) - side) / 2
    image.draw(in: NSRect(x: origin, y: origin, width: side, height: side))  // vector: sharp at every size
    NSGraphicsContext.restoreGraphicsState()
    return rep
}

func png(_ rep: NSBitmapImageRep) -> Data { rep.representation(using: .png, properties: [:])! }

// macOS: the sizes iconutil expects
let iconset = URL(fileURLWithPath: arguments[2], isDirectory: true)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for points in [16, 32, 128, 256, 512] {
    try png(render(macImage, points)).write(to: iconset.appendingPathComponent("icon_\(points)x\(points).png"))
    try png(render(macImage, points * 2)).write(to: iconset.appendingPathComponent("icon_\(points)x\(points)@2x.png"))
}

// Windows: 32-bit DIB entries for the small sizes, PNG for 256 px
extension Data {
    mutating func append<T: FixedWidthInteger>(le value: T) { Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) } }
}

func dib(_ rep: NSBitmapImageRep) -> Data {
    let size = rep.pixelsWide
    var data = Data()
    data.append(le: UInt32(40)); data.append(le: Int32(size)); data.append(le: Int32(size * 2))
    data.append(le: UInt16(1)); data.append(le: UInt16(32))
    for _ in 0..<6 { data.append(le: UInt32(0)) }
    let pixels = rep.bitmapData!
    for y in (0..<size).reversed() {  // bottom-up rows, BGRA with straight (non-premultiplied) alpha
        for x in 0..<size {
            let p = pixels + (y * size + x) * 4
            let a = Int(p[3])
            let unpremultiply = { (c: UInt8) -> UInt8 in a == 0 ? 0 : UInt8(min(255, Int(c) * 255 / a)) }
            data.append(contentsOf: [unpremultiply(p[2]), unpremultiply(p[1]), unpremultiply(p[0]), p[3]])
        }
    }
    data.append(Data(count: ((size + 31) / 32) * 4 * size))  // AND mask, unused with 32-bit alpha
    return data
}

// The glyph spans 741 px of its 1024 canvas; on Windows it fills about 94% of the icon width.
let windowsZoom: CGFloat = 0.94 / (741.111 / 1024.0)
let icoSizes = [16, 20, 24, 32, 40, 48, 64, 256]
let images = icoSizes.map { size in
    let rep = render(windowsImage, size, zoom: windowsZoom)
    return size >= 256 ? png(rep) : dib(rep)
}
var ico = Data()
ico.append(le: UInt16(0)); ico.append(le: UInt16(1)); ico.append(le: UInt16(images.count))
var offset = 6 + 16 * images.count
for (size, image) in zip(icoSizes, images) {
    let dimension = UInt8(size >= 256 ? 0 : size)
    ico.append(contentsOf: [dimension, dimension, 0, 0])
    ico.append(le: UInt16(1)); ico.append(le: UInt16(32))
    ico.append(le: UInt32(image.count)); ico.append(le: UInt32(offset))
    offset += image.count
}
images.forEach { ico.append($0) }
try ico.write(to: URL(fileURLWithPath: arguments[4]))
