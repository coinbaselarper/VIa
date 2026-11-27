#include "mtl_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include <gta6rt_hud/stats.h>

namespace gta6rt::gpu {

const char* limit_kind_name(PipelineLimit::Kind k) {
    switch (k) {
    case PipelineLimit::Kind::none: return "none";
    case PipelineLimit::Kind::threadgroup_memory: return "threadgroup_memory";
    case PipelineLimit::Kind::mesh_size: return "mesh_size";
    case PipelineLimit::Kind::mesh_scalars: return "mesh_scalars";
    case PipelineLimit::Kind::threads_per_group: return "threads_per_group";
    }
    return "unknown";
}

}  // namespace gta6rt::gpu

namespace gta6rt::metal {

namespace {

MTLBlendFactor blend_factor(Blend b) {
    switch (b) {
    case Blend::zero: return MTLBlendFactorZero;
    case Blend::one: return MTLBlendFactorOne;
    case Blend::src_color: return MTLBlendFactorSourceColor;
    case Blend::inv_src_color: return MTLBlendFactorOneMinusSourceColor;
    case Blend::src_alpha: return MTLBlendFactorSourceAlpha;
    case Blend::inv_src_alpha: return MTLBlendFactorOneMinusSourceAlpha;
    case Blend::dest_alpha: return MTLBlendFactorDestinationAlpha;
    case Blend::inv_dest_alpha: return MTLBlendFactorOneMinusDestinationAlpha;
    case Blend::dest_color: return MTLBlendFactorDestinationColor;
    case Blend::inv_dest_color: return MTLBlendFactorOneMinusDestinationColor;
    case Blend::src_alpha_sat: return MTLBlendFactorSourceAlphaSaturated;
    case Blend::blend_factor: return MTLBlendFactorBlendColor;
    case Blend::inv_blend_factor: return MTLBlendFactorOneMinusBlendColor;
    case Blend::src1_color: return MTLBlendFactorSource1Color;
    case Blend::inv_src1_color: return MTLBlendFactorOneMinusSource1Color;
    case Blend::src1_alpha: return MTLBlendFactorSource1Alpha;
    case Blend::inv_src1_alpha: return MTLBlendFactorOneMinusSource1Alpha;
    }
    return MTLBlendFactorOne;
}

MTLBlendOperation blend_op(BlendOp op) {
    switch (op) {
    case BlendOp::add: return MTLBlendOperationAdd;
    case BlendOp::subtract: return MTLBlendOperationSubtract;
    case BlendOp::rev_subtract: return MTLBlendOperationReverseSubtract;
    case BlendOp::min: return MTLBlendOperationMin;
    case BlendOp::max: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

MTLStencilOperation stencil_op(StencilOp op) {
    switch (op) {
    case StencilOp::keep: return MTLStencilOperationKeep;
    case StencilOp::zero: return MTLStencilOperationZero;
    case StencilOp::replace: return MTLStencilOperationReplace;
    case StencilOp::incr_sat: return MTLStencilOperationIncrementClamp;
    case StencilOp::decr_sat: return MTLStencilOperationDecrementClamp;
    case StencilOp::invert: return MTLStencilOperationInvert;
    case StencilOp::incr: return MTLStencilOperationIncrementWrap;
    case StencilOp::decr: return MTLStencilOperationDecrementWrap;
    }
    return MTLStencilOperationKeep;
}

MTLCompareFunction compare(CompareFunc f) {
    return f == CompareFunc::none ? MTLCompareFunctionAlways : MTLCompareFunction(std::uint32_t(f) - 1);
}

id<MTLDepthStencilState> make_depth_state(id<MTLDevice> device, const DepthStencilDesc& ds) {
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = ds.depth_test ? compare(ds.depth_func) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = ds.depth_test && ds.depth_write;
    if (ds.stencil) {
        auto face = [&](const StencilFace& f) {
            MTLStencilDescriptor* s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = compare(f.func);
            s.stencilFailureOperation = stencil_op(f.fail);
            s.depthFailureOperation = stencil_op(f.depth_fail);
            s.depthStencilPassOperation = stencil_op(f.pass);
            s.readMask = ds.stencil_read_mask;
            s.writeMask = ds.stencil_write_mask;
            return s;
        };
        dd.frontFaceStencil = face(ds.front);
        dd.backFaceStencil = face(ds.back);
    }
    return [device newDepthStencilStateWithDescriptor:dd];
}

template <typename Array>
bool set_color_targets(Array* attachments, const std::array<std::uint32_t, 8>& formats, std::uint32_t count,
                       const std::array<TargetBlend, 8>& blends, std::string& err) {
    for (std::uint32_t i = 0; i < count && i < 8; ++i) {
        NativeFormat nf;
        if (!native_format(formats[i], nf)) {
            err = "render target " + std::to_string(i) + ": DXGI format " + std::to_string(formats[i]);
            return false;
        }
        MTLRenderPipelineColorAttachmentDescriptor* ca = attachments[i];
        ca.pixelFormat = nf.pixel;
        const TargetBlend& b = blends[i];
        ca.blendingEnabled = b.enable;
        ca.sourceRGBBlendFactor = blend_factor(b.src);
        ca.destinationRGBBlendFactor = blend_factor(b.dst);
        ca.rgbBlendOperation = blend_op(b.op);
        ca.sourceAlphaBlendFactor = blend_factor(b.src_alpha);
        ca.destinationAlphaBlendFactor = blend_factor(b.dst_alpha);
        ca.alphaBlendOperation = blend_op(b.op_alpha);
        MTLColorWriteMask mask = 0;
        if (b.write_mask & 1) mask |= MTLColorWriteMaskRed;
        if (b.write_mask & 2) mask |= MTLColorWriteMaskGreen;
        if (b.write_mask & 4) mask |= MTLColorWriteMaskBlue;
        if (b.write_mask & 8) mask |= MTLColorWriteMaskAlpha;
        ca.writeMask = mask;
    }
    return true;
}

std::string data_type_name(MTLDataType t) {
    switch (t) {
    case MTLDataTypeStruct: return "struct";
    case MTLDataTypeArray: return "array";
    case MTLDataTypePointer: return "pointer";
    case MTLDataTypeFloat: return "float";
    case MTLDataTypeFloat2: return "float2";
    case MTLDataTypeFloat3: return "float3";
    case MTLDataTypeFloat4: return "float4";
    case MTLDataTypeInt: return "int";
    case MTLDataTypeInt4: return "int4";
    case MTLDataTypeUInt: return "uint";
    case MTLDataTypeUInt2: return "uint2";
    case MTLDataTypeUInt4: return "uint4";
    case MTLDataTypeTexture: return "texture";
    case MTLDataTypeSampler: return "sampler";
    case MTLDataTypeULong: return "ulong";
    default: return "type" + std::to_string(int(t));
    }
}

void fill_members(MTLStructType* st, std::vector<ReflectedMember>& out) {
    for (MTLStructMember* m in st.members) {
        ReflectedMember rm;
        rm.name = m.name.UTF8String;
        rm.offset = m.offset;
        rm.type = data_type_name(m.dataType);
        if (m.dataType == MTLDataTypeArray) {
            MTLArrayType* a = m.arrayType;
            rm.array_length = a.arrayLength;
            rm.type = "array<" + data_type_name(a.elementType) + ">";
        } else if (m.dataType == MTLDataTypePointer) {
            rm.type = "pointer<" + data_type_name(m.pointerType.elementType) + ">";
        }
        out.push_back(std::move(rm));
    }
}

std::vector<ReflectedBinding> convert_bindings(NSArray<id<MTLBinding>>* bindings) {
    std::vector<ReflectedBinding> out;
    for (id<MTLBinding> b in bindings) {
        ReflectedBinding rb;
        rb.name = b.name.UTF8String;
        rb.index = std::uint32_t(b.index);
        rb.used = b.used;
        rb.access = b.access == MTLBindingAccessReadOnly ? "read" : b.access == MTLBindingAccessWriteOnly ? "write" : "read_write";
        switch (b.type) {
        case MTLBindingTypeBuffer: {
            rb.kind = BindingKind::buffer;
            id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
            rb.data_size = bb.bufferDataSize;
            rb.argument_buffer = bb.bufferPointerType ? bb.bufferPointerType.elementIsArgumentBuffer : NO;
            if (bb.bufferStructType) fill_members(bb.bufferStructType, rb.members);
            break;
        }
        case MTLBindingTypeTexture: rb.kind = BindingKind::texture; break;
        case MTLBindingTypeSampler: rb.kind = BindingKind::sampler; break;
        case MTLBindingTypeThreadgroupMemory: rb.kind = BindingKind::other; break;
        default: rb.kind = BindingKind::other; break;
        }
        out.push_back(std::move(rb));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
    return out;
}

const char* stage_name(std::uint32_t stage) {
    static const char* names[] = {"vertex", "fragment", "kernel", "object", "mesh"};
    return stage < 5 ? names[stage] : "?";
}

struct PlanContext {
    const RootLayoutDesc* root = nullptr;
    const std::vector<InputElement>* inputs = nullptr;
    int rect_params_index = -1;
    int d24_params_index = -1;
    PipelineReflection* refl = nullptr;
};

// How the root contract lands on one stage's [[buffer(n)]] slots, from Metal's reflection (DESIGN.md 2.2).
void plan_stage(const std::vector<ReflectedBinding>& bindings, std::uint32_t stage, const PlanContext& c, StagePlan& sp) {
    PipelineReflection& r = *c.refl;
    const std::string sn = std::string(stage_name(stage)) + " ";
    for (const ReflectedBinding& b : bindings) {
        BindingPlanEntry e;
        e.stage = stage;
        e.index = b.index;
        if (b.kind != BindingKind::buffer) {
            if (b.kind == BindingKind::other) continue;
            e.note = "direct " + std::string(b.kind == BindingKind::texture ? "texture" : "sampler") + " binding " +
                     b.name + ": not part of the root contract";
            r.mismatches.push_back(sn + e.note);
            r.plan.push_back(e);
            continue;
        }
        if (stage == 0 && c.inputs && b.index >= 23 && b.index <= 30) {
            bool vb = false;
            for (const InputElement& in : *c.inputs) vb |= 30 - in.slot == b.index;
            if (vb) {
                e.what = BindingPlanEntry::What::vertex_buffer;
                e.binding = 30 - b.index;
                e.note = b.name;
                r.plan.push_back(e);
                continue;
            }
        }
        if (stage == 1 && c.d24_params_index >= 0 && int(b.index) == c.d24_params_index) {
            e.what = BindingPlanEntry::What::d24_params;
            e.note = "D24 depth parameters";
            r.plan.push_back(e);
            continue;
        }
        const std::string prefix = "spvDescriptorSet";
        if (b.name.rfind(prefix, 0) == 0 && b.name.size() > prefix.size() &&
            std::isdigit(static_cast<unsigned char>(b.name[prefix.size()]))) {
            const std::uint32_t set = std::uint32_t(std::stoul(b.name.substr(prefix.size())));
            e.set = set;
            e.what = BindingPlanEntry::What::table;
            if (set == 0) {
                std::string offs;
                for (const auto& m : b.members) offs += (offs.empty() ? "" : ", ") + m.name + "@" + std::to_string(m.offset);
                e.note = "set 0 holds every set-0 binding of the shader (unsplit recipe): members " + offs;
                r.mismatches.push_back(sn + b.name + ": " + e.note + "; the tables cannot be bound separately");
                e.what = BindingPlanEntry::What::unmapped;
            } else {
                e.binding = set - 1;
                bool in_root = false;
                if (c.root)
                    for (const RootTable& t : c.root->tables) in_root |= t.binding == e.binding;
                std::uint64_t max_offset = 0;
                for (const auto& m : b.members) max_offset = std::max(max_offset, m.offset);
                if (max_offset != 0) {
                    e.note = "members at nonzero offsets";
                    r.mismatches.push_back(b.name + ": members at nonzero offsets");
                }
                if (!in_root) {
                    e.note = "binding " + std::to_string(e.binding) + " not in the root layout";
                    r.mismatches.push_back(sn + b.name + " -> binding " + std::to_string(e.binding) +
                                           " not in the root layout");
                    e.what = BindingPlanEntry::What::unmapped;
                } else {
                    sp.tables.emplace_back(e.binding, b.index);
                    e.note = "table binding " + std::to_string(e.binding);
                }
            }
            r.plan.push_back(e);
            continue;
        }
        if (c.root && !b.argument_buffer && b.data_size == c.root->push_bytes && sp.push_index < 0) {
            e.what = BindingPlanEntry::What::push_block;
            sp.push_index = int(b.index);
            e.note = b.name + " (" + std::to_string(b.data_size) + " B push block)";
            r.plan.push_back(e);
            continue;
        }
        if (stage == 0 && c.rect_params_index >= 0 && int(b.index) == c.rect_params_index) {
            e.what = BindingPlanEntry::What::rect_params;
            r.plan.push_back(e);
            continue;
        }
        e.note = b.name + ": " + std::to_string(b.data_size) + " B buffer, no root mapping";
        r.mismatches.push_back(sn + e.note);
        r.plan.push_back(e);
    }
}

std::string ns_error(NSError* e, const char* fallback) {
    return e ? std::string(e.localizedDescription.UTF8String) : std::string(fallback);
}

// Metal's limit errors: "Threadgroup memory size (N) exceeds the maximum threadgroup memory allowed (M)", "Total mesh
// size (N) exceeds the maximum mesh size allowed (M)", "Mesh unique scalar count of N exceeds limit of M".
bool parse_limit(const std::string& text, PipelineLimit& out) {
    std::smatch m;
    static const std::regex tg("Threadgroup memory size \\((\\d+)\\) exceeds the maximum threadgroup memory allowed \\((\\d+)\\)");
    static const std::regex ms("Total mesh size \\((\\d+)\\) exceeds the maximum mesh size allowed \\((\\d+)\\)");
    static const std::regex sc("Mesh unique scalar count of (\\d+) exceeds limit of (\\d+)");
    if (std::regex_search(text, m, tg)) {
        out.kind = PipelineLimit::Kind::threadgroup_memory;
    } else if (std::regex_search(text, m, ms)) {
        out.kind = PipelineLimit::Kind::mesh_size;
    } else if (std::regex_search(text, m, sc)) {
        out.kind = PipelineLimit::Kind::mesh_scalars;
    } else {
        return false;
    }
    out.value = std::stoull(m[1].str());
    out.limit = std::stoull(m[2].str());
    out.reason = text;
    out.estimated = false;
    return true;
}

}  // namespace

// ---- libraries ----

MetalLibrary::MetalLibrary(MetalDevice* device, id<MTLLibrary> library, std::string source, double seconds,
                           MslOptions options)
    : device_(device), library_(library), source_(std::move(source)), seconds_(seconds), options_(options) {
    options_.label = nullptr;
}

id<MTLLibrary> MetalLibrary::mtl(std::string* error) {
    std::lock_guard lock(mutex_);
    if (!library_ && error_.empty()) {
        double secs = 0;
        library_ = device_->compile_msl(source_, options_.language_major, options_.language_minor, options_.fast_math,
                                        &error_, &secs);
        seconds_ = secs;
        if (!library_ && error_.empty()) error_ = "MSL compile failed";
    }
    if (!library_ && error) *error = error_;
    return library_;
}

std::vector<std::string> MetalLibrary::function_names() const {
    std::vector<std::string> out;
    id<MTLLibrary> lib = const_cast<MetalLibrary*>(this)->mtl();
    for (NSString* n in lib.functionNames) out.emplace_back(n.UTF8String);
    return out;
}

static id<MTLFunction> function_of(ShaderLibrary* library, const std::string& entry, std::string& err) {
    auto* lib = static_cast<MetalLibrary*>(library);
    if (!lib) {
        err = "no library";
        return nil;
    }
    std::string lerr;
    id<MTLLibrary> l = lib->mtl(&lerr);
    if (!l) {
        err = "library: " + lerr;
        return nil;
    }
    id<MTLFunction> f = [l newFunctionWithName:@(entry.c_str())];
    if (!f) err = "function '" + entry + "' not found";
    return f;
}

// ---- compile state ----

PipelineCore::PipelineCore(MetalDevice* device, CompileMode mode, NotReady not_ready, Priority priority)
    : device_(device), mode_(mode), not_ready_(not_ready), priority_(priority) {}

void PipelineCore::start() {
    if (state_.load() == PipelineState::failed) return;
    if (mode_ == CompileMode::eager) {
        core_wait();
    } else if (mode_ == CompileMode::async) {
        device_->compiler().enqueue(this, priority_);
    }
}

PipelineState PipelineCore::core_state() const {
    const PipelineState s = state_.load();
    if (s != PipelineState::deferred) return s;
    if (claimed_.load()) return PipelineState::compiling;
    return queued.load() ? PipelineState::queued : s;
}

void PipelineCore::run_build() {
    @autoreleasepool {
        build();
    }
}

Status PipelineCore::core_wait() {
    const PipelineState s = state_.load();
    if (s == PipelineState::ready) return Status::ok;
    if (s == PipelineState::failed) return status_.load();
    if (claim()) {
        device_->compiler().steal(this);
        run_build();
    }
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] {
        const PipelineState t = state_.load();
        return t == PipelineState::ready || t == PipelineState::failed;
    });
    return state_.load() == PipelineState::ready ? Status::ok : status_.load();
}

bool PipelineCore::ready_for_encode(Status* why) {
    const PipelineState s = state_.load();
    if (s == PipelineState::ready) return true;
    if (s == PipelineState::failed) {
        *why = status_.load();
        return false;
    }
    if (not_ready_ == NotReady::wait) {
        const Status st = core_wait();
        if (st == Status::ok) return true;
        *why = st;
        return false;
    }
    if (!claimed_.load() && !queued.load()) device_->compiler().enqueue(this, Priority::demand);
    device_->counters().draws_skipped_not_ready++;
    if (!skip_logged_.exchange(true)) log("command skipped: its pipeline is still compiling (NotReady::skip)");
    *why = Status::not_ready;
    return false;
}

void PipelineCore::finish_ok(double seconds) {
    device_->counters().pipelines_compiled++;
    std::lock_guard lock(mutex_);
    seconds_ = seconds;
    status_ = Status::ok;
    state_ = PipelineState::ready;
    cv_.notify_all();
}

void PipelineCore::finish_fail(Status status, const std::string& error, double seconds) {
    if (status == Status::limit_exceeded) device_->counters().pipelines_refused_limit++;
    std::lock_guard lock(mutex_);
    error_ = error;
    seconds_ = seconds;
    status_ = status;
    state_ = PipelineState::failed;
    cv_.notify_all();
}

void PipelineCore::detach_from_queue() { device_->compiler().forget(this); }

// ---- compile queue ----

namespace {

std::uint32_t env_u32(const char* name, std::uint32_t fallback) {
    const char* v = std::getenv(name);
    return v && *v ? std::uint32_t(std::strtoul(v, nullptr, 10)) : fallback;
}

int qos_named(const char* name, qos_class_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return int(fallback);
    const std::string s = v;
    if (s == "user_interactive") return int(QOS_CLASS_USER_INTERACTIVE);
    if (s == "user_initiated") return int(QOS_CLASS_USER_INITIATED);
    if (s == "default") return int(QOS_CLASS_DEFAULT);
    if (s == "utility") return int(QOS_CLASS_UTILITY);
    if (s == "background") return int(QOS_CLASS_BACKGROUND);
    return int(fallback);
}

}  // namespace

// Compile work runs in MTLCompilerService at the QoS of the thread that asks (XPC carries it), so background
// builds ask at utility and leave the CPU to the title's own threads first.
void CompileQueue::init(std::uint32_t threads) {
    threads_n_ = env_u32("GTA6RT_METAL_COMPILE_THREADS", threads ? threads : 1);
    if (!threads_n_) threads_n_ = 1;
    active_limit_ = threads_n_;
    background_limit_ = std::min(env_u32("GTA6RT_METAL_COMPILE_BACKGROUND", threads_n_ > 1 ? threads_n_ - 1 : 1), threads_n_);
    large_limit_ = std::min(env_u32("GTA6RT_METAL_COMPILE_LARGE", threads_n_ > 1 ? threads_n_ - 1 : 1), threads_n_);
    large_bytes_ = std::size_t(env_u32("GTA6RT_METAL_COMPILE_LARGE_KB", 512)) * 1024;
    qos_[0] = qos_named("GTA6RT_METAL_COMPILE_QOS_DEMAND", QOS_CLASS_USER_INITIATED);
    qos_[1] = qos_named("GTA6RT_METAL_COMPILE_QOS_DEMAND", QOS_CLASS_USER_INITIATED);
    qos_[2] = qos_named("GTA6RT_METAL_COMPILE_QOS_BACKGROUND", QOS_CLASS_UTILITY);
}

void CompileQueue::set_limits(std::uint32_t active, std::uint32_t background) {
    {
        std::lock_guard lock(mutex_);
        if (active) active_limit_ = std::min(active, threads_n_);
        if (background) background_limit_ = std::min(background, threads_n_);
    }
    cv_.notify_all();
}

// A large library (cold AIR -> GPU code: 20-41 s for 1-2 MB, pso_probe) holds a worker for its whole build, so at
// most large_limit_ run at once and a smaller one of the same priority goes first meanwhile; demanded work before
// background work. *index is the queue position to take.
bool CompileQueue::runnable_locked(std::size_t* priority, std::size_t* index) const {
    if (running_.size() >= active_limit_) return false;
    for (std::size_t pr = 0; pr < queues_.size(); ++pr) {
        if (queues_[pr].empty()) continue;
        if (pr == std::size_t(Priority::background) && running_background_ >= background_limit_) return false;
        const auto& q = queues_[pr];
        for (std::size_t i = 0; i < q.size(); ++i) {
            if (running_large_ >= large_limit_ && q[i]->cost_bytes >= large_bytes_) continue;
            *priority = pr;
            *index = i;
            return true;
        }
    }
    return false;
}

CompileQueue::~CompileQueue() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void CompileQueue::publish_pending_locked() const {
    gta6rt_hud_set_pipelines_pending(queues_[0].size() + queues_[1].size() + queues_[2].size() + running_.size());
}

void CompileQueue::erase_locked(PipelineCore* p) {
    for (auto& q : queues_) {
        auto it = std::find(q.begin(), q.end(), p);
        if (it != q.end()) {
            q.erase(it);
            break;
        }
    }
    p->queued = false;
    publish_pending_locked();
}

void CompileQueue::enqueue(PipelineCore* p, Priority priority) {
    {
        std::lock_guard lock(mutex_);
        if (p->claimed_ref()) return;
        const auto pr = std::size_t(priority);
        if (p->queued.load()) {
            for (std::size_t i = 0; i < queues_.size(); ++i) {
                auto it = std::find(queues_[i].begin(), queues_[i].end(), p);
                if (it == queues_[i].end()) continue;
                if (i > pr) {
                    queues_[i].erase(it);
                    queues_[pr].push_back(p);
                }
                break;
            }
        } else {
            queues_[pr].push_back(p);
            p->queued = true;
        }
        publish_pending_locked();
        while (threads_.size() < threads_n_) threads_.emplace_back([this] { run(); });
    }
    cv_.notify_one();
}

bool CompileQueue::steal(PipelineCore* p) {
    std::lock_guard lock(mutex_);
    if (!p->queued.load()) return false;
    erase_locked(p);
    ++stolen_;
    return true;
}

void CompileQueue::forget(PipelineCore* p) {
    std::unique_lock lock(mutex_);
    if (p->queued.load()) erase_locked(p);
    p->claim();
    done_cv_.wait(lock, [&] { return running_.count(p) == 0; });
}

void CompileQueue::run() {
    pthread_setname_np("gta6rt pipeline compile");
    std::unique_lock lock(mutex_);
    int qos_now = -1;
    while (true) {
        std::size_t pr = 0, index = 0;
        cv_.wait(lock, [&] { return stop_ || runnable_locked(&pr, &index); });
        if (stop_) return;
        if (index) ++large_deferred_;
        PipelineCore* p = queues_[pr][index];
        queues_[pr].erase(queues_[pr].begin() + std::ptrdiff_t(index));
        p->queued = false;
        if (!p->claim()) continue;
        running_.insert(p);
        const bool bg = pr == std::size_t(Priority::background);
        const bool large = p->cost_bytes >= large_bytes_;
        if (bg) ++running_background_;
        if (large) ++running_large_;
        publish_pending_locked();
        lock.unlock();
        if (qos_[pr] != qos_now) {
            qos_now = qos_[pr];
            pthread_set_qos_class_self_np(qos_class_t(qos_now), 0);
        }
        p->run_build();
        lock.lock();
        running_.erase(p);
        if (bg) --running_background_;
        if (large) --running_large_;
        publish_pending_locked();
        completed_[pr]++;
        done_cv_.notify_all();
        cv_.notify_all();
    }
}

CompileQueueStats CompileQueue::stats() const {
    std::lock_guard lock(mutex_);
    CompileQueueStats s;
    s.threads = std::uint32_t(threads_.size());
    for (std::size_t i = 0; i < 3; ++i) {
        s.queued[i] = queues_[i].size();
        s.completed[i] = completed_[i];
    }
    s.running = running_.size();
    s.stolen = stolen_;
    s.running_background = running_background_;
    s.active_limit = active_limit_;
    s.background_limit = background_limit_;
    s.running_large = running_large_;
    s.large_limit = large_limit_;
    s.large_passed = large_deferred_;
    return s;
}

// ---- render pipelines ----

static std::size_t library_bytes(std::initializer_list<ShaderLibrary*> libs) {
    std::size_t n = 0;
    for (ShaderLibrary* l : libs)
        if (l) n += static_cast<MetalLibrary*>(l)->bytes();
    return n;
}

MetalRenderPipeline::MetalRenderPipeline(MetalDevice* device, const RenderPipelineDesc& desc)
    : PipelineCore(device, desc.mode, desc.not_ready, desc.priority), desc_(desc) {
    if (desc.label) label_text = desc.label;
    desc_.label = nullptr;
    cost_bytes = library_bytes({desc.vs.library, desc.ps.library});
}

MetalRenderPipeline::~MetalRenderPipeline() { detach_from_queue(); }

void MetalRenderPipeline::prepare() {
    build_rect();
    build_d24();
}

void MetalRenderPipeline::build_d24() {
    NativeFormat nf;
    if (desc_.depth_format == format::unknown || !native_format(desc_.depth_format, nf) || !nf.info.emulated) return;
    if (desc_.d24 != D24Mode::fragment) return;
    auto* ps_lib = static_cast<MetalLibrary*>(desc_.ps.library);
    if (ps_lib && ps_lib->source().empty()) {
        log("D24 fragment depth: pixel library has no MSL source (metallib); using the rasterizer bias");
        return;
    }
    d24_wrapper_ = msl::d24_fragment_wrapper(ps_lib ? ps_lib->source() : std::string(), desc_.ps.entry);
    if (!d24_wrapper_.ok) {
        log("D24 fragment depth refused (%s); using the rasterizer bias", d24_wrapper_.error.c_str());
        return;
    }
    d24_mode_ = D24Mode::fragment;
    device_->counters().d24_fragment_pipelines++;
}

MTLRenderPipelineDescriptor* MetalRenderPipeline::make_descriptor(bool for_rect, std::string& err) {
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    if (for_rect) {
        std::string werr;
        double secs = 0;
        id<MTLLibrary> lib = device_->compile_msl(rect_wrapper_.source, 3, 0, false, &werr, &secs);
        if (!lib) {
            err = "rect wrapper compile: " + werr;
            return nil;
        }
        pd.vertexFunction = [lib newFunctionWithName:@(rect_wrapper_.entry.c_str())];
    } else {
        pd.vertexFunction = function_of(desc_.vs.library, desc_.vs.entry, err);
    }
    if (!pd.vertexFunction) {
        if (err.empty()) err = "vertex function '" + rect_wrapper_.entry + "' not found";
        return nil;
    }
    if (d24_mode_ == D24Mode::fragment) {
        std::string werr;
        double secs = 0;
        id<MTLLibrary> lib = device_->compile_msl(d24_wrapper_.source, 3, 0, false, &werr, &secs);
        if (!lib) {
            err = "D24 fragment wrapper compile: " + werr;
            return nil;
        }
        pd.fragmentFunction = [lib newFunctionWithName:@(d24_wrapper_.entry.c_str())];
    } else if (desc_.ps.library) {
        pd.fragmentFunction = function_of(desc_.ps.library, desc_.ps.entry, err);
        if (!pd.fragmentFunction) return nil;
    }
    if (!for_rect && !desc_.inputs.empty()) {
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        std::set<std::uint32_t> slots;
        for (const InputElement& e : desc_.inputs) {
            MTLVertexFormat vf = vertex_format(e.format);
            if (vf == MTLVertexFormatInvalid) {
                err = "input location " + std::to_string(e.location) + ": DXGI format " + std::to_string(e.format) +
                      " has no vertex format";
                return nil;
            }
            if (e.slot > 7) {
                err = "input slot above 7";
                return nil;
            }
            vd.attributes[e.location].format = vf;
            vd.attributes[e.location].offset = e.offset;
            vd.attributes[e.location].bufferIndex = vertex_buffer_index(e.slot);
            if (slots.insert(e.slot).second) {
                MTLVertexBufferLayoutDescriptor* l = vd.layouts[vertex_buffer_index(e.slot)];
                l.stride = MTLBufferLayoutStrideDynamic;
                l.stepFunction = e.per_instance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
                l.stepRate = e.per_instance ? std::max(1u, e.step_rate) : 1;
            }
        }
        pd.vertexDescriptor = vd;
    }
    if (!set_color_targets(pd.colorAttachments, desc_.rt_formats, desc_.rt_count, desc_.blend, err)) return nil;
    if (desc_.depth_format != format::unknown) {
        NativeFormat nf;
        if (!native_format(desc_.depth_format, nf) || !nf.info.depth) {
            err = "depth format " + std::to_string(desc_.depth_format);
            return nil;
        }
        pd.depthAttachmentPixelFormat = nf.pixel;
        if (nf.info.stencil) pd.stencilAttachmentPixelFormat = nf.pixel;
    }
    pd.rasterSampleCount = std::max(1u, desc_.samples);
    switch (desc_.topology) {
    case TopologyType::point: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
    case TopologyType::line: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
    default: pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
    }
    return pd;
}

void MetalRenderPipeline::build_rect() {
    if (desc_.topology != TopologyType::rectangle || desc_.rect == RectMode::none) return;
    rect_mode_ = desc_.rect;
    if (rect_mode_ != RectMode::vertex_expand) return;
    auto* vs_lib = static_cast<MetalLibrary*>(desc_.vs.library);
    if (!vs_lib || vs_lib->source().empty()) {
        rect_wrapper_.error = "vertex library has no MSL source";
        rect_mode_ = RectMode::cpu_rewrite;
        return;
    }
    std::vector<rect::WrapperInput> inputs;
    for (const InputElement& e : desc_.inputs) {
        if (e.per_instance) {
            rect_wrapper_.error = "input location " + std::to_string(e.location) +
                                  " is per-instance; the rectangle wrapper fetches per vertex only";
            log("rect wrapper refused (%s), falling back to cpu_rewrite", rect_wrapper_.error.c_str());
            rect_mode_ = RectMode::cpu_rewrite;
            return;
        }
        inputs.push_back({e.location, e.format, e.slot, e.offset});
    }
    rect_wrapper_ = rect::generate_rect_wrapper(vs_lib->source(), desc_.vs.entry, inputs);
    if (!rect_wrapper_.ok) {
        log("rect wrapper refused (%s), falling back to cpu_rewrite", rect_wrapper_.error.c_str());
        rect_mode_ = RectMode::cpu_rewrite;
    }
}

void MetalRenderPipeline::build() {
    const double t0 = now_seconds();
    std::string err;
    MTLRenderPipelineDescriptor* pd = make_descriptor(false, err);
    if (!pd) {
        finish_fail(Status::pipeline_failed, err, now_seconds() - t0);
        return;
    }
    MTLRenderPipelineReflection* refl = nil;
    NSError* error = nil;
    bool hit = false;
    id<MTLRenderPipelineState> pso = device_->build_render(pd, &refl, &error, &hit);
    if (!pso) {
        finish_fail(Status::pipeline_failed, ns_error(error, "pipeline creation failed"), now_seconds() - t0);
        return;
    }
    from_archive_ = hit;
    build_plan(refl);
    if (rect_mode_ == RectMode::vertex_expand) {
        MTLRenderPipelineDescriptor* rd = make_descriptor(true, err);
        NSError* rerr = nil;
        id<MTLRenderPipelineState> rp = rd ? [device_->mtl() newRenderPipelineStateWithDescriptor:rd error:&rerr] : nil;
        if (!rp) {
            log("rect wrapper pipeline: %s; falling back to cpu_rewrite", rerr ? ns_error(rerr, "").c_str() : err.c_str());
            rect_mode_ = RectMode::cpu_rewrite;
        }
        rect_pso_ = rp;
        if (rp) rect_dummy_ = [device_->mtl() newBufferWithLength:16 options:MTLResourceStorageModeShared];
    }
    depth_state_ = make_depth_state(device_->mtl(), desc_.depth_stencil);
    pso_ = pso;
    finish_ok(now_seconds() - t0);
}

void MetalRenderPipeline::build_plan(MTLRenderPipelineReflection* refl) {
    reflection_ = {};
    reflection_.vertex = convert_bindings(refl.vertexBindings);
    reflection_.fragment = convert_bindings(refl.fragmentBindings);
    std::string err;
    id<MTLFunction> vf = function_of(desc_.vs.library, desc_.vs.entry, err);
    for (MTLVertexAttribute* a in vf.vertexAttributes) {
        ReflectedAttribute ra;
        ra.name = a.name.UTF8String;
        ra.location = std::uint32_t(a.attributeIndex);
        ra.type = data_type_name(a.attributeType);
        ra.active = a.active;
        reflection_.attributes.push_back(ra);
        if (!a.active) continue;
        bool found = false;
        for (const InputElement& e : desc_.inputs) found |= e.location == a.attributeIndex;
        if (!found)
            reflection_.mismatches.push_back("vertex attribute " + ra.name + " location " + std::to_string(ra.location) +
                                             " has no input element");
    }
    PlanContext c;
    c.root = desc_.root ? &desc_.root->desc() : nullptr;
    c.inputs = &desc_.inputs;
    c.rect_params_index = rect_mode_ == RectMode::vertex_expand ? int(rect_wrapper_.params_index) : -1;
    c.d24_params_index = d24_params_index();
    c.refl = &reflection_;
    plan_stage(reflection_.vertex, 0, c, vertex_plan_);
    plan_stage(reflection_.fragment, 1, c, fragment_plan_);
    for (const InputElement& in : desc_.inputs) {
        const std::uint32_t idx = vertex_buffer_index(in.slot);
        for (const ReflectedBinding& b : reflection_.vertex) {
            if (b.index == idx && b.name.rfind("vertexBuffer", 0) != 0)
                reflection_.mismatches.push_back("vertex buffer slot " + std::to_string(in.slot) + " index " +
                                                 std::to_string(idx) + " collides with " + b.name);
        }
    }
}

// ---- compute pipelines ----

MetalComputePipeline::MetalComputePipeline(MetalDevice* device, const ComputePipelineDesc& desc)
    : PipelineCore(device, desc.mode, desc.not_ready, desc.priority), desc_(desc) {
    if (desc.label) label_text = desc.label;
    desc_.label = nullptr;
    cost_bytes = library_bytes({desc.cs.library});
    threads_ = desc.threads;
    if (!threads_[0] || !threads_[1] || !threads_[2]) {
        auto* lib = static_cast<MetalLibrary*>(desc.cs.library);
        auto wg = lib ? msl::workgroup_size(lib->source()) : std::nullopt;
        if (wg) {
            threads_ = *wg;
        } else {
            threads_ = {1, 1, 1};
            finish_fail(Status::invalid_argument,
                        "threadgroup size unknown: no gl_WorkGroupSize in the MSL; set ComputePipelineDesc::threads", 0);
        }
    }
}

MetalComputePipeline::~MetalComputePipeline() { detach_from_queue(); }

void MetalComputePipeline::build() {
    const double t0 = now_seconds();
    std::string err;
    id<MTLFunction> fn = function_of(desc_.cs.library, desc_.cs.entry, err);
    if (!fn) {
        finish_fail(Status::pipeline_failed, err, now_seconds() - t0);
        return;
    }
    const std::uint64_t total = std::uint64_t(threads_[0]) * threads_[1] * threads_[2];
    if (total > device_->caps().max_threads_per_threadgroup) {
        limit_ = {PipelineLimit::Kind::threads_per_group, total, device_->caps().max_threads_per_threadgroup, true,
                  "threadgroup of " + std::to_string(total) + " threads"};
        finish_fail(Status::limit_exceeded, limit_.reason, now_seconds() - t0);
        return;
    }
    MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
    pd.computeFunction = fn;
    pd.maxTotalThreadsPerThreadgroup = NSUInteger(total);
    MTLComputePipelineReflection* refl = nil;
    NSError* error = nil;
    bool hit = false;
    id<MTLComputePipelineState> pso = device_->build_compute(pd, &refl, &error, &hit);
    if (!pso) {
        const std::string text = ns_error(error, "compute pipeline creation failed");
        if (parse_limit(text, limit_)) {
            finish_fail(Status::limit_exceeded, text, now_seconds() - t0);
        } else {
            finish_fail(Status::pipeline_failed, text, now_seconds() - t0);
        }
        return;
    }
    from_archive_ = hit;
    reflection_ = {};
    reflection_.kernel = convert_bindings(refl.bindings);
    PlanContext c;
    c.root = desc_.root ? &desc_.root->desc() : nullptr;
    c.refl = &reflection_;
    plan_stage(reflection_.kernel, 2, c, plan_);
    pso_ = pso;
    finish_ok(now_seconds() - t0);
}

// ---- mesh pipelines ----

MetalMeshPipeline::MetalMeshPipeline(MetalDevice* device, const MeshPipelineDesc& desc)
    : PipelineCore(device, desc.mode, desc.not_ready, desc.priority), desc_(desc) {
    if (desc.label) label_text = desc.label;
    desc_.label = nullptr;
    cost_bytes = library_bytes({desc.mesh.library, desc.object.library, desc.ps.library});
}

MetalMeshPipeline::~MetalMeshPipeline() { detach_from_queue(); }

void MetalMeshPipeline::prepare() {
    auto threads_of = [](ShaderLibrary* l, std::array<std::uint32_t, 3> given) {
        if (given[0] && given[1] && given[2]) return given;
        auto* lib = static_cast<MetalLibrary*>(l);
        auto wg = lib ? msl::workgroup_size(lib->source()) : std::nullopt;
        return wg ? *wg : std::array<std::uint32_t, 3>{1, 1, 1};
    };
    mesh_threads_ = threads_of(desc_.mesh.library, desc_.mesh_threads);
    if (desc_.object.library) object_threads_ = threads_of(desc_.object.library, desc_.object_threads);
    auto* ml = static_cast<MetalLibrary*>(desc_.mesh.library);
    if (!ml) {
        finish_fail(Status::invalid_argument, "no mesh library", 0);
        return;
    }
    if (ml->source().empty()) return;
    estimate_ = msl::estimate_mesh(ml->source(), desc_.mesh.entry);
    if (!desc_.precheck || !estimate_.parsed) return;
    const DeviceCaps& caps = device_->caps();
    std::string vstruct;
    {
        const std::regex decl("mesh<\\s*(\\w+)\\s*,");
        std::smatch m;
        if (std::regex_search(ml->source(), m, decl)) vstruct = m[1].str();
    }
    const std::uint32_t unique = msl::mesh_unique_scalars(ml->source(), vstruct);
    if (estimate_.mesh_bytes > caps.max_mesh_bytes) {
        limit_ = {PipelineLimit::Kind::mesh_size, estimate_.mesh_bytes, caps.max_mesh_bytes, true,
                  "mesh size " + std::to_string(estimate_.mesh_bytes) + " B (" + std::to_string(estimate_.max_vertices) +
                      " vertices x 16 B x ceil(" + std::to_string(estimate_.vertex_scalars) + " scalars / 4)) > " +
                      std::to_string(caps.max_mesh_bytes) + " B" +
                      (unique > caps.max_mesh_scalars ? "; unique scalars " + std::to_string(unique) + " > " +
                                                            std::to_string(caps.max_mesh_scalars)
                                                      : std::string())};
    } else if (unique > caps.max_mesh_scalars) {
        limit_ = {PipelineLimit::Kind::mesh_scalars, unique, caps.max_mesh_scalars, true,
                  "unique scalars " + std::to_string(unique) + " > " + std::to_string(caps.max_mesh_scalars)};
    }
    if (limit_.kind != PipelineLimit::Kind::none) finish_fail(Status::limit_exceeded, limit_.reason, 0);
}

void MetalMeshPipeline::build() {
    const double t0 = now_seconds();
    std::string err;
    MTLMeshRenderPipelineDescriptor* pd = [MTLMeshRenderPipelineDescriptor new];
    pd.meshFunction = function_of(desc_.mesh.library, desc_.mesh.entry, err);
    if (!pd.meshFunction) {
        finish_fail(Status::pipeline_failed, "mesh " + err, now_seconds() - t0);
        return;
    }
    if (desc_.object.library) {
        pd.objectFunction = function_of(desc_.object.library, desc_.object.entry, err);
        if (!pd.objectFunction) {
            finish_fail(Status::pipeline_failed, "object " + err, now_seconds() - t0);
            return;
        }
        pd.payloadMemoryLength = desc_.payload_bytes;
        pd.maxTotalThreadsPerObjectThreadgroup = object_threads_[0] * object_threads_[1] * object_threads_[2];
    }
    pd.maxTotalThreadsPerMeshThreadgroup = mesh_threads_[0] * mesh_threads_[1] * mesh_threads_[2];
    if (desc_.ps.library) {
        pd.fragmentFunction = function_of(desc_.ps.library, desc_.ps.entry, err);
        if (!pd.fragmentFunction) {
            finish_fail(Status::pipeline_failed, "fragment " + err, now_seconds() - t0);
            return;
        }
    } else {
        pd.rasterizationEnabled = NO;
    }
    if (!set_color_targets(pd.colorAttachments, desc_.rt_formats, desc_.rt_count, desc_.blend, err)) {
        finish_fail(Status::pipeline_failed, err, now_seconds() - t0);
        return;
    }
    if (desc_.depth_format != format::unknown) {
        NativeFormat nf;
        if (!native_format(desc_.depth_format, nf) || !nf.info.depth) {
            finish_fail(Status::pipeline_failed, "depth format " + std::to_string(desc_.depth_format), now_seconds() - t0);
            return;
        }
        pd.depthAttachmentPixelFormat = nf.pixel;
        if (nf.info.stencil) pd.stencilAttachmentPixelFormat = nf.pixel;
    }
    pd.rasterSampleCount = std::max(1u, desc_.samples);
    MTLRenderPipelineReflection* refl = nil;
    NSError* error = nil;
    bool hit = false;
    id<MTLRenderPipelineState> pso = device_->build_mesh(pd, &refl, &error, &hit);
    if (!pso) {
        const std::string text = ns_error(error, "mesh pipeline creation failed");
        if (parse_limit(text, limit_)) {
            if (text.find("unique scalar count") != std::string::npos && limit_.kind == PipelineLimit::Kind::mesh_size)
                limit_.reason = text;
            finish_fail(Status::limit_exceeded, text, now_seconds() - t0);
        } else {
            finish_fail(Status::pipeline_failed, text, now_seconds() - t0);
        }
        return;
    }
    from_archive_ = hit;
    reflection_ = {};
    reflection_.object = convert_bindings(refl.objectBindings);
    reflection_.mesh = convert_bindings(refl.meshBindings);
    reflection_.fragment = convert_bindings(refl.fragmentBindings);
    PlanContext c;
    c.root = desc_.root ? &desc_.root->desc() : nullptr;
    c.refl = &reflection_;
    plan_stage(reflection_.object, 3, c, object_plan_);
    plan_stage(reflection_.mesh, 4, c, mesh_plan_);
    plan_stage(reflection_.fragment, 1, c, fragment_plan_);
    depth_state_ = make_depth_state(device_->mtl(), desc_.depth_stencil);
    pso_ = pso;
    finish_ok(now_seconds() - t0);
}

// ---- binary archive ----

namespace {

std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

constexpr MTLPipelineOption kReflect = MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;
constexpr const char* kShardSuffix = ".metalarchive";

std::string read_text(const std::string& path) {
    std::ifstream k(path);
    std::stringstream s;
    s << k.rdbuf();
    return s.str();
}

std::uint64_t size_of(const std::string& path) {
    struct stat sb {};
    return ::stat(path.c_str(), &sb) == 0 ? std::uint64_t(sb.st_size) : 0;
}

bool exists(const std::string& path) {
    struct stat sb {};
    return ::stat(path.c_str(), &sb) == 0;
}

bool write_text_atomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + "." + std::to_string(::getpid()) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        o << text;
        o.flush();
        if (!o) {
            ::unlink(tmp.c_str());
            return false;
        }
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// "gta6rt-<hash>-<tag>-<n>.metalarchive" -> n; the single file of earlier versions ("gta6rt-<hash>.metalarchive") -> 0.
std::uint32_t shard_seq(const std::string& name, std::size_t base_len) {
    const std::size_t end = name.size() - std::strlen(kShardSuffix);
    if (end <= base_len) return 0;
    const std::size_t dash = name.rfind('-', end);
    if (dash == std::string::npos || dash < base_len) return 0;
    return std::uint32_t(std::strtoul(name.substr(dash + 1, end - dash - 1).c_str(), nullptr, 10));
}

std::string clean_tag(const std::string& tag) {
    std::string out;
    for (char c : tag)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
    return out.empty() ? "run" : out;
}

// Metal's archive reader crashes on some damaged files (SIGSEGV in deserializeBinaryArchiveHeader on a truncated one,
// measured), so a shard is handed to Metal only when its size and content hash match what its .key file recorded.
std::string content_line(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::uint64_t h = 0x9e3779b97f4a7c15ull, n = 0;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        const std::size_t got = std::size_t(in.gcount());
        std::size_t i = 0;
        for (; i + 8 <= got; i += 8) {
            std::uint64_t w;
            std::memcpy(&w, buf.data() + i, 8);
            h = (h ^ w) * 0x100000001b3ull;
            h ^= h >> 29;
        }
        for (; i < got; ++i) h = (h ^ static_cast<unsigned char>(buf[i])) * 0x100000001b3ull;
        n += got;
    }
    char line[64];
    std::snprintf(line, sizeof line, "\ncontent %llu %016llx", (unsigned long long)n, (unsigned long long)h);
    return line;
}

id<MTLBinaryArchive> open_file(id<MTLDevice> device, const std::string& path, NSError** error) {
    MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
    ad.url = [NSURL fileURLWithPath:@(path.c_str())];
    return [device newBinaryArchiveWithDescriptor:ad error:error];
}

}  // namespace

NSArray* MetalDevice::archive_lookup_locked() const {
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:archive_shards_.size() + 1];
    for (id<MTLBinaryArchive> s : archive_shards_) [a addObject:s];
    if (archive_) [a addObject:archive_];
    return a;
}

