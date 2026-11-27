// What a Vulkan driver does with f16 denormals and f32 NaN/inf/-0 when a module declares the translator's float-controls
// execution modes, only some of them, or none (port/shaders_vk/STATUS.md). Runs fc_probe.comp in three variants:
//   none  : no float-controls modes            (rdna2spirv --no-float-controls)
//   apple : SignedZeroInfNanPreserve 32        (rdna2spirv --apple-float-controls)
//   full  : SignedZeroInfNanPreserve 32 + DenormPreserve 16 (the unmodified translator)
//   fc_vulkan_probe DRIVER.dylib DIR   (DIR holds none.spv, apple.spv, full.spv)
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr unsigned kF16Tests = 21, kF32Tests = 16, kLanes = 2048;
const char* const kF16Names[kF16Tests] = {"half->float", "h*1",     "h+0",       "h+h",      "fma(h,1,0)", "float->half",
                                          "h*0.5",       "max/min", "abs",       "neg",      "h/1",        "sqrt(|h|)",
                                          "h!=0",        "clamp",   "mix(0,h,1)", "dot",     "exp2(-14-k)", "h-0",
                                          "UnpackHalf2x16", "PackHalf2x16", "QuantizeToF16"};
const char* const kF32Names[kF32Tests] = {"isnan(NaN)",   "NaN!=NaN",   "isinf(inf)", "bits(-1*0)",  "NaN<1||NaN>=1",
                                          "0/0 is NaN",   "1/0",        "-0+0",       "NaN==NaN?0:1", "inf*0 is NaN",
                                          "-0*1",         "inf-inf NaN", "NaN*1 NaN",  "1/-0",        "isnan(NaN*1)",
                                          "-0 passes"};

float half_to_float(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(1024 + m), e - 25);
    return (h & 0x8000) ? -v : v;
}

uint16_t float_to_half_rte(float f) {
    const uint16_t sign = std::signbit(f) ? 0x8000 : 0;
    const float a = std::fabs(f);
    uint16_t best = 0;
    float err = INFINITY;
    for (uint16_t h = 0; h < 0x7c00; ++h) {
        const float d = std::fabs(half_to_float(h) - a);
        if (d < err || (d == err && !(h & 1))) {
            err = d;
            best = h;
        }
    }
    return uint16_t(sign | best);
}

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

bool is_nan_bits(uint32_t u) { return (u & 0x7f800000u) == 0x7f800000u && (u & 0x7fffffu); }

std::vector<uint32_t> read_spv(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::vector<uint32_t> w(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(w.data()), std::streamsize(w.size() * 4));
    return w;
}

