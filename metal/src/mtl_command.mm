#include "mtl_internal.h"

namespace gta6rt::metal {

void MetalCommandList::reset() {
    commands_.clear();
    state_ = std::make_shared<DrawState>();
    state_shared_ = false;
    closed_ = false;
    in_pass_ = false;
    error_.clear();
}

Status MetalCommandList::close() {
    if (closed_) return Status::invalid_argument;
    if (in_pass_) {
        error_ = "close inside a render pass";
        return Status::invalid_argument;
    }
    closed_ = true;
    return error_.empty() ? Status::ok : Status::invalid_argument;
}

void MetalCommandList::push(Cmd&& cmd) {
    if (closed_) {
        error_ = "command recorded after close";
        return;
    }
    commands_.push_back(std::move(cmd));
}

DrawState& MetalCommandList::edit_state() {
    if (state_shared_) {
        state_ = std::make_shared<DrawState>(*state_);
        state_shared_ = false;
    }
    return *state_;
}

void MetalCommandList::begin_render_pass(const RenderPassDesc& desc) {
    if (in_pass_) error_ = "nested render pass";
    in_pass_ = true;
    push(CmdBeginPass{desc});
}

void MetalCommandList::end_render_pass() {
    if (!in_pass_) error_ = "end_render_pass without begin";
    in_pass_ = false;
    push(CmdEndPass{});
}

void MetalCommandList::set_render_targets(std::span<const TargetView> targets, const TargetView* depth) {
    CmdSetTargets c;
    c.count = std::uint32_t(std::min<std::size_t>(targets.size(), 8));
    for (std::uint32_t i = 0; i < c.count; ++i) c.targets[i] = targets[i];
    if (depth && depth->texture) {
        c.depth = *depth;
        c.has_depth = true;
    }
    push(std::move(c));
}

void MetalCommandList::clear_render_target(const TargetView& target, const std::array<float, 4>& color) {
    push(CmdClearTarget{target, color});
}

void MetalCommandList::clear_depth_stencil(const TargetView& target, bool depth, float depth_value, bool stencil,
                                           std::uint8_t stencil_value) {
    push(CmdClearDepth{target, depth, depth_value, stencil, stencil_value});
}

void MetalCommandList::discard(gpu::Resource* resource) { push(CmdDiscard{resource}); }

void MetalCommandList::set_pipeline(gpu::RenderPipeline* pipeline) {
    edit_state().pipeline = static_cast<MetalRenderPipeline*>(pipeline);
}

void MetalCommandList::set_root_layout(const gpu::RootLayout* layout) {
    edit_state().root = static_cast<const MetalRootLayout*>(layout);
}

void MetalCommandList::set_user_data(std::uint32_t first, std::span<const std::uint32_t> dwords) {
    auto& s = edit_state();
    for (std::size_t i = 0; i < dwords.size() && first + i < s.user_data.size(); ++i) s.user_data[first + i] = dwords[i];
}

void MetalCommandList::set_heap_base(std::uint64_t value) { edit_state().heap_base = value; }
void MetalCommandList::set_code_base(std::uint64_t value) { edit_state().code_base = value; }

void MetalCommandList::set_descriptor_table(std::uint32_t binding, gpu::DescriptorHeap* heap) {
    if (binding >= 32) {
        error_ = "table binding above 31";
        return;
    }
    edit_state().tables[binding] = static_cast<MetalDescriptorHeap*>(heap);
}

void MetalCommandList::set_vertex_buffers(std::uint32_t first_slot, std::span<const VertexBufferView> views) {
    auto& s = edit_state();
    for (std::size_t i = 0; i < views.size() && first_slot + i < s.vertex_buffers.size(); ++i)
        s.vertex_buffers[first_slot + i] = views[i];
}

void MetalCommandList::set_index_buffer(const IndexBufferView& view) { edit_state().index = view; }
void MetalCommandList::set_topology(std::uint32_t d3d_topology) { edit_state().topology = d3d_topology; }

void MetalCommandList::set_viewport(const Viewport& viewport) {
    auto& s = edit_state();
    s.viewport = viewport;
    s.viewport_set = true;
}

void MetalCommandList::set_scissor(const Rect& rect) {
    auto& s = edit_state();
    s.scissor = rect;
    s.scissor_set = true;
}

void MetalCommandList::set_blend_factor(const std::array<float, 4>& factor) { edit_state().blend_factor = factor; }
void MetalCommandList::set_stencil_ref(std::uint32_t ref) {
    auto& s = edit_state();
    s.stencil_ref = ref;
    s.stencil_back_ref = ref;
    s.separate_stencil_refs = false;
}

void MetalCommandList::set_stencil_refs(std::uint32_t front, std::uint32_t back) {
    auto& s = edit_state();
    s.stencil_ref = front;
    s.stencil_back_ref = back;
    s.separate_stencil_refs = true;
}

std::shared_ptr<const DrawState> MetalCommandList::snapshot() {
    state_shared_ = true;
    return state_;
}

void MetalCommandList::draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
                            std::uint32_t first_instance) {
    if (!vertex_count || !instance_count) return;
    CmdDraw d;
    d.state = snapshot();
    d.count = vertex_count;
    d.instances = instance_count;
    d.first = first_vertex;
    d.first_instance = first_instance;
    push(std::move(d));
}

