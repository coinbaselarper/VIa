#include "mtl_internal.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

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
    std::unique_lock lock(mutex_);
    while (true) {
        cv_.wait(lock, [&] {
            if (stop_) return true;
            for (const auto& q : queues_)
                if (!q.empty()) return true;
            return false;
        });
        if (stop_) return;
        std::size_t pr = 0;
        while (queues_[pr].empty()) ++pr;
        PipelineCore* p = queues_[pr].front();
        queues_[pr].pop_front();
        p->queued = false;
        if (!p->claim()) continue;
        running_.insert(p);
        publish_pending_locked();
        lock.unlock();
        p->run_build();
        lock.lock();
        running_.erase(p);
        publish_pending_locked();
        completed_[pr]++;
        done_cv_.notify_all();
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
    return s;
}

// ---- render pipelines ----

MetalRenderPipeline::MetalRenderPipeline(MetalDevice* device, const RenderPipelineDesc& desc)
    : PipelineCore(device, desc.mode, desc.not_ready, desc.priority), desc_(desc) {
    desc_.label = nullptr;
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
    desc_.label = nullptr;
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
    desc_.label = nullptr;
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

}  // namespace

id<MTLRenderPipelineState> MetalDevice::build_render(MTLRenderPipelineDescriptor* pd, MTLRenderPipelineReflection** refl,
                                                     NSError** error, bool* hit) {
    id<MTLBinaryArchive> archive = nil;
    {
        std::lock_guard lock(archive_mutex_);
        archive = archive_;
    }
    *hit = false;
    if (archive) {
        pd.binaryArchives = @[ archive ];
        NSError* miss = nil;
        id<MTLRenderPipelineState> p = [device_ newRenderPipelineStateWithDescriptor:pd
                                                                             options:kReflect | MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                          reflection:refl
                                                                               error:&miss];
        if (p) {
            *hit = true;
            stats_.archive_hits++;
            std::lock_guard lock(archive_mutex_);
            archive_stats_.hits++;
            return p;
        }
        stats_.archive_misses++;
        std::lock_guard lock(archive_mutex_);
        archive_stats_.misses++;
    }
    id<MTLRenderPipelineState> p = [device_ newRenderPipelineStateWithDescriptor:pd options:kReflect reflection:refl error:error];
    if (p && archive) {
        std::lock_guard lock(archive_mutex_);
        NSError* ae = nil;
        if ([archive addRenderPipelineFunctionsWithDescriptor:pd error:&ae]) {
            archive_stats_.added++;
            archive_dirty_ = true;
        } else {
            archive_stats_.add_failures++;
        }
    }
    return p;
}

id<MTLRenderPipelineState> MetalDevice::build_mesh(MTLMeshRenderPipelineDescriptor* pd, MTLRenderPipelineReflection** refl,
                                                   NSError** error, bool* hit) {
    id<MTLBinaryArchive> archive = nil;
    {
        std::lock_guard lock(archive_mutex_);
        archive = archive_;
    }
    *hit = false;
    if (archive) {
        pd.binaryArchives = @[ archive ];
        NSError* miss = nil;
        id<MTLRenderPipelineState> p = [device_ newRenderPipelineStateWithMeshDescriptor:pd
                                                                                 options:kReflect | MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                              reflection:refl
                                                                                   error:&miss];
        if (p) {
            *hit = true;
            stats_.archive_hits++;
            std::lock_guard lock(archive_mutex_);
            archive_stats_.hits++;
            return p;
        }
        stats_.archive_misses++;
        std::lock_guard lock(archive_mutex_);
        archive_stats_.misses++;
    }
    id<MTLRenderPipelineState> p = [device_ newRenderPipelineStateWithMeshDescriptor:pd options:kReflect reflection:refl error:error];
    if (p && archive) {
        std::lock_guard lock(archive_mutex_);
        NSError* ae = nil;
        if ([archive addMeshRenderPipelineFunctionsWithDescriptor:pd error:&ae]) {
            archive_stats_.added++;
            archive_dirty_ = true;
        } else {
            archive_stats_.add_failures++;
        }
    }
    return p;
}