std::string MetalDevice::next_shard_path_locked() {
    char seq[16];
    std::snprintf(seq, sizeof seq, "%04u", ++archive_seq_);
    return archive_prefix_ + "-" + archive_tag_ + "-" + seq + kShardSuffix;
}

void MetalDevice::set_archive_tag(const std::string& tag) {
    std::lock_guard lock(archive_mutex_);
    archive_tag_ = clean_tag(tag);
}

// Lookup in every shard (a hit costs no compile), else compile and add to the writable shard. The add holds only
// archive_add_mutex_, which a flush takes just long enough to swap in a new writable shard.
template <class P, class Make, class Add>
P MetalDevice::archive_build(id descriptor, bool* hit, NSError** error, Make&& make, Add&& add) {
    NSArray* lookup = nil;
    {
        std::lock_guard lock(archive_mutex_);
        if (archive_) lookup = archive_lookup_locked();
    }
    *hit = false;
    if (lookup) {
        [descriptor setBinaryArchives:lookup];
        NSError* miss = nil;
        const double t0 = now_seconds();
        P p = make(kReflect | MTLPipelineOptionFailOnBinaryArchiveMiss, &miss);
        const double dt = now_seconds() - t0;
        std::lock_guard lock(archive_mutex_);
        archive_stats_.lookup_seconds += dt;
        if (p) {
            *hit = true;
            stats_.archive_hits++;
            archive_stats_.hits++;
            return p;
        }
        stats_.archive_misses++;
        archive_stats_.misses++;
    }
    [descriptor setBinaryArchives:nil];
    const double t1 = now_seconds();
    P p = make(kReflect, error);
    const double compile_dt = now_seconds() - t1;
    if (!lookup) return p;
    {
        std::lock_guard lock(archive_mutex_);
        archive_stats_.compile_seconds += compile_dt;
    }
    if (!p) return p;
    std::lock_guard add_lock(archive_add_mutex_);
    id<MTLBinaryArchive> writable = nil;
    {
        std::lock_guard lock(archive_mutex_);
        writable = archive_;
    }
    if (!writable) return p;
    NSError* ae = nil;
    const double t2 = now_seconds();
    const BOOL added = add(writable, &ae);
    const double add_dt = now_seconds() - t2;
    std::lock_guard lock(archive_mutex_);
    archive_stats_.add_seconds += add_dt;
    if (added) {
        archive_stats_.added++;
        archive_stats_.pending++;
    } else {
        archive_stats_.add_failures++;
        archive_stats_.last_error = ns_error(ae, "add to archive failed");
    }
    return p;
}