#define LOAD(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: fc_vulkan_probe DRIVER.dylib DIR\n");
        return 2;
    }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr"));
    if (!gipa) {
        std::fprintf(stderr, "no vkGetInstanceProcAddr\n");
        return 2;
    }
    VkInstance instance = VK_NULL_HANDLE;
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (create_instance(&ici, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateInstance failed\n");
        return 2;
    }
    LOAD(vkEnumeratePhysicalDevices);
    LOAD(vkGetPhysicalDeviceProperties2);
    LOAD(vkGetPhysicalDeviceFeatures2);
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(vkGetPhysicalDeviceMemoryProperties);
    LOAD(vkCreateDevice);
    LOAD(vkGetDeviceProcAddr);
    uint32_t count = 1;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &count, &phys);
    if (!phys) return 2;

    VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &fc};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(phys, &props);
    std::printf("device %s, driver %s %s\n", props.properties.deviceName, drv.driverName, drv.driverInfo);
    std::printf("properties: shaderDenormPreserveFloat16=%u shaderDenormFlushToZeroFloat16=%u "
                "shaderSignedZeroInfNanPreserveFloat32=%u shaderSignedZeroInfNanPreserveFloat16=%u denormBehaviorIndependence=%d\n",
                fc.shaderDenormPreserveFloat16, fc.shaderDenormFlushToZeroFloat16, fc.shaderSignedZeroInfNanPreserveFloat32,
                fc.shaderSignedZeroInfNanPreserveFloat16, int(fc.denormBehaviorIndependence));

    VkPhysicalDeviceShaderFloat16Int8Features f16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f16};
    vkGetPhysicalDeviceFeatures2(phys, &feats);
    if (!f16.shaderFloat16 || !feats.features.shaderInt16) {
        std::fprintf(stderr, "device lacks shaderFloat16 (%u) or shaderInt16 (%u)\n", f16.shaderFloat16,
                     feats.features.shaderInt16);
        return 77;
    }
    VkPhysicalDeviceShaderFloat16Int8Features f16_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    f16_on.shaderFloat16 = VK_TRUE;
    VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f16_on};
    enable.features.shaderInt16 = VK_TRUE;

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qf.data());
    uint32_t family = 0;
    while (family < qcount && !(qf[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* exts[] = {"VK_KHR_portability_subset"};
    uint32_t ext_count = 0;
    {
        LOAD(vkEnumerateDeviceExtensionProperties);
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> e(n);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, e.data());
        for (auto& x : e)
            if (!std::strcmp(x.extensionName, exts[0])) ext_count = 1;
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enable};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ext_count;
    dci.ppEnabledExtensionNames = exts;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateDevice failed\n");
        return 2;
    }
    auto gdpa = vkGetDeviceProcAddr;
#define DLOAD(name) auto name = reinterpret_cast<PFN_##name>(gdpa(dev, #name))
    DLOAD(vkGetDeviceQueue);
    DLOAD(vkCreateBuffer);
    DLOAD(vkGetBufferMemoryRequirements);
    DLOAD(vkAllocateMemory);
    DLOAD(vkBindBufferMemory);
    DLOAD(vkMapMemory);
    DLOAD(vkCreateShaderModule);
    DLOAD(vkCreateDescriptorSetLayout);
    DLOAD(vkCreatePipelineLayout);
    DLOAD(vkCreateComputePipelines);
    DLOAD(vkCreateDescriptorPool);
    DLOAD(vkAllocateDescriptorSets);
    DLOAD(vkUpdateDescriptorSets);
    DLOAD(vkCreateCommandPool);
    DLOAD(vkAllocateCommandBuffers);
    DLOAD(vkBeginCommandBuffer);
    DLOAD(vkCmdBindPipeline);
    DLOAD(vkCmdBindDescriptorSets);
    DLOAD(vkCmdDispatch);
    DLOAD(vkEndCommandBuffer);
    DLOAD(vkQueueSubmit);
    DLOAD(vkQueueWaitIdle);
    DLOAD(vkResetCommandBuffer);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    auto make_buffer = [&](VkDeviceSize size, void** mapped) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VkBuffer b = VK_NULL_HANDLE;
        vkCreateBuffer(dev, &bci, nullptr, &b);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev, b, &req);
        uint32_t type = 0;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (type < mem.memoryTypeCount &&
               (!(req.memoryTypeBits & (1u << type)) || (mem.memoryTypes[type].propertyFlags & want) != want))
            ++type;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        VkDeviceMemory m = VK_NULL_HANDLE;
        vkAllocateMemory(dev, &mai, nullptr, &m);
        vkBindBufferMemory(dev, b, m, 0);
        vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, mapped);
        return b;
    };
    const size_t out_words = size_t(kLanes) * kF16Tests + kF32Tests;
    void *in_map = nullptr, *out_map = nullptr;
    VkBuffer in_buf = make_buffer(4 * 2056, &in_map);
    VkBuffer out_buf = make_buffer(4 * out_words, &out_map);
    auto* in = static_cast<uint32_t*>(in_map);
    for (uint32_t i = 0; i < 1024; ++i) in[i] = i;
    for (uint32_t i = 0; i < 1024; ++i) in[1024 + i] = 0x8000u | i;
    in[2048] = 0x3c00;
    in[2049] = 0;
    in[2050] = 0x3800;
    in[2051] = 0x3f800000u;
    in[2052] = 0x7fc00000u;
    in[2053] = 0x7f800000u;
    in[2054] = 0x80000000u;
    in[2055] = 0;

    VkDescriptorSetLayoutBinding binds[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
        binds[b].binding = b;
        binds[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[b].descriptorCount = 1;
        binds[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 2;
    dsl.pBindings = binds;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &set_layout);
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(dev, &plci, nullptr, &layout);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(dev, &dpci, nullptr, &pool);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(dev, &dsai, &set);
    VkDescriptorBufferInfo infos[2] = {{in_buf, 0, VK_WHOLE_SIZE}, {out_buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b].dstSet = set;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &infos[b];
    }
    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    VkCommandPool cpool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &cpci, nullptr, &cpool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cbai, &cb);

    const auto* out = static_cast<const uint32_t*>(out_map);
    int total_wrong = 0;
    for (const char* variant : {"none", "apple", "full"}) {
        const auto words = read_spv(std::string(argv[2]) + "/" + variant + ".spv");
        if (words.empty()) {
            std::fprintf(stderr, "%s.spv missing\n", variant);
            return 2;
        }
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = words.size() * 4;
        smci.pCode = words.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev, &smci, nullptr, &module) != VK_SUCCESS) {
            std::printf("%s: vkCreateShaderModule failed\n", variant);
            continue;
        }
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = layout;
        VkPipeline pipe = VK_NULL_HANDLE;
        const VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe);
        if (pr != VK_SUCCESS) {
            std::printf("%s: vkCreateComputePipelines returned %d\n", variant, int(pr));
            continue;
        }
        std::memset(out_map, 0xcd, 4 * out_words);
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &cbbi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cb, kLanes / 64, 1, 1);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        int wrong_f16[kF16Tests] = {}, subnormal_lanes = 0;
        uint32_t first_bad[kF16Tests];
        std::memset(first_bad, 0xff, sizeof first_bad);
        for (uint32_t i = 0; i < kLanes; ++i) {
            const uint16_t h = uint16_t(in[i]);
            if ((h & 0x7fff) == 0) continue;
            ++subnormal_lanes;
            const float v = half_to_float(h);
            const uint16_t neg = uint16_t(h ^ 0x8000);
            const uint16_t mag = uint16_t(h & 0x7fff);
            const uint16_t half_v = float_to_half_rte(v * 0.5f);
            const uint16_t sq = float_to_half_rte(std::sqrt(std::fabs(v)));
            const uint32_t k = i & 15;
            const uint16_t ex = float_to_half_rte(std::ldexp(1.0f, -14 - int(k)));
            const uint32_t expect[kF16Tests] = {float_bits(v), h,   h,   float_to_half_rte(v + v), h,   h,
                                                half_v,        h,   mag, neg,                      h,   sq,
                                                1,             h,   h,   h,                        ex,  h,
                                                float_bits(v), h,   float_bits(v)};
            for (unsigned t = 0; t < kF16Tests; ++t) {
                const uint32_t got = out[i * kF16Tests + t];
                const bool approximate = t == 11 || t == 16;
                const bool close = approximate && (expect[t] == 0 ? got == 0
                                                                  : got != 0 && (got > expect[t] ? got - expect[t]
                                                                                                 : expect[t] - got) <= 1);
                if (got != expect[t] && !close) {
                    if (!wrong_f16[t]) first_bad[t] = i;
                    ++wrong_f16[t];
                }
            }
        }
        std::printf("%s: f16 over %d subnormal inputs (wrong / tested):", variant, subnormal_lanes);
        int wrong_total = 0;
        for (unsigned t = 0; t < kF16Tests; ++t) {
            std::printf(" %s %d", kF16Names[t], wrong_f16[t]);
            wrong_total += wrong_f16[t];
        }
        std::printf("\n");
        for (unsigned t = 0; t < kF16Tests; ++t)
            if (wrong_f16[t]) {
                const uint32_t i = first_bad[t];
                std::printf("  %s first wrong: input 0x%04x -> 0x%08x\n", kF16Names[t], in[i], out[i * kF16Tests + t]);
            }
        const uint32_t* r = out + size_t(kLanes) * kF16Tests;
        const bool ok32[kF32Tests] = {r[0] == 1,
                                      r[1] == 1,
                                      r[2] == 1,
                                      r[3] == 0x80000000u,
                                      r[4] == 0,
                                      is_nan_bits(r[5]),
                                      r[6] == 0x7f800000u,
                                      r[7] == 0,
                                      r[8] == 1,
                                      is_nan_bits(r[9]),
                                      r[10] == 0x80000000u,
                                      is_nan_bits(r[11]),
                                      is_nan_bits(r[12]),
                                      r[13] == 0xff800000u,
                                      r[14] == 1,
                                      r[15] == 0x80000000u};
        int wrong32 = 0;
        std::printf("%s: f32 NaN/inf/-0:", variant);
        for (unsigned t = 0; t < kF32Tests; ++t) {
            if (!ok32[t]) ++wrong32;
            std::printf(" %s=%s", kF32Names[t], ok32[t] ? "ok" : "WRONG");
            if (!ok32[t]) std::printf("(0x%08x)", r[t]);
        }
        std::printf("\n%s: %d f16 lane results wrong, %d of %u f32 checks wrong\n", variant, wrong_total, wrong32, kF32Tests);
        total_wrong += wrong_total + wrong32;
    }
    std::printf("total wrong: %d\n", total_wrong);
    return 0;
}
