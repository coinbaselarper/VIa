#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <string>

static double now() { return [NSDate timeIntervalSinceReferenceDate]; }

static MTLPixelFormat color_format(NSString *s) {
    if ([s isEqualToString:@"f"]) return MTLPixelFormatRGBA16Float;
    if ([s isEqualToString:@"u"]) return MTLPixelFormatRGBA32Uint;
    if ([s isEqualToString:@"i"]) return MTLPixelFormatRGBA32Sint;
    return MTLPixelFormatInvalid;
}

static MTLVertexFormat vertex_format(MTLDataType t) {
    switch (t) {
        case MTLDataTypeFloat: return MTLVertexFormatFloat;
        case MTLDataTypeFloat2: return MTLVertexFormatFloat2;
        case MTLDataTypeFloat3: return MTLVertexFormatFloat3;
        case MTLDataTypeFloat4: return MTLVertexFormatFloat4;
        case MTLDataTypeHalf: return MTLVertexFormatHalf;
        case MTLDataTypeHalf2: return MTLVertexFormatHalf2;
        case MTLDataTypeHalf3: return MTLVertexFormatHalf3;
        case MTLDataTypeHalf4: return MTLVertexFormatHalf4;
        case MTLDataTypeUInt: return MTLVertexFormatUInt;
        case MTLDataTypeUInt2: return MTLVertexFormatUInt2;
        case MTLDataTypeUInt3: return MTLVertexFormatUInt3;
        case MTLDataTypeUInt4: return MTLVertexFormatUInt4;
        case MTLDataTypeInt: return MTLVertexFormatInt;
        case MTLDataTypeInt2: return MTLVertexFormatInt2;
        case MTLDataTypeInt3: return MTLVertexFormatInt3;
        case MTLDataTypeInt4: return MTLVertexFormatInt4;
        default: return MTLVertexFormatFloat4;
    }
}

static NSArray *struct_members(MTLStructType *st) {
    NSMutableArray *a = [NSMutableArray array];
    for (MTLStructMember *m in st.members) {
        NSMutableDictionary *d = [@{@"name": m.name, @"offset": @(m.offset), @"type": @(m.dataType)} mutableCopy];
        if (m.dataType == MTLDataTypeArray) {
            d[@"array_len"] = @(m.arrayType.arrayLength);
            d[@"element"] = @(m.arrayType.elementType);
            d[@"stride"] = @(m.arrayType.stride);
        }
        if (m.argumentIndex) d[@"id"] = @(m.argumentIndex);
        [a addObject:d];
    }
    return a;
}

static NSArray *bindings(NSArray<id<MTLBinding>> *bs) {
    NSMutableArray *a = [NSMutableArray array];
    for (id<MTLBinding> b in bs) {
        NSMutableDictionary *d = [@{@"name": b.name, @"index": @(b.index), @"type": @(b.type), @"used": @(b.used)} mutableCopy];
        if (b.type == MTLBindingTypeBuffer) {
            id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
            d[@"size"] = @(bb.bufferDataSize);
            d[@"data_type"] = @(bb.bufferDataType);
            if (bb.bufferStructType) d[@"members"] = struct_members(bb.bufferStructType);
            else if (bb.bufferPointerType.elementStructType) d[@"members"] = struct_members(bb.bufferPointerType.elementStructType);
        }
        [a addObject:d];
    }
    return a;
}

static MTLVertexDescriptor *vertex_desc(NSArray<MTLAttribute *> *attrs) {
    MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
    for (MTLAttribute *a in attrs) {
        vd.attributes[a.attributeIndex].format = vertex_format(a.attributeType);
        vd.attributes[a.attributeIndex].bufferIndex = 30;
        vd.attributes[a.attributeIndex].offset = 0;
    }
    vd.layouts[30].stride = 64;
    return vd;
}

static MTLStageInputOutputDescriptor *stage_desc(NSArray<MTLAttribute *> *attrs) {
    MTLStageInputOutputDescriptor *sd = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    for (MTLAttribute *a in attrs) {
        sd.attributes[a.attributeIndex].format = (MTLAttributeFormat)vertex_format(a.attributeType);
        sd.attributes[a.attributeIndex].bufferIndex = 30;
        sd.attributes[a.attributeIndex].offset = 0;
    }
    sd.layouts[30].stride = 64;
    sd.layouts[30].stepFunction = MTLStepFunctionThreadPositionInGridX;
    return sd;
}