id<MTLRenderPipelineState> MetalDevice::build_render(MTLRenderPipelineDescriptor* pd, MTLRenderPipelineReflection** refl,
                                                     NSError** error, bool* hit) {
    return archive_build<id<MTLRenderPipelineState>>(
        pd, hit, error,
        [&](MTLPipelineOption o, NSError** e) {
            return [device_ newRenderPipelineStateWithDescriptor:pd options:o reflection:refl error:e];
        },
        [&](id<MTLBinaryArchive> a, NSError** e) { return [a addRenderPipelineFunctionsWithDescriptor:pd error:e]; });
}

id<MTLRenderPipelineState> MetalDevice::build_mesh(MTLMeshRenderPipelineDescriptor* pd, MTLRenderPipelineReflection** refl,
                                                   NSError** error, bool* hit) {
    return archive_build<id<MTLRenderPipelineState>>(
        pd, hit, error,
        [&](MTLPipelineOption o, NSError** e) {
            return [device_ newRenderPipelineStateWithMeshDescriptor:pd options:o reflection:refl error:e];
        },
        [&](id<MTLBinaryArchive> a, NSError** e) { return [a addMeshRenderPipelineFunctionsWithDescriptor:pd error:e]; });
}

id<MTLComputePipelineState> MetalDevice::build_compute(MTLComputePipelineDescriptor* pd, MTLComputePipelineReflection** refl,
                                                       NSError** error, bool* hit) {
    return archive_build<id<MTLComputePipelineState>>(
        pd, hit, error,
        [&](MTLPipelineOption o, NSError** e) {
            return [device_ newComputePipelineStateWithDescriptor:pd options:o reflection:refl error:e];
        },
        [&](id<MTLBinaryArchive> a, NSError** e) { return [a addComputePipelineFunctionsWithDescriptor:pd error:e]; });
}

