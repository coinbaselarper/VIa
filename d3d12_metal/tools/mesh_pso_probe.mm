// mesh_pso_probe MESH.metallib [PS.metallib] ...: whether Metal creates a mesh render pipeline from each mesh library
// (entry main0), with the pixel library when given (G-buffer attachments: RGBA8 sRGB, 4 x RGBA8, RG16F, D32S8) or
// rasterisation off. One line per mesh library: ok, or Metal's error.
//   clang++ -std=c++20 -fobjc-arc -O1 mesh_pso_probe.mm -framework Metal -framework Foundation -o mesh_pso_probe

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <string>

static id<MTLFunction> entry(id<MTLDevice> device, const char* path, std::string& error) {
    NSError* e = nil;
    id<MTLLibrary> lib = [device newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&e];
    if (!lib) {
        error = e.localizedDescription.UTF8String;
        return nil;
    }
    id<MTLFunction> f = [lib newFunctionWithName:@"main0"];
    if (!f) error = "no main0";
    return f;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        std::string ps_path;
        int failed = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--ps" && i + 1 < argc) {
                ps_path = argv[++i];
                continue;
            }
            std::string error;
            id<MTLFunction> mesh = entry(device, a.c_str(), error);
            id<MTLFunction> ps = mesh && !ps_path.empty() ? entry(device, ps_path.c_str(), error) : nil;
            if (!mesh || (!ps_path.empty() && !ps)) {
                std::printf("%s\tlibrary\t%s\n", a.c_str(), error.c_str());
                ++failed;
                continue;
            }
            MTLMeshRenderPipelineDescriptor* d = [MTLMeshRenderPipelineDescriptor new];
            d.meshFunction = mesh;
            if (ps) {
                d.fragmentFunction = ps;
                const MTLPixelFormat formats[6] = {MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm,
                                                   MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm,
                                                   MTLPixelFormatRG16Float, MTLPixelFormatRGBA8Unorm};
                for (int k = 0; k < 6; ++k) d.colorAttachments[k].pixelFormat = formats[k];
                d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            } else {
                d.rasterizationEnabled = NO;
            }
            NSError* e = nil;
            id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithMeshDescriptor:d
                                                                                    options:MTLPipelineOptionNone
                                                                                 reflection:nil
                                                                                      error:&e];
            if (p)
                std::printf("%s\tok\tmax threads %lu\n", a.c_str(), (unsigned long)p.maxTotalThreadsPerMeshThreadgroup);
            else
                std::printf("%s\tfailed\t%s\n", a.c_str(), e.localizedDescription.UTF8String);
            failed += p ? 0 : 1;
        }
        return failed ? 1 : 0;
    }
}