id<MTLComputePipelineState> MetalDevice::build_compute(MTLComputePipelineDescriptor* pd, MTLComputePipelineReflection** refl,
                                                       NSError** error, bool* hit) {
    id<MTLBinaryArchive> archive = nil;
    {
        std::lock_guard lock(archive_mutex_);
        archive = archive_;
    }
    *hit = false;
    if (archive) {
        pd.binaryArchives = @[ archive ];
        NSError* miss = nil;
        id<MTLComputePipelineState> p = [device_ newComputePipelineStateWithDescriptor:pd
                                                                               options:kReflect | MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                            reflection:refl
                                                                                 error:&miss];
        if (p) {
            *hit = true;
            stats_.archive_hits++;
            std::lock_guard lock(archive_mutex_);
            archive_stats_.hits++;
            return p;
        }
        stats_.archive_misses++;
        std::lock_guard lock(archive_mutex_);
        archive_stats_.misses++;
    }
    id<MTLComputePipelineState> p = [device_ newComputePipelineStateWithDescriptor:pd options:kReflect reflection:refl error:error];
    if (p && archive) {
        std::lock_guard lock(archive_mutex_);
        NSError* ae = nil;
        if ([archive addComputePipelineFunctionsWithDescriptor:pd error:&ae]) {
            archive_stats_.added++;
            archive_dirty_ = true;
        } else {
            archive_stats_.add_failures++;
        }
    }
    return p;
}

Status MetalDevice::open_pipeline_cache(const PipelineCacheDesc& desc, std::string* error) {
    if (!desc.directory || !*desc.directory) {
        if (error) *error = "no directory";
        return Status::invalid_argument;
    }
    NSString* dir = @(desc.directory);
    NSError* e = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&e]) {
        if (error) *error = ns_error(e, "cannot create the cache directory");
        return Status::invalid_argument;
    }
    const std::string key = "gta6rt_metal binary archive v1; translator " + desc.translator_version + "; gpu " +
                            caps_.name + "; os " + std::string(NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String) +
                            "; process " + caps_.process_arch;
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)fnv1a(key));
    const std::string path = std::string(desc.directory) + "/gta6rt-" + hex + ".metalarchive";
    const std::string key_path = path + ".key";
    std::string stored;
    {
        std::ifstream k(key_path);
        std::stringstream s;
        s << k.rdbuf();
        stored = s.str();
    }
    MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
    const bool exists = [[NSFileManager defaultManager] fileExistsAtPath:@(path.c_str())];
    const bool load = desc.load && exists && stored == key;
    if (load) ad.url = [NSURL fileURLWithPath:@(path.c_str())];
    const double t0 = now_seconds();
    id<MTLBinaryArchive> archive = [device_ newBinaryArchiveWithDescriptor:ad error:&e];
    const double t1 = now_seconds();
    if (!archive) {
        if (error) *error = ns_error(e, "binary archive creation failed");
        return Status::unsupported;
    }
    std::lock_guard lock(archive_mutex_);
    archive_ = archive;
    archive_stats_ = {};
    archive_stats_.open = true;
    archive_stats_.loaded = load;
    archive_stats_.path = path;
    archive_stats_.key = key;
    archive_stats_.load_seconds = t1 - t0;
    if (load) {
        NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:@(path.c_str()) error:nil];
        archive_stats_.file_bytes = [attrs fileSize];
    }
    archive_dirty_ = false;
    return Status::ok;
}

Status MetalDevice::flush_pipeline_cache(std::string* error) {
    std::lock_guard lock(archive_mutex_);
    if (!archive_) return Status::not_ready;
    if (!archive_dirty_) return Status::ok;
    const std::string tmp = archive_stats_.path + ".tmp";
    NSError* e = nil;
    const double t0 = now_seconds();
    if (![archive_ serializeToURL:[NSURL fileURLWithPath:@(tmp.c_str())] error:&e]) {
        if (error) *error = ns_error(e, "serialize failed");
        return Status::unsupported;
    }
    std::rename(tmp.c_str(), archive_stats_.path.c_str());
    std::ofstream(archive_stats_.path + ".key") << archive_stats_.key;
    archive_stats_.serialize_seconds = now_seconds() - t0;
    NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:@(archive_stats_.path.c_str()) error:nil];
    archive_stats_.file_bytes = [attrs fileSize];
    archive_dirty_ = false;
    return Status::ok;
}

PipelineCacheStats MetalDevice::pipeline_cache_stats() const {
    std::lock_guard lock(archive_mutex_);
    return archive_stats_;
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

}  // namespace gta6rt::metal