Status MetalDevice::open_pipeline_cache(const PipelineCacheDesc& desc, std::string* error) {
    if (!desc.directory || !*desc.directory) {
        if (error) *error = "no directory";
        return Status::invalid_argument;
    }
    NSString* ns_dir = @(desc.directory);
    NSError* e = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:ns_dir withIntermediateDirectories:YES attributes:nil error:&e]) {
        if (error) *error = ns_error(e, "cannot create the cache directory");
        return Status::invalid_argument;
    }
    const std::string key = "gta6rt_metal binary archive v1; translator " + desc.translator_version + "; gpu " +
                            caps_.name + "; os " + std::string(NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String) +
                            "; process " + caps_.process_arch;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)fnv1a(key));
    const std::string dir = desc.directory;
    const std::string base = std::string("gta6rt-") + hex;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = de.path().filename().string();
        const std::size_t sl = std::strlen(kShardSuffix);
        if (name.rfind(base, 0) != 0 || name.size() <= base.size() + sl) continue;
        if (name.compare(name.size() - sl, sl, kShardSuffix) != 0) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::vector<id<MTLBinaryArchive>> shards;
    std::vector<std::string> files;
    std::uint64_t bytes = 0;
    std::uint32_t unreadable = 0, seq = 0;
    const double t0 = now_seconds();
    for (const std::string& name : names) {
        seq = std::max(seq, shard_seq(name, base.size()));
        if (!desc.load) continue;
        const std::string path = dir + "/" + name;
        const std::string recorded = read_text(path + ".key");
        if (recorded.rfind(key, 0) != 0 || recorded.find('\n') != key.size()) continue;
        NSError* le = nil;
        const bool intact = recorded.substr(key.size()) == content_line(path);
        id<MTLBinaryArchive> a = intact ? open_file(device_, path, &le) : nil;
        if (!intact) le = [NSError errorWithDomain:@"gta6rt" code:1 userInfo:@{NSLocalizedDescriptionKey : @"size or content hash differs from its .key file (or none recorded)"}];
        if (!a) {
            const std::string aside = path + ".unreadable";
            std::rename(path.c_str(), aside.c_str());
            log("pipeline archive shard %s does not load (%s): moved to %s", path.c_str(),
                ns_error(le, "unknown").c_str(), aside.c_str());
            ++unreadable;
            continue;
        }
        shards.push_back(a);
        files.push_back(path);
        bytes += size_of(path);
    }
    const double t1 = now_seconds();
    id<MTLBinaryArchive> writable = [device_ newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
    if (!writable) {
        if (error) *error = ns_error(e, "binary archive creation failed");
        return Status::unsupported;
    }
    std::lock_guard add_lock(archive_add_mutex_);
    std::lock_guard lock(archive_mutex_);
    archive_ = writable;
    archive_shards_ = std::move(shards);
    archive_files_ = std::move(files);
    archive_dir_ = dir;
    archive_prefix_ = dir + "/" + base;
    archive_seq_ = seq;
    archive_shard_cap_ = desc.shard_bytes ? desc.shard_bytes : 150'000'000;
    archive_tag_ = clean_tag(desc.tag);
    archive_stats_ = {};
    archive_stats_.open = true;
    archive_stats_.loaded = !archive_shards_.empty();
    archive_stats_.path = archive_prefix_;
    archive_stats_.key = key;
    archive_stats_.load_seconds = t1 - t0;
    archive_stats_.file_bytes = bytes;
    archive_stats_.shards = std::uint32_t(archive_shards_.size());
    archive_stats_.shards_unreadable = unreadable;
    return Status::ok;
}

