// Pairwise difference of two images of the same size: mean |a-b| per RGB channel (0-255), share of pixels whose largest
// channel difference exceeds 16 / 48, each image's mean, and the share of near-black pixels in each.
// build: clang++ -std=c++17 -O2 -fobjc-arc gbudget_imgdiff.mm -framework CoreGraphics -framework ImageIO -framework Foundation -o imgdiff
// usage: imgdiff A.png B.png [OUT_DIFF.png]
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static bool load(const char* path, std::vector<unsigned char>& px, size_t& w, size_t& h) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    w = CGImageGetWidth(img);
    h = CGImageGetHeight(img);
    px.assign(w * h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    return true;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) {
            std::fprintf(stderr, "usage: imgdiff A.png B.png [OUT_DIFF.png]\n");
            return 2;
        }
        std::vector<unsigned char> a, b;
        size_t wa, ha, wb, hb;
        if (!load(argv[1], a, wa, ha) || !load(argv[2], b, wb, hb)) {
            std::fprintf(stderr, "cannot read an image\n");
            return 1;
        }
        if (wa != wb || ha != hb) {
            std::fprintf(stderr, "sizes differ: %zux%zu vs %zux%zu\n", wa, ha, wb, hb);
            return 1;
        }
        const size_t n = wa * ha;
        double d[3] = {0, 0, 0}, ma[3] = {0, 0, 0}, mb[3] = {0, 0, 0};
        size_t over16 = 0, over48 = 0, blacka = 0, blackb = 0;
        std::vector<unsigned char> out(n * 4, 255);
        for (size_t i = 0; i < n; ++i) {
            int big = 0, maxa = 0, maxb = 0;
            for (int c = 0; c < 3; ++c) {
                const int x = a[i * 4 + c], y = b[i * 4 + c];
                const int e = std::abs(x - y);
                d[c] += e;
                ma[c] += x;
                mb[c] += y;
                big = std::max(big, e);
                maxa = std::max(maxa, x);
                maxb = std::max(maxb, y);
                out[i * 4 + c] = (unsigned char)std::min(255, e * 4);
            }
            over16 += big > 16;
            over48 += big > 48;
            blacka += maxa < 4;
            blackb += maxb < 4;
        }
        std::printf("%zux%zu mean|a-b| %.2f %.2f %.2f; >16: %.2f%%; >48: %.2f%%; mean a %.1f %.1f %.1f b %.1f %.1f %.1f; "
                    "near-black a %.2f%% b %.2f%%\n",
                    wa, ha, d[0] / n, d[1] / n, d[2] / n, 100.0 * over16 / n, 100.0 * over48 / n, ma[0] / n, ma[1] / n,
                    ma[2] / n, mb[0] / n, mb[1] / n, mb[2] / n, 100.0 * blacka / n, 100.0 * blackb / n);
        if (argc > 3) {
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = CGBitmapContextCreate(out.data(), wa, ha, 8, wa * 4, cs, kCGImageAlphaNoneSkipLast);
            CGImageRef img = CGBitmapContextCreateImage(ctx);
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[3]]];
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
            CGImageDestinationAddImage(dst, img, nullptr);
            CGImageDestinationFinalize(dst);
            CFRelease(dst);
            CGImageRelease(img);
            CGContextRelease(ctx);
            CGColorSpaceRelease(cs);
        }
    }
    return 0;
}