static NSDictionary *run(id<MTLDevice> dev, NSDictionary *job) {
    NSMutableDictionary *r = [NSMutableDictionary dictionary];
    if (job[@"key"]) r[@"key"] = job[@"key"];
    NSString *mode = job[@"mode"];
    NSError *e = nil;
    double t = now();
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:job[@"lib"]] error:&e];
    r[@"lib_s"] = @(now() - t);
    if (!lib) { r[@"result"] = @"LIB_FAIL"; r[@"error"] = e.localizedDescription ?: @""; return r; }
    id<MTLFunction> f = [lib newFunctionWithName:job[@"fn"] ?: @"main0"];
    if (!f) { r[@"result"] = @"NO_FUNCTION"; r[@"functions"] = lib.functionNames; return r; }
    BOOL reflect = [job[@"reflect"] boolValue];
    MTLPipelineOption opt = reflect ? (MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo) : MTLPipelineOptionNone;
    id pso = nil;
    t = now();
    if ([mode isEqualToString:@"cs"] || [mode isEqualToString:@"vs_kernel"]) {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = f;
        if (f.stageInputAttributes.count) d.stageInputDescriptor = stage_desc(f.stageInputAttributes);
        MTLComputePipelineReflection *refl = nil;
        id<MTLComputePipelineState> c = [dev newComputePipelineStateWithDescriptor:d options:opt reflection:reflect ? &refl : nil error:&e];
        if (c) { r[@"threadExecutionWidth"] = @(c.threadExecutionWidth); r[@"maxThreads"] = @(c.maxTotalThreadsPerThreadgroup);
                 r[@"static_tg_mem"] = @(c.staticThreadgroupMemoryLength); }
        if (refl) r[@"bindings"] = bindings(refl.bindings);
        pso = c;
    } else if ([mode isEqualToString:@"vs"] || [mode isEqualToString:@"ps"]) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        NSString *stub = job[@"stub"];
        NSString *src = stub ? [NSString stringWithContentsOfFile:stub encoding:NSUTF8StringEncoding error:nil] : nil;
        if ([mode isEqualToString:@"vs"]) {
            if (!src) src = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 stub_main() { return float4(0); }";
        } else if (!src) {
            src = @"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 stub_main(uint i [[vertex_id]]) { return float4(0,0,0,1); }";
        }
        id<MTLLibrary> sl = [dev newLibraryWithSource:src options:nil error:&e];
        if (!sl) { r[@"result"] = @"STUB_FAIL"; r[@"error"] = e.localizedDescription ?: @""; return r; }
        id<MTLFunction> sf = [sl newFunctionWithName:@"stub_main"];
        if ([mode isEqualToString:@"vs"]) {
            d.vertexFunction = f;
            d.fragmentFunction = sf;
            d.vertexDescriptor = vertex_desc(f.stageInputAttributes);
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        } else {
            d.vertexFunction = sf;
            d.fragmentFunction = f;
            NSArray *cs = [job[@"colors"] ?: @"" componentsSeparatedByString:@","];
            for (NSUInteger i = 0; i < cs.count && i < 8; i++) d.colorAttachments[i].pixelFormat = color_format(cs[i]);
            if ([job[@"stencil"] boolValue]) {
                d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            } else if ([job[@"depth"] boolValue]) {
                d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            }
        }
        MTLRenderPipelineReflection *refl = nil;
        id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d options:opt reflection:reflect ? &refl : nil error:&e];
        if (refl) r[@"bindings"] = bindings([mode isEqualToString:@"vs"] ? refl.vertexBindings : refl.fragmentBindings);
        pso = p;
    } else if ([mode isEqualToString:@"ms"]) {
        MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
        d.meshFunction = f;
        d.rasterizationEnabled = NO;
        if (job[@"obj_lib"]) {
            id<MTLLibrary> ol = [dev newLibraryWithURL:[NSURL fileURLWithPath:job[@"obj_lib"]] error:&e];
            id<MTLFunction> of = ol ? [ol newFunctionWithName:job[@"obj_fn"] ?: @"main0"] : nil;
            if (!of) { r[@"result"] = @"OBJ_FAIL"; r[@"error"] = e.localizedDescription ?: @"no object function"; return r; }
            d.objectFunction = of;
            d.maxTotalThreadsPerObjectThreadgroup = 1;
            d.payloadMemoryLength = [job[@"payload_bytes"] unsignedIntegerValue] ?: 24;
        }
        if (job[@"mesh_threads"]) d.maxTotalThreadsPerMeshThreadgroup = [job[@"mesh_threads"] unsignedIntegerValue];
        MTLRenderPipelineReflection *refl = nil;
        id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithMeshDescriptor:d options:opt reflection:reflect ? &refl : nil error:&e];
        if (p) r[@"meshThreadExecutionWidth"] = @(p.meshThreadExecutionWidth);
        if (refl) r[@"bindings"] = bindings(refl.meshBindings);
        pso = p;
    } else {
        r[@"result"] = @"BAD_MODE";
        return r;
    }
    r[@"pso_s"] = @(now() - t);
    r[@"result"] = pso ? @"OK" : @"FAIL";
    if (!pso) r[@"error"] = e.localizedDescription ?: @"";
    return r;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("{\"result\":\"NO_DEVICE\"}\n"); return 1; }
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, stdin)) > 0) {
            @autoreleasepool {
                NSData *data = [NSData dataWithBytes:line length:(NSUInteger)n];
                NSDictionary *job = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
                NSDictionary *r = job ? run(dev, job) : @{@"result": @"BAD_JOB"};
                NSData *j = [NSJSONSerialization dataWithJSONObject:r options:0 error:nil];
                fwrite(j.bytes, 1, j.length, stdout);
                fputc('\n', stdout);
                fflush(stdout);
            }
        }
        free(line);
    }
    return 0;
}