void MetalCommandList::draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                                    std::int32_t base_vertex, std::uint32_t first_instance) {
    if (!index_count || !instance_count) return;
    CmdDraw d;
    d.state = snapshot();
    d.indexed = true;
    d.count = index_count;
    d.instances = instance_count;
    d.first = first_index;
    d.base_vertex = base_vertex;
    d.first_instance = first_instance;
    push(std::move(d));
}

void MetalCommandList::copy_buffer(gpu::Buffer* dst, std::uint64_t dst_offset, gpu::Buffer* src,
                                   std::uint64_t src_offset, std::uint64_t size) {
    if (!dst || !src) {
        error_ = "copy_buffer with a null buffer";
        return;
    }
    if (!size) return;
    push(CmdCopyBuffer{static_cast<MetalBuffer*>(dst), dst_offset, static_cast<MetalBuffer*>(src), src_offset, size});
}

void MetalCommandList::copy_buffer_to_texture(const BufferTextureCopy& copy) {
    if (!copy.buffer || !copy.texture) {
        error_ = "copy_buffer_to_texture with a null operand";
        return;
    }
    push(CmdCopyBufferTexture{copy, true});
}

void MetalCommandList::copy_texture_to_buffer(const BufferTextureCopy& copy) {
    if (!copy.buffer || !copy.texture) {
        error_ = "copy_texture_to_buffer with a null operand";
        return;
    }
    push(CmdCopyBufferTexture{copy, false});
}

void MetalCommandList::copy_texture(const TextureCopy& copy) {
    if (!copy.src || !copy.dst) {
        error_ = "copy_texture with a null texture";
        return;
    }
    if (!copy.width || !copy.height || !copy.depth) return;
    push(CmdCopyTexture{copy});
}

void MetalCommandList::fill_buffer_u32(gpu::Buffer* buffer, std::uint64_t offset, std::uint64_t size,
                                       std::uint32_t value) {
    if (!buffer || (offset & 3) || (size & 3)) {
        error_ = "fill_buffer_u32 needs a buffer and 4-byte aligned offset and size";
        return;
    }
    if (!size) return;
    push(CmdFillU32{static_cast<MetalBuffer*>(buffer), offset, size, value});
}

void MetalCommandList::barrier(std::span<const Barrier> barriers) {
    if (barriers.empty()) return;
    push(CmdBarrier{std::vector<Barrier>(barriers.begin(), barriers.end())});
}

void MetalCommandList::write_value(gpu::Buffer* buffer, std::uint64_t offset, std::uint64_t value, std::uint32_t bytes) {
    if (!buffer || (bytes != 4 && bytes != 8) || (offset & 3)) {
        error_ = "write_value needs a buffer, 4 or 8 bytes and a 4-byte aligned offset";
        return;
    }
    push(CmdWriteValue{static_cast<MetalBuffer*>(buffer), offset, value, bytes});
}

