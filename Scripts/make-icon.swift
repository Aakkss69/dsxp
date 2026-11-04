// Draws DSXP's app icon (an original controller glyph, no third-party artwork)
// into an .appiconset folder.  Usage: swift Scripts/make-icon.swift <appiconset dir>
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let outDir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1]
                 : "App/Assets.xcassets/AppIcon.appiconset", isDirectory: true)
try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: a)
}

func render(_ px: Int) -> CGImage {
    let space = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0,
                        space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.scaleBy(x: CGFloat(px) / 1024, y: CGFloat(px) / 1024)
    ctx.setShouldAntialias(true)

    // background tile on Apple's 1024 grid (824 pt body, 100 pt margin)
    let tile = CGPath(roundedRect: CGRect(x: 100, y: 100, width: 824, height: 824),
                      cornerWidth: 186, cornerHeight: 186, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: rgb(0x000000, 0.35))
    ctx.addPath(tile)
    ctx.setFillColor(rgb(0x0B1633))
    ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addPath(tile)
    ctx.clip()
    let grad = CGGradient(colorsSpace: space, colors: [rgb(0x2F6BFF), rgb(0x0B1633)] as CFArray,
                          locations: [0, 1])!
    ctx.drawLinearGradient(grad, start: CGPoint(x: 512, y: 924), end: CGPoint(x: 512, y: 100), options: [])
    ctx.restoreGState()

    // controller silhouette: a wide body plus two grips
    let pad = CGMutablePath()
    pad.addRoundedRect(in: CGRect(x: 228, y: 400, width: 568, height: 230), cornerWidth: 112, cornerHeight: 112)
    pad.addEllipse(in: CGRect(x: 214, y: 262, width: 200, height: 300))
    pad.addEllipse(in: CGRect(x: 610, y: 262, width: 200, height: 300))
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -8), blur: 18, color: rgb(0x000000, 0.4))
    ctx.addPath(pad)
    ctx.setFillColor(rgb(0xF4F7FF))
    ctx.fillPath()
    ctx.restoreGState()

    let ink = rgb(0x14213F)
    ctx.setFillColor(ink)
    // d-pad
    ctx.fill(CGRect(x: 314, y: 466, width: 30, height: 104))
    ctx.fill(CGRect(x: 277, y: 503, width: 104, height: 30))
    // face buttons
    for (dx, dy) in [(0, 42), (42, 0), (0, -42), (-42, 0)] as [(CGFloat, CGFloat)] {
        ctx.fillEllipse(in: CGRect(x: 694 + dx - 18, y: 518 + dy - 18, width: 36, height: 36))
    }
    // sticks
    for cx in [430.0, 594.0] as [CGFloat] {
        ctx.setFillColor(ink)
        ctx.fillEllipse(in: CGRect(x: cx - 48, y: 404, width: 96, height: 96))
        ctx.setFillColor(rgb(0x3A4A70))
        ctx.fillEllipse(in: CGRect(x: cx - 28, y: 424, width: 56, height: 56))
    }
    // touchpad with a glowing light-bar edge
    let touch = CGPath(roundedRect: CGRect(x: 438, y: 540, width: 148, height: 72),
                       cornerWidth: 18, cornerHeight: 18, transform: nil)
    ctx.addPath(touch)
    ctx.setFillColor(rgb(0xDDE4F5))
    ctx.fillPath()
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 16, color: rgb(0x4FA3FF, 0.9))
    ctx.addPath(touch)
    ctx.setStrokeColor(rgb(0x3D8BFF))
    ctx.setLineWidth(10)
    ctx.strokePath()
    ctx.restoreGState()

    return ctx.makeImage()!
}

let sizes = [16, 32, 64, 128, 256, 512, 1024]
for px in sizes {
    let url = outDir.appendingPathComponent("icon_\(px).png")
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, render(px), nil)
    CGImageDestinationFinalize(dest)
}

let entries: [(String, String, Int)] = [
    ("16x16", "1x", 16), ("16x16", "2x", 32), ("32x32", "1x", 32), ("32x32", "2x", 64),
    ("128x128", "1x", 128), ("128x128", "2x", 256), ("256x256", "1x", 256), ("256x256", "2x", 512),
    ("512x512", "1x", 512), ("512x512", "2x", 1024),
]
let images = entries.map { "    { \"idiom\" : \"mac\", \"size\" : \"\($0.0)\", \"scale\" : \"\($0.1)\", \"filename\" : \"icon_\($0.2).png\" }" }
let json = "{\n  \"images\" : [\n" + images.joined(separator: ",\n") +
    "\n  ],\n  \"info\" : { \"author\" : \"xcode\", \"version\" : 1 }\n}\n"
try json.write(to: outDir.appendingPathComponent("Contents.json"), atomically: true, encoding: .utf8)
print("wrote \(sizes.count) icons to \(outDir.path)")
