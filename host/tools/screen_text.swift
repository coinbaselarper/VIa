// usage: screen_text [--stats] PNG...
// Default: the text macOS Vision recognises in each image, one line per image: "<path>\t<line> | <line> | ...".
// --stats: "<path>\tmean R G B\tcolours N" (distinct RGB values, counted up to 65536, printed as 65536+ there).
// Used to prove from the present-time PNG dumps which title page or movie frame was on screen.
import AppKit
import Foundation
import Vision

func cgImage(_ path: String) -> CGImage? {
    guard let image = NSImage(contentsOfFile: path) else { return nil }
    return image.cgImage(forProposedRect: nil, context: nil, hints: nil)
}

func stats(_ cg: CGImage) -> String {
    let w = cg.width, h = cg.height
    var px = [UInt8](repeating: 0, count: w * h * 4)
    let space = CGColorSpaceCreateDeviceRGB()
    px.withUnsafeMutableBytes { buf in
        let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                            space: space, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
    }
    var sum = [UInt64](repeating: 0, count: 3)
    var colours = Set<UInt32>()
    var i = 0
    while i < px.count {
        let r = px[i], g = px[i + 1], b = px[i + 2]
        sum[0] += UInt64(r); sum[1] += UInt64(g); sum[2] += UInt64(b)
        if colours.count < 65536 { colours.insert(UInt32(r) << 16 | UInt32(g) << 8 | UInt32(b)) }
        i += 4
    }
    let n = Double(w * h)
    let mean = String(format: "mean %.1f %.1f %.1f", Double(sum[0]) / n, Double(sum[1]) / n, Double(sum[2]) / n)
    return "\(mean)\tcolours \(colours.count)\(colours.count >= 65536 ? "+" : "")"
}

var args = Array(CommandLine.arguments.dropFirst())
let statsMode = args.first == "--stats"
if statsMode { args.removeFirst() }
var failed = false
for path in args {
    guard let cg = cgImage(path) else {
        FileHandle.standardError.write("screen_text: cannot read \(path)\n".data(using: .utf8)!)
        failed = true
        continue
    }
    if statsMode {
        print("\(path)\t\(stats(cg))")
        continue
    }
    let request = VNRecognizeTextRequest()
    request.recognitionLevel = .accurate
    request.usesLanguageCorrection = false
    do {
        try VNImageRequestHandler(cgImage: cg, options: [:]).perform([request])
    } catch {
        FileHandle.standardError.write("screen_text: \(path): \(error)\n".data(using: .utf8)!)
        failed = true
        continue
    }
    let lines = (request.results ?? []).compactMap { $0.topCandidates(1).first?.string }
    print("\(path)\t\(lines.joined(separator: " | "))")
}
exit(failed ? 1 : 0)