void MetalCommandList::wait_value(gpu::Buffer* buffer, std::uint64_t offset, std::uint64_t value, std::uint32_t bytes,
                                  WaitCompare compare) {
    if (!buffer || (bytes != 4 && bytes != 8) || !buffer->cpu_pointer()) {
        error_ = "wait_value needs a CPU-visible buffer and 4 or 8 bytes";
        return;
    }
    push(CmdWaitValue{static_cast<MetalBuffer*>(buffer), offset, value, bytes, compare});
}

void MetalCommandList::begin_event(const char* label) { push(CmdBeginEvent{label ? label : ""}); }
void MetalCommandList::end_event() { push(CmdEndEvent{}); }

void MetalCommandList::set_compute_pipeline(gpu::ComputePipeline* pipeline) {
    edit_state().compute = static_cast<MetalComputePipeline*>(pipeline);
}

void MetalCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t flags) {
    if (!x || !y || !z) return;
    if (in_pass_) {
        error_ = "dispatch inside a render pass";
        return;
    }
    push(CmdDispatch{snapshot(), x, y, z, flags});
}

void MetalCommandList::set_mesh_pipeline(gpu::MeshPipeline* pipeline) {
    edit_state().mesh = static_cast<MetalMeshPipeline*>(pipeline);
}

void MetalCommandList::draw_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!x || !y || !z) return;
    push(CmdDrawMesh{snapshot(), x, y, z});
}

void MetalCommandList::execute_indirect(const IndirectDesc& desc) {
    static constexpr std::uint32_t packed[] = {16, 20, 12, 12};
    const std::uint32_t need = packed[std::uint32_t(desc.kind) & 3];
    if (!desc.arguments || desc.stride < need || (desc.stride & 3) || (desc.argument_offset & 3) ||
        (desc.count_buffer && (desc.count_offset & 3))) {
        error_ = "execute_indirect: arguments, a stride of at least the packed size, 4-byte alignment";
        return;
    }
    if (!desc.max_count) return;
    if (desc.kind == IndirectKind::dispatch && in_pass_) {
        error_ = "indirect dispatch inside a render pass";
        return;
    }
    push(CmdIndirect{snapshot(), desc});
}

void MetalCommandList::begin_query(gpu::QueryHeap* heap, std::uint32_t index) {
    auto* h = static_cast<MetalQueryHeap*>(heap);
    if (!h || index >= h->count() || (h->type() != QueryType::occlusion && h->type() != QueryType::binary_occlusion &&
                                      h->type() != QueryType::pipeline_statistics)) {
        error_ = "begin_query: an occlusion or pipeline-statistics heap and an index in range";
        return;
    }
    push(CmdQuery{h, index, true});
}

void MetalCommandList::end_query(gpu::QueryHeap* heap, std::uint32_t index) {
    auto* h = static_cast<MetalQueryHeap*>(heap);
    if (!h || index >= h->count()) {
        error_ = "end_query: index out of range";
        return;
    }
    push(CmdQuery{h, index, false});
}

void MetalCommandList::resolve_queries(gpu::QueryHeap* heap, std::uint32_t first, std::uint32_t count, gpu::Buffer* dst,
                                       std::uint64_t offset) {
    auto* h = static_cast<MetalQueryHeap*>(heap);
    if (!h || !dst || first + count > h->count() || (offset & 7)) {
        error_ = "resolve_queries: range, destination and 8-byte alignment";
        return;
    }
    if (!count) return;
    push(CmdResolveQueries{h, first, count, static_cast<MetalBuffer*>(dst), offset});
}

void MetalCommandList::write_timestamp(gpu::Buffer* buffer, std::uint64_t offset) {
    if (!buffer || (offset & 7)) {
        error_ = "write_timestamp needs a buffer and an 8-byte aligned offset";
        return;
    }
    push(CmdWriteTimestamp{static_cast<MetalBuffer*>(buffer), offset});
}

}  // namespace gta6rt::metal