Status MetalDevice::flush_pipeline_cache(std::string* error) {
    std::lock_guard flush_lock(archive_flush_mutex_);
    id<MTLBinaryArchive> sealed = nil;
    std::uint64_t n = 0;
    std::string path, key;
    {
        std::lock_guard add_lock(archive_add_mutex_);
        std::lock_guard lock(archive_mutex_);
        if (!archive_) return Status::not_ready;
        if (!archive_stats_.pending) return Status::ok;
        NSError* e = nil;
        id<MTLBinaryArchive> fresh = [device_ newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
        if (!fresh) {
            if (error) *error = ns_error(e, "binary archive creation failed");
            return Status::unsupported;
        }
        sealed = archive_;
        n = archive_stats_.pending;
        archive_shards_.push_back(sealed);
        archive_ = fresh;
        archive_stats_.pending = 0;
        path = next_shard_path_locked();
        key = archive_stats_.key;
    }
    const double t0 = now_seconds();
    const std::string tmp = path + "." + std::to_string(::getpid()) + ".tmp";
    NSError* e = nil;
    std::string why;
    bool ok = [sealed serializeToURL:[NSURL fileURLWithPath:@(tmp.c_str())] error:&e];
    if (!ok) why = ns_error(e, "serialize failed");
    if (ok) {
        NSError* ve = nil;
        if (!open_file(device_, tmp, &ve)) {
            ok = false;
            why = ns_error(ve, "the written shard does not load");
        }
    }
    std::string final_path;
    const std::string content = ok ? content_line(tmp) : std::string();
    if (ok) {
        for (int attempt = 0; attempt < 64 && final_path.empty(); ++attempt) {
            if (!exists(path) && write_text_atomic(path + ".key", key + content) && ::link(tmp.c_str(), path.c_str()) == 0) {
                final_path = path;
                break;
            }
            std::lock_guard lock(archive_mutex_);
            path = next_shard_path_locked();
        }
        if (final_path.empty()) {
            ok = false;
            why = "no free shard name";
        }
    }
    ::unlink(tmp.c_str());
    id<MTLBinaryArchive> on_disk = nil;
    if (ok) {
        NSError* oe = nil;
        on_disk = open_file(device_, final_path, &oe);
    }
    const double dt = now_seconds() - t0;
    const std::uint64_t bytes = ok ? size_of(final_path) : 0;
    std::lock_guard lock(archive_mutex_);
    std::erase_if(archive_shards_, [&](id<MTLBinaryArchive> a) { return a == sealed; });
    if (!ok) {
        // After a failed serialize Metal's in-memory archive can be damaged (STATUS-cache.md): it is not looked up
        // again; its pipelines are compiled again at their next creation.
        archive_stats_.flush_failures++;
        archive_stats_.last_error = why;
        if (error) *error = why;
        return Status::unsupported;
    }
    archive_shards_.push_back(on_disk ? on_disk : sealed);
    archive_files_.push_back(final_path);
    archive_stats_.shards++;
    archive_stats_.written += n;
    archive_stats_.flushes++;
    archive_stats_.file_bytes += bytes;
    archive_stats_.serialize_seconds = dt;
    archive_stats_.serialize_longest = std::max(archive_stats_.serialize_longest, dt);
    if (n) archive_bytes_per_pipeline_ = std::max<std::uint64_t>(4096, (archive_bytes_per_pipeline_ + bytes / n) / 2);
    return Status::ok;
}

PipelineCacheStats MetalDevice::pipeline_cache_stats() const {
    std::lock_guard lock(archive_mutex_);
    PipelineCacheStats s = archive_stats_;
    s.pending_bytes = s.pending * archive_bytes_per_pipeline_;
    return s;
}

void MetalDevice::precompile(std::span<gpu::Pipeline* const> pipelines, Priority priority) {
    for (gpu::Pipeline* p : pipelines) {
        if (auto* core = dynamic_cast<PipelineCore*>(p)) {
            if (core->core_state() == PipelineState::ready || core->core_state() == PipelineState::failed) continue;
            compiler_.enqueue(core, priority);
        }
    }
}

MeshEstimate MetalDevice::estimate_mesh(gpu::ShaderLibrary* library, const std::string& entry) const {
    auto* lib = static_cast<MetalLibrary*>(library);
    if (!lib || lib->source().empty()) {
        MeshEstimate e;
        e.error = "no MSL source";
        return e;
    }
    return msl::estimate_mesh(lib->source(), entry);
}

std::unique_ptr<gpu::RenderPipeline> MetalDevice::create_render_pipeline(const RenderPipelineDesc& desc) {
    auto p = std::make_unique<MetalRenderPipeline>(this, desc);
    stats_.pipelines_created++;
    p->prepare();
    p->start();
    return p;
}

std::unique_ptr<gpu::ComputePipeline> MetalDevice::create_compute_pipeline(const ComputePipelineDesc& desc) {
    auto p = std::make_unique<MetalComputePipeline>(this, desc);
    stats_.pipelines_created++;
    p->start();
    return p;
}

std::unique_ptr<gpu::MeshPipeline> MetalDevice::create_mesh_pipeline(const MeshPipelineDesc& desc) {
    auto p = std::make_unique<MetalMeshPipeline>(this, desc);
    stats_.pipelines_created++;
    p->prepare();
    p->start();
    return p;
}

bool set_compile_limits(gpu::Device* device, std::uint32_t active, std::uint32_t background) {
    auto* d = dynamic_cast<MetalDevice*>(device);
    if (!d) return false;
    d->set_compile_limits(active, background);
    return true;
}

bool set_pipeline_cache_tag(gpu::Device* device, const std::string& tag) {
    auto* d = dynamic_cast<MetalDevice*>(device);
    if (!d) return false;
    d->set_archive_tag(tag);
    return true;
}

}  // namespace gta6rt::metal
