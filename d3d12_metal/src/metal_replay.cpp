// One recorded D3D12 command list (command_list.h) onto a gpu::CommandList: the cut point of ADAPTER.md section 4,
// in place of vulkan_replay.h. Commands Metal has no path for yet are counted by name (MetalContext::note_unsupported)
// and dropped, never half-recorded: a gpu::CommandList that recorded an invalid command fails its close().

#include "metal_formats.h"
#include "metal_context.h"
#include "gfx10_descriptors.h"
#include "rect_list_geometry.h"
#include "title_texture_memory.h"
#include "title_va_alias.h"
#include "vulkan_gpu_labels.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace leonida {

namespace {

struct Resolved {
    gpu::Buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

class Replay {
public:
    Replay(const CommandList& list, gpu::CommandList& out, MetalContext& context, gpu::QueueType queue,
           std::vector<label::Write>* labels, std::vector<std::unique_ptr<gpu::Buffer>>* temps)
        : list_(list), out_(out), context_(context), queue_(queue), labels_(labels), temps_(temps) {}

    ReplayResult run() {
        out_.set_root_layout(context_.root_layout());
        out_.set_heap_base(context_.heap_base());
        for (std::uint32_t b = 0; b < bindless::binding_count; ++b)
            if (gpu::DescriptorHeap* t = context_.table(b)) out_.set_descriptor_table(b, t);
        auto& st = context_.stats();
        for (const Command& command : list_.commands()) {
            st.commands.fetch_add(1, std::memory_order_relaxed);
            std::visit([this](const auto& c) { record(c); }, command);
        }
        if (event_depth_) {
            for (; event_depth_; --event_depth_) out_.end_event();
        }
        return ReplayResult::success;
    }

private:
    // `use`: the list writes or reads the resource itself (not a barrier): a deferred title texture gets its Metal
    // texture here.
    const MetalBinding* binding(ResourceId id, bool use = true) {
        if (!id) return nullptr;
        auto it = bindings_.find(id);
        if (it == bindings_.end()) {
            Cached c;
            c.ok = context_.binding(id, c.b);
            it = bindings_.emplace(id, c).first;
        }
        Cached& c = it->second;
        if (use && c.ok && !c.b.texture && !c.b.buffer && c.b.ensure) {
            if (c.b.ensure(c.b.owner)) {
                c.ok = context_.binding(id, c.b) && c.b.texture;
                context_.stats().deferred_backed.fetch_add(1, std::memory_order_relaxed);
            } else {
                c.ok = false;
            }
        }
        return c.ok ? &c.b : nullptr;
    }
    gpu::Resource* resource(ResourceId id) {
        const MetalBinding* b = binding(id, false);
        if (!b) return nullptr;
        if (b->texture) return b->texture;
        return b->buffer;
    }
    bool buffer_of(ResourceId id, std::uint64_t offset, std::uint64_t bytes, Resolved& r) {
        const MetalBinding* b = binding(id);
        if (!b || !b->buffer) return false;
        if (b->size && (offset > b->size || bytes > b->size - offset)) return false;
        r.buffer = b->buffer;
        r.offset = b->buffer_offset + offset;
        r.bytes = bytes;
        return true;
    }
    // A console VA: a buffer resource or VA range, an arena offset, or a title address with a window. r.bytes is how
    // much of [address, address + bytes) the buffer found holds (a view may name more than its buffer has).
    bool va(std::uint64_t address, std::uint64_t bytes, Resolved& r, TitleVaAliases::Kind kind) {
        if (!address) return false;
        std::uint64_t n = bytes ? bytes : 1;
        if (context_.buffer_at(address, n, r.buffer, r.offset) && n) {
            r.bytes = n;
            return true;
        }
        // The title's own memory (a per-frame ring): the device's resolver makes a window of those pages, mirrored
        // before every submission, as for a V# in a bound table.
        const std::uint64_t alias = title_va_aliases().resolve(address, bytes ? bytes : 1, kind);
        if (alias != address) {
            n = bytes ? bytes : 1;
            if (context_.buffer_at(alias, n, r.buffer, r.offset) && n) {
                r.bytes = n;
                return true;
            }
        }
        return false;
    }
    void unsupported(const char* what) { context_.note_unsupported(what); }

    gpu::TargetView target(const TargetView& v, const MetalBinding& b, std::uint32_t layer) {
        gpu::TargetView t;
        t.texture = b.texture;
        t.format = v.format == ColorViewFormat::resource ? 0 : metal_format(static_cast<VkFormat>(v.format));
        t.mip = v.mip;
        t.layer = layer;
        return t;
    }

    void barrier_all(std::uint32_t before, std::uint32_t after, gpu::Resource* r = nullptr) {
        gpu::Barrier b;
        b.type = gpu::BarrierType::transition;
        b.resource = r;
        b.before = before;
        b.after = after;
        out_.barrier({&b, 1});
        context_.stats().barriers.fetch_add(1, std::memory_order_relaxed);
    }
    static bool writes(std::uint32_t access) {
        constexpr std::uint32_t w = d3d12_access::render_target | d3d12_access::unordered_access |
                                    d3d12_access::depth_stencil_write | d3d12_access::stream_output |
                                    d3d12_access::copy_dest | d3d12_access::resolve_dest |
                                    d3d12_access::raytracing_acceleration_structure_write;
        return access == d3d12_access::common || (access & w);
    }
    void enhanced(std::uint32_t access_before, std::uint32_t access_after, ResourceId id) {
        if (access_before == d3d12_access::no_access && access_after == d3d12_access::no_access) return;
        if (!writes(access_before) && !writes(access_after)) return;
        barrier_all(d3d12_state::unordered_access, d3d12_state::unordered_access, id ? resource(id) : nullptr);
    }

    void record(const TransitionBarrier& c) {
        gpu::Barrier b;
        b.type = gpu::BarrierType::transition;
        b.resource = resource(c.resource);
        b.before = c.before;
        b.after = c.after;
        out_.barrier({&b, 1});
        context_.stats().barriers.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const UavBarrier& c) {
        gpu::Barrier b;
        b.type = gpu::BarrierType::uav;
        b.resource = c.resource ? resource(c.resource) : nullptr;
        out_.barrier({&b, 1});
        context_.stats().barriers.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const AliasingBarrier&) {
        gpu::Barrier b;
        b.type = gpu::BarrierType::aliasing;
        out_.barrier({&b, 1});
        context_.stats().barriers.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const GlobalBarrier& c) { enhanced(c.access_before, c.access_after, 0); }
    void record(const BufferBarrier& c) { enhanced(c.access_before, c.access_after, c.resource); }
    void record(const TextureBarrier& c) {
        if (c.flags & texture_barrier_discard)
            if (const MetalBinding* b = binding(c.resource, false); b && b->texture) out_.discard(b->texture);
        enhanced(c.access_before, c.access_after, c.resource);
    }

    void record(const ClearRenderTarget& c) {
        const MetalBinding* b = binding(c.target.resource);
        if (!b || !b->texture) return unsupported("ClearRenderTargetView of an unknown texture");
        if (!c.rects.empty()) return unsupported("ClearRenderTargetView with rectangles");
        const std::uint32_t layers = std::max(1u, c.target.layer_count);
        for (std::uint32_t l = 0; l < layers; ++l)
            out_.clear_render_target(target(c.target, *b, c.target.first_layer + l), c.color);
        rt_dirty_ = true;
        context_.stats().clears.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const ClearDepthStencil& c) {
        const MetalBinding* b = binding(c.target.resource);
        if (!b || !b->texture) return unsupported("ClearDepthStencilView of an unknown texture");
        if (!c.rects.empty()) return unsupported("ClearDepthStencilView with rectangles");
        const std::uint32_t layers = std::max(1u, c.target.layer_count);
        for (std::uint32_t l = 0; l < layers; ++l)
            out_.clear_depth_stencil(target(c.target, *b, c.target.first_layer + l), c.flags & clear_depth, c.depth,
                                     (c.flags & clear_stencil) && b->stencil, c.stencil);
        rt_dirty_ = true;
        context_.stats().clears.fetch_add(1, std::memory_order_relaxed);
    }

    void record(const CopyBuffer& c) {
        if (c.tiled_resource) return unsupported("CopyTiles on a tiled buffer");
        Resolved d, s;
        if (!buffer_of(c.dst, c.dst_offset, c.size, d) || !buffer_of(c.src, c.src_offset, c.size, s))
            return unsupported("CopyBufferRegion outside a known buffer");
        out_.copy_buffer(d.buffer, d.offset, s.buffer, s.offset, c.size);
        context_.stats().copies.fetch_add(1, std::memory_order_relaxed);
    }

    struct Sub {
        std::uint32_t mip = 0, layer = 0, plane = 0;
    };
    static Sub subresource(const MetalBinding& b, std::uint32_t index) {
        const std::uint32_t mips = std::max(1u, b.mip_levels);
        const std::uint32_t layers = std::max(1u, b.array_layers);
        return {index % mips, (index / mips) % layers, index / (mips * layers)};
    }
    void record(const CopyTexture& c) {
        if (c.tile_footprint) return unsupported("CopyTiles footprint of a tiled texture");
        const bool to_texture = c.src.footprint;
        const CopyLocation& tex = to_texture ? c.dst : c.src;
        const CopyLocation& foot = to_texture ? c.src : c.dst;
        if (!c.src.footprint && !c.dst.footprint) return copy_between_textures(c);
        const MetalBinding* t = binding(tex.resource);
        const MetalBinding* f = binding(foot.resource);
        if (!t || !t->texture || !f || !f->buffer) return unsupported("CopyTextureRegion of an unknown resource");
        const Sub sub = subresource(*t, tex.subresource);
        if (t->stencil && sub.plane == 1) return unsupported("CopyTextureRegion of a stencil plane");
        if (t->depth24 && t->depth_stencil) return unsupported("CopyTextureRegion of a D24 texture (footprint repack)");
        std::uint32_t x = 0, y = 0, z = 0, w = foot.width, h = foot.height, d = foot.depth;
        if (to_texture) {
            x = c.x, y = c.y, z = c.z;
            if (c.has_box) {
                w = c.box.right - c.box.left, h = c.box.bottom - c.box.top, d = c.box.back - c.box.front;
                if (c.box.left || c.box.top || c.box.front) return unsupported("CopyTextureRegion from a footprint box");
            }
        } else if (c.has_box) {
            x = c.box.left, y = c.box.top, z = c.box.front;
            w = c.box.right - c.box.left, h = c.box.bottom - c.box.top, d = c.box.back - c.box.front;
            if (c.x || c.y || c.z) return unsupported("CopyTextureRegion into a footprint at an offset");
        }
        if (!t->volume && (z || d > 1)) return unsupported("CopyTextureRegion of a 2D texture with a depth range");
        const std::uint32_t block = std::max(1u, t->block_dim);
        const std::uint64_t slice_bytes = std::uint64_t(foot.row_pitch) * ((foot.height + block - 1) / block);
        for (std::uint32_t i = 0; i < std::max(1u, d); ++i) {
            gpu::BufferTextureCopy k;
            k.buffer = f->buffer;
            k.offset = f->buffer_offset + foot.offset + i * slice_bytes;
            k.row_pitch = foot.row_pitch;
            k.texture = t->texture;
            k.mip = sub.mip;
            k.layer = t->volume ? z + i : sub.layer;
            k.x = x, k.y = y, k.width = w, k.height = h;
            k.depth_plane = t->depth_stencil;
            if (to_texture) out_.copy_buffer_to_texture(k);
            else out_.copy_texture_to_buffer(k);
        }
        context_.stats().copies.fetch_add(1, std::memory_order_relaxed);
    }
    // Texture to texture: one subresource region, between textures of the same DXGI format (Metal copies texels
    // as they are; a copy between two formats of one family is not one Metal blit and is counted instead).
    bool texture_copy(const MetalBinding& d, std::uint32_t dst_sub, std::uint32_t dx, std::uint32_t dy, std::uint32_t dz,
                      const MetalBinding& s, std::uint32_t src_sub, const CopyBox* box) {
        const Sub ds = subresource(d, dst_sub), ss = subresource(s, src_sub);
        if (ds.plane || ss.plane) return unsupported("CopyTextureRegion of a stencil plane"), false;
        if (d.samples > 1 || s.samples > 1) return unsupported("CopyTextureRegion of a multisampled texture"), false;
        const std::uint32_t sw = std::max(1u, s.width >> ss.mip), sh = std::max(1u, s.height >> ss.mip);
        const std::uint32_t sd = s.volume ? std::max(1u, s.depth >> ss.mip) : 1;
        CopyBox b{0, 0, 0, sw, sh, sd};
        if (box) b = *box;
        if (b.right > sw || b.bottom > sh || b.back > sd || b.left >= b.right || b.top >= b.bottom || b.front >= b.back)
            return true;
        if (d.format != s.format) return copy_through_buffer(d, ds, dx, dy, dz, s, ss, b);
        gpu::TextureCopy k;
        k.src = s.texture;
        k.src_mip = ss.mip;
        k.src_layer = s.volume ? b.front : ss.layer;
        k.src_x = b.left;
        k.src_y = b.top;
        k.dst = d.texture;
        k.dst_mip = ds.mip;
        k.dst_layer = d.volume ? dz : ds.layer;
        k.dst_x = dx;
        k.dst_y = dy;
        k.width = b.right - b.left;
        k.height = b.bottom - b.top;
        k.depth = s.volume ? b.back - b.front : 1;
        out_.copy_texture(k);
        return true;
    }
    // Two formats of one block size (D3D12 allows a copy between formats of a family and, on the console, BC blocks
    // to and from 64/128-bit texels): the bytes go through a buffer of this submission, block for block.
    bool copy_through_buffer(const MetalBinding& d, const Sub& ds, std::uint32_t dx, std::uint32_t dy, std::uint32_t dz,
                             const MetalBinding& s, const Sub& ss, const CopyBox& b) {
        const std::uint32_t sk = std::max(1u, s.block_dim), dk = std::max(1u, d.block_dim);
        if (!temps_ || !s.block_bytes || s.block_bytes != d.block_bytes || s.depth_stencil || d.depth_stencil) {
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1) < 8)
                std::fprintf(stderr, "[rt] d3d12-metal: CopyTextureRegion DXGI %u -> %u (block %u B x%u -> %u B x%u) "
                                     "not copied\n", s.format, d.format, s.block_bytes, sk, d.block_bytes, dk);
            return unsupported("CopyTextureRegion between textures of two formats of different block sizes"), false;
        }
        const std::uint32_t bw = (b.right - b.left + sk - 1) / sk, bh = (b.bottom - b.top + sk - 1) / sk;
        const std::uint32_t slices = b.back - b.front;
        const std::uint32_t pitch = (bw * s.block_bytes + 255) & ~255u;
        const std::uint64_t slice_bytes = std::uint64_t(pitch) * bh;
        gpu::BufferDesc bd;
        bd.size = slice_bytes * slices;
        bd.memory = gpu::MemoryKind::device_local;
        bd.usage = gpu::usage::copy;
        bd.label = "texture format copy";
        auto buffer = context_.gpu().create_buffer(bd);
        if (!buffer) return unsupported("CopyTextureRegion between two formats: no staging buffer"), false;
        const std::uint32_t dw = std::max(1u, d.width >> ds.mip), dh = std::max(1u, d.height >> ds.mip);
        for (std::uint32_t z = 0; z < slices; ++z) {
            gpu::BufferTextureCopy k;
            k.buffer = buffer.get();
            k.offset = z * slice_bytes;
            k.row_pitch = pitch;
            k.texture = s.texture;
            k.mip = ss.mip;
            k.layer = s.volume ? b.front + z : ss.layer;
            k.x = b.left, k.y = b.top, k.width = b.right - b.left, k.height = b.bottom - b.top;
            out_.copy_texture_to_buffer(k);
        }
        barrier_all(d3d12_state::copy_dest, d3d12_state::copy_source, buffer.get());
        for (std::uint32_t z = 0; z < slices; ++z) {
            gpu::BufferTextureCopy k;
            k.buffer = buffer.get();
            k.offset = z * slice_bytes;
            k.row_pitch = pitch;
            k.texture = d.texture;
            k.mip = ds.mip;
            k.layer = d.volume ? dz + z : ds.layer;
            k.x = dx, k.y = dy;
            k.width = std::min(bw * dk, dw > dx ? dw - dx : 0u);
            k.height = std::min(bh * dk, dh > dy ? dh - dy : 0u);
            if (k.width && k.height) out_.copy_buffer_to_texture(k);
        }
        temps_->push_back(std::move(buffer));
        context_.stats().format_copies.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    void copy_between_textures(const CopyTexture& c) {
        const MetalBinding* d = binding(c.dst.resource);
        const MetalBinding* s = binding(c.src.resource);
        if (!d || !d->texture || !s || !s->texture) return unsupported("CopyTextureRegion of an unknown texture");
        if (texture_copy(*d, c.dst.subresource, c.x, c.y, c.z, *s, c.src.subresource, c.has_box ? &c.box : nullptr))
            context_.stats().copies.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const CopyResource& c) {
        const MetalBinding* d = binding(c.dst);
        const MetalBinding* s = binding(c.src);
        if (!d || !s) return unsupported("CopyResource of an unknown resource");
        if (d->buffer && s->buffer && !d->texture && !s->texture) {
            out_.copy_buffer(d->buffer, d->buffer_offset, s->buffer, s->buffer_offset, std::min(d->size, s->size));
            context_.stats().copies.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!d->texture || !s->texture) return unsupported("CopyResource between a buffer and a texture");
        const std::uint32_t mips = std::max(1u, s->mip_levels), layers = s->volume ? 1 : std::max(1u, s->array_layers);
        for (std::uint32_t layer = 0; layer < layers; ++layer)
            for (std::uint32_t mip = 0; mip < mips; ++mip)
                if (!texture_copy(*d, mip + layer * mips, 0, 0, 0, *s, mip + layer * mips, nullptr)) return;
        context_.stats().copies.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const ResolveSubresource&) { unsupported("ResolveSubresource"); }
    void record(const Predication& c) {
        if (c.buffer) unsupported("SetPredication (predicated work runs unconditionally)");
    }
    // A buffer view: every 32-bit word of the view (or of its element rects) gets values[0], which is what D3D12 does
    // for raw and structured views and for R32 typed ones (VK_FORMAT_R32_UINT/SINT/SFLOAT); other typed buffer views
    // and texture views are counted.
    void record(const ClearUnorderedAccess& c) {
        const MetalBinding* b = binding(c.view.resource);
        if (!b) return unsupported("ClearUnorderedAccessView of an unknown resource");
        if (!c.view.buffer) return clear_texture_uav(c, *b);
        if (!b->buffer) return unsupported("ClearUnorderedAccessView of a buffer view with no buffer");
        if (c.view.format && c.view.format != 98 && c.view.format != 99 && c.view.format != 100)
            return unsupported("ClearUnorderedAccessView of a typed buffer view other than R32");
        const std::uint32_t element = std::max(4u, c.view.element_size ? c.view.element_size : 4u);
        const auto fill = [&](std::uint64_t first, std::uint64_t bytes) {
            first &= ~std::uint64_t(3);
            bytes &= ~std::uint64_t(3);
            if (!bytes) return;
            if (b->size && (first > b->size || bytes > b->size - first)) bytes = first < b->size ? (b->size - first) & ~3ull : 0;
            if (bytes) out_.fill_buffer_u32(b->buffer, b->buffer_offset + first, bytes, c.values[0]);
        };
        if (c.rects.empty()) {
            fill(c.view.offset, c.view.size);
        } else {
            for (const Rect& r : c.rects)
                if (r.right > r.left && r.left >= 0)
                    fill(c.view.offset + std::uint64_t(r.left) * element,
                         std::uint64_t(std::int64_t(r.right) - r.left) * element);
        }
        context_.stats().clears.fetch_add(1, std::memory_order_relaxed);
    }
    // A texture view of a texture that can be a render target (2D or 2D array, created with render-target usage):
    // cleared as one, the four values as the clear colour (float bits for ClearUnorderedAccessViewFloat).
    void clear_texture_uav(const ClearUnorderedAccess& c, const MetalBinding& b) {
        if (!b.texture || b.volume || b.samples > 1 || b.depth_stencil || !c.rects.empty() ||
            !(b.texture->desc().usage & gpu::usage::render_target))
            return unsupported("ClearUnorderedAccessView of a texture (not a 2D render-target-capable texture)");
        std::array<float, 4> color;
        for (int i = 0; i < 4; ++i) color[i] = c.is_float ? std::bit_cast<float>(c.values[i]) : float(c.values[i]);
        const std::uint32_t layers = c.view.layer_count ? c.view.layer_count : 1;
        for (std::uint32_t l = 0; l < layers; ++l) {
            gpu::TargetView t;
            t.texture = b.texture;
            t.mip = c.view.mip;
            t.layer = c.view.first_layer + l;
            out_.clear_render_target(t, color);
        }
        rt_dirty_ = true;
        context_.stats().clears.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const DiscardResource& c) {
        if (const MetalBinding* b = binding(c.resource, false); b && b->texture) {
            out_.discard(b->texture);
            rt_dirty_ = true;
        }
    }
    void record(const WriteImmediate& c) {
        gpu::Buffer* buffer = nullptr;
        std::uint64_t offset = 0;
        if (!context_.resolve_write(c.address, buffer, offset)) return unsupported("WriteBufferImmediate to an unknown VA");
        if (c.mode == write_immediate_mode::marker_out) barrier_all(d3d12_state::unordered_access, d3d12_state::copy_dest);
        out_.write_value(buffer, offset, c.value, 4);
    }
    void record(const label::Write& c) {
        if (c.clock == 2) return unsupported("GPU label with the system clock");
        if (labels_) labels_->push_back(c);
    }
    void record(const AtomicCopyBuffer& c) {
        Resolved d, s;
        if (!buffer_of(c.dst, c.dst_offset, c.bytes, d) || !buffer_of(c.src, c.src_offset, c.bytes, s))
            return unsupported("AtomicCopyBufferUINT outside a known buffer");
        barrier_all(d3d12_state::unordered_access, d3d12_state::copy_source);
        out_.copy_buffer(d.buffer, d.offset, s.buffer, s.offset, c.bytes);
    }
    void record(const BeginQuery& c) {
        QueryHeapEntry e;
        if (!context_.query_heap(c.heap, e) || !e.heap) return unsupported("BeginQuery on an unknown heap");
        out_.begin_query(e.heap, c.index);
    }
    void record(const EndQuery& c) {
        QueryHeapEntry e;
        if (!context_.query_heap(c.heap, e) || !e.heap) return unsupported("EndQuery on an unknown heap");
        out_.end_query(e.heap, c.index);
    }
    void record(const ResolveQueries& c) {
        QueryHeapEntry e;
        if (!context_.query_heap(c.heap, e) || !e.heap) return unsupported("ResolveQueryData on an unknown heap");
        Resolved d;
        const std::uint64_t bytes = std::uint64_t(c.count) * std::max(8u, query_result_size(c.type));
        if (!buffer_of(c.dst, c.offset, bytes, d)) return unsupported("ResolveQueryData into an unknown buffer");
        out_.resolve_queries(e.heap, c.first, c.count, d.buffer, d.offset);
    }
    void record(const DebugMarker& c) {
        switch (c.kind) {
        case DebugMarker::Kind::set:
            out_.begin_event(c.label.c_str());
            out_.end_event();
            break;
        case DebugMarker::Kind::begin:
            out_.begin_event(c.label.c_str());
            ++event_depth_;
            break;
        case DebugMarker::Kind::end:
            if (event_depth_) {
                out_.end_event();
                --event_depth_;
            }
            break;
        }
    }
    void record(const BuildAccelerationStructure&) { unsupported("BuildRaytracingAccelerationStructure"); }
    void record(const CopyAccelerationStructure&) { unsupported("CopyRaytracingAccelerationStructure"); }
    void record(const AccelerationStructurePostbuild&) { unsupported("EmitRaytracingAccelerationStructurePostbuildInfo"); }

    using Work = MetalContext::Work;
    using Fate = MetalContext::Fate;
    static Fate pipeline_fate(const gpu::Pipeline* pl, const char*& why) {
        if (!pl) return Fate::library_pending;
        switch (pl->state()) {
        case gpu::PipelineState::failed: why = pl->error().c_str(); return Fate::metal_refused;
        case gpu::PipelineState::compiling:
        case gpu::PipelineState::queued: return Fate::compiling;
        default: return Fate::recorded;
        }
    }
    Fate compute(std::uintptr_t id, MetalComputePipeline& p, const char*& why) {
        if (!context_.compute_pipeline(id, p)) {
            unsupported("Dispatch with an unknown compute pipeline");
            return Fate::pso_unknown;
        }
        if (p.refused) {
            why = p.name;
            return Fate::pso_refused;
        }
        if (!p.pipeline && p.ensure && p.ensure(p.owner)) context_.compute_pipeline(id, p);
        if (p.refused) {
            why = p.name;
            return Fate::pso_refused;
        }
        return pipeline_fate(p.pipeline, why);
    }
    void set_compute(const MetalComputePipeline& p, const std::array<std::uint32_t, 32>& user_data) {
        if (p.pipeline != bound_compute_) {
            out_.set_compute_pipeline(p.pipeline);
            bound_compute_ = p.pipeline;
        }
        out_.set_user_data(0, user_data);
        out_.set_code_base(p.code_base);
    }
    void record(const Dispatch& c) {
        auto& st = context_.stats();
        MetalComputePipeline p;
        const char* why = nullptr;
        if (const Fate f = compute(c.pipeline, p, why); f != Fate::recorded) {
            st.dispatches_skipped.fetch_add(1, std::memory_order_relaxed);
            context_.note_work(Work::dispatch, f, why);
            if (MetalContext::census_on()) context_.note_census(p.name, "dispatch-skipped");
            return;
        }
        context_.note_work(Work::dispatch, Fate::recorded);
        if (MetalContext::census_on()) context_.note_census(p.name, "dispatch");
        set_compute(p, c.user_data);
        out_.dispatch(c.x, c.y, c.z);
        st.dispatches.fetch_add(1, std::memory_order_relaxed);
    }

    void record(const ExecuteIndirect& c) {
        auto& st = context_.stats();
        if (c.kind != IndirectKind::dispatch) {
            context_.note_work(Work::indirect_uncaptured, Fate::not_implemented);
            return unsupported("ExecuteIndirect draw without captured draw state");
        }
        MetalComputePipeline p;
        const char* why = nullptr;
        const Fate f = c.pipeline ? compute(c.pipeline, p, why) : Fate::pso_unknown;
        if (f != Fate::recorded) {
            st.dispatches_skipped.fetch_add(1, std::memory_order_relaxed);
            context_.note_work(Work::indirect_dispatch, f, why);
            return;
        }
        gpu::IndirectDesc d;
        if (!indirect(gpu::IndirectKind::dispatch, c.stride, c.max_count, c.argument_buffer, c.argument_offset,
                      c.count_buffer, c.count_offset, d)) {
            context_.note_work(Work::indirect_dispatch, Fate::arguments_unknown);
            return unsupported("ExecuteIndirect with an unknown argument or count buffer");
        }
        context_.note_work(Work::indirect_dispatch, Fate::recorded);
        set_compute(p, c.user_data);
        out_.execute_indirect(d);
        st.dispatches.fetch_add(1, std::memory_order_relaxed);
    }
    bool indirect(gpu::IndirectKind kind, std::uint32_t stride, std::uint32_t max_count, ResourceId args,
                  std::uint64_t args_offset, ResourceId count, std::uint64_t count_offset, gpu::IndirectDesc& d) {
        Resolved a;
        if (!buffer_of(args, args_offset, 4, a)) return false;
        d.kind = kind;
        d.stride = stride;
        d.max_count = max_count;
        d.arguments = a.buffer;
        d.argument_offset = a.offset;
        if (count) {
            Resolved n;
            if (!buffer_of(count, count_offset, 4, n)) return false;
            d.count_buffer = n.buffer;
            d.count_offset = n.offset;
        }
        return true;
    }

    Fate graphics(const Draw& draw, MetalGraphicsPipeline& p, const char*& why) {
        if (!context_.graphics_pipeline(draw.pipeline, p)) {
            unsupported("draw with an unknown graphics pipeline");
            return Fate::pso_unknown;
        }
        if (p.refused) {
            why = p.name;
            return Fate::pso_refused;
        }
        if (!p.render && !p.mesh && p.ensure && p.ensure(p.owner)) context_.graphics_pipeline(draw.pipeline, p);
        if (p.refused) {
            why = p.name;
            return Fate::pso_refused;
        }
        if (p.mesh) return pipeline_fate(p.mesh, why);
        return pipeline_fate(p.render, why);
    }
    static Work work_of(const Draw& draw, const MetalGraphicsPipeline& p, bool indirect) {
        if (draw.mesh || p.mesh)
            return indirect ? (p.split ? Work::indirect_mesh_split : Work::indirect_mesh)
                            : (p.split ? Work::mesh_split : Work::mesh);
        if (draw.topology == gpu::topology::rect_list) return indirect ? Work::indirect_rect : Work::draw_rect;
        if (draw.indexed) return indirect ? Work::indirect_draw_indexed : Work::draw_indexed;
        return indirect ? Work::indirect_draw : Work::draw;
    }
    bool bind_targets(const Draw& draw) {
        std::array<gpu::TargetView, 8> rts{};
        std::uint32_t count = 0;
        if (draw.views) {
            count = std::min<std::uint32_t>(draw.views->render_target_count, 8);
            for (std::uint32_t i = 0; i < count; ++i) {
                const TargetView& v = draw.views->render_targets[i];
                if (!v.resource) continue;
                const MetalBinding* b = binding(v.resource);
                if (!b || !b->texture) return false;
                rts[i] = target(v, *b, v.first_layer);
            }
        }
        gpu::TargetView depth;
        const gpu::TargetView* dp = nullptr;
        if (draw.depth.resource) {
            const MetalBinding* b = binding(draw.depth.resource);
            if (!b || !b->texture) return false;
            depth = target(draw.depth, *b, draw.depth.first_layer);
            depth.format = 0;
            dp = &depth;
        }
        const bool same = !rt_dirty_ && count == bound_rt_count_ && (dp != nullptr) == has_depth_ &&
                          (!dp || (depth.texture == bound_depth_.texture && depth.mip == bound_depth_.mip &&
                                   depth.layer == bound_depth_.layer)) &&
                          std::equal(rts.begin(), rts.begin() + count, bound_rts_.begin(), [](const auto& a, const auto& b) {
                              return a.texture == b.texture && a.format == b.format && a.mip == b.mip && a.layer == b.layer;
                          });
        if (same) return true;
        out_.set_render_targets({rts.data(), count}, dp);
        bound_rts_ = rts;
        bound_rt_count_ = count;
        bound_depth_ = depth;
        has_depth_ = dp != nullptr;
        rt_dirty_ = false;
        return true;
    }
    Fate bind_draw_state(const Draw& draw, const MetalGraphicsPipeline& p, const char*& why) {
        if (!bind_targets(draw)) {
            unsupported("draw with an unknown render target or depth texture");
            return Fate::target_unknown;
        }
        if (p.mesh) {
            out_.set_mesh_pipeline(p.mesh);
        } else {
            gpu::RenderPipeline* pipeline = p.render;
            if (draw.topology == gpu::topology::rect_list && p.topology_type != topology_type_rect_list) {
                pipeline = p.rect;
                if (!pipeline && p.ensure_rect && p.ensure_rect(p.owner)) {
                    MetalGraphicsPipeline again;
                    if (context_.graphics_pipeline(draw.pipeline, again)) pipeline = again.rect;
                }
                if (!pipeline) {
                    unsupported("rect-list draw whose rectangle pipeline variant is not built");
                    return Fate::library_pending;
                }
                if (const Fate f = pipeline_fate(pipeline, why); f != Fate::recorded) return f;
            }
            out_.set_pipeline(pipeline);
            if (draw.views) {
                std::array<gpu::VertexBufferView, 8> vbs{};
                std::uint32_t used = 0;
                for (std::uint32_t slot = 0; slot < 32; ++slot) {
                    const VertexBufferView& v = draw.views->vertex_buffers[slot];
                    if (!v.va) continue;
                    if (slot >= 8) {
                        unsupported("vertex buffer slots 8-31");
                        continue;
                    }
                    Resolved r;
                    if (!va(v.va, v.size, r, TitleVaAliases::vertex_view)) {
                        unsupported("vertex buffer at an unknown VA");
                        static std::atomic<unsigned> logged{0};
                        if (logged.fetch_add(1) < 8)
                            std::fprintf(stderr, "[rt] d3d12-metal: vertex buffer slot %u va 0x%llx size %u stride %u "
                                                 "names no buffer\n",
                                         slot, static_cast<unsigned long long>(v.va), v.size, v.stride);
                        return Fate::vertex_unknown;
                    }
                    vbs[slot] = {r.buffer->gpu_va() + r.offset, std::uint32_t(std::min<std::uint64_t>(v.size, r.bytes)),
                                 v.stride};
                    used = slot + 1;
                }
                if (used) out_.set_vertex_buffers(0, {vbs.data(), used});
            }
            if (draw.indexed) {
                gpu::IndexBufferView ib;
                if (draw.index.va) {
                    Resolved r;
                    if (!va(draw.index.va, draw.index.size, r, TitleVaAliases::index_view)) {
                        unsupported("index buffer at an unknown VA");
                        return Fate::index_unknown;
                    }
                    ib.va = r.buffer->gpu_va() + r.offset;
                    ib.size = std::uint32_t(std::min<std::uint64_t>(draw.index.size, r.bytes));
                    ib.format = draw.index.format == 42 ? gpu::format::r32_uint : gpu::format::r16_uint;
                    out_.set_index_buffer(ib);
                } else {
                    unsupported("indexed draw without an index buffer");
                    return Fate::index_unknown;
                }
            }
            out_.set_topology(draw.topology);
        }
        out_.set_user_data(0, draw.user_data);
        out_.set_code_base(p.code_base);
        const Viewport& v = draw.viewport;
        out_.set_viewport({v.x, v.y, v.width, v.height, v.min_depth, v.max_depth});
        out_.set_scissor({draw.scissor.left, draw.scissor.top, draw.scissor.right, draw.scissor.bottom});
        out_.set_blend_factor(draw.blend_factor);
        out_.set_stencil_refs(draw.stencil_front, draw.stencil_back);
        if (draw.viewports) unsupported("more than one viewport or scissor (the first is used)");
        return Fate::recorded;
    }
    void record(const Draw& draw) {
        auto& st = context_.stats();
        MetalGraphicsPipeline p;
        const char* why = nullptr;
        Fate f = graphics(draw, p, why);
        if (f == Fate::recorded) f = bind_draw_state(draw, p, why);
        context_.note_work(work_of(draw, p, false), f, why);
        if (f != Fate::recorded) {
            st.draws_skipped.fetch_add(1, std::memory_order_relaxed);
            if (MetalContext::census_on()) context_.note_census(p.name, "draw-skipped");
            return;
        }
        if (MetalContext::census_on())
            context_.note_census(p.name, draw.topology == gpu::topology::rect_list ? "draw-rect" : "draw");
        trace_draw(draw, p);
        if (p.mesh) out_.draw_mesh(draw.count, draw.instances, draw.first);
        else if (draw.indexed) out_.draw_indexed(draw.count, draw.instances, draw.first, draw.base_vertex, draw.first_instance);
        else out_.draw(draw.count, draw.instances, draw.first, draw.first_instance);
        st.draws.fetch_add(1, std::memory_order_relaxed);
    }
    void record(const IndirectRasterDraw& payload) {
        auto& st = context_.stats();
        if (!payload) return;
        const Draw& draw = payload->state;
        const IndirectDrawArgs& a = payload->arguments;
        MetalGraphicsPipeline p;
        const char* why = nullptr;
        Fate f = graphics(draw, p, why);
        const Work work = work_of(draw, p, true);
        if (f == Fate::recorded && !p.mesh && work != Work::indirect_rect &&
            (draw.topology == gpu::topology::quad_list || draw.topology > gpu::topology::triangle_strip)) {
            unsupported("indirect quad-list or adjacency draw");
            f = Fate::not_implemented;
        }
        if (f == Fate::recorded) f = bind_draw_state(draw, p, why);
        gpu::IndirectDesc d;
        const gpu::IndirectKind kind = p.mesh         ? gpu::IndirectKind::dispatch_mesh
                                       : draw.indexed ? gpu::IndirectKind::draw_indexed
                                                      : gpu::IndirectKind::draw;
        if (f == Fate::recorded && !indirect(kind, a.stride, a.max_count, a.argument_buffer, a.argument_offset,
                                             a.count_buffer, a.count_offset, d)) {
            unsupported("indirect draw with an unknown argument or count buffer");
            f = Fate::arguments_unknown;
        }
        context_.note_work(work, f, why);
        if (f != Fate::recorded) {
            st.draws_skipped.fetch_add(1, std::memory_order_relaxed);
            if (MetalContext::census_on()) context_.note_census(p.name, "indirect-draw-skipped");
            return;
        }
        if (MetalContext::census_on()) context_.note_census(p.name, "indirect-draw");
        out_.execute_indirect(d);
        st.draws.fetch_add(1, std::memory_order_relaxed);
    }

    // GTA6RT_METAL_DRAW_TRACE=<part of a pipeline name>: the first 8 draws of such pipelines, with their vertex
    // buffers and the first two vertices' first 32 bytes as floats (read at the buffer's CPU address).
    // GTA6RT_METAL_DRAW_TRACE_EVERY=N: then every Nth such draw too, 64 lines at most. GTA6RT_METAL_DRAW_TRACE_TABLE=
    // <hex offsets>: the descriptor table at s8 (as the shader reads it, heap_base + s8) and, for each listed offset
    // holding a V#, the V# and the first 32 words of the memory it names.
    void trace_draw(const Draw& draw, const MetalGraphicsPipeline& p) {
        static const std::vector<std::string> wants = [] {
            std::vector<std::string> out;
            const char* v = std::getenv("GTA6RT_METAL_DRAW_TRACE");
            for (std::string s = v ? v : ""; !s.empty();) {
                const auto comma = s.find(',');
                out.push_back(s.substr(0, comma));
                s = comma == std::string::npos ? "" : s.substr(comma + 1);
            }
            return out;
        }();
        static const unsigned every = [] {
            const char* v = std::getenv("GTA6RT_METAL_DRAW_TRACE_EVERY");
            return v ? unsigned(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        static std::atomic<unsigned> seen[4]{}, printed{0};
        if (wants.empty() || !p.name) return;
        std::size_t which = 0;
        while (which < wants.size() && (wants[which].empty() || !std::strstr(p.name, wants[which].c_str()))) ++which;
        if (which == wants.size()) return;
        const unsigned n = seen[std::min<std::size_t>(which, 3)].fetch_add(1);
        if (n >= 8 && (!every || n % every)) return;
        if (printed.fetch_add(1) >= 96) return;
        std::string text;
        char line[256];
        std::snprintf(line, sizeof line, "%s: count %u instances %u first %u indexed %d topology %u viewport %.0f,%.0f %.0fx%.0f",
                      p.name, draw.count, draw.instances, draw.first, int(draw.indexed), unsigned(draw.topology),
                      draw.viewport.x, draw.viewport.y, draw.viewport.width, draw.viewport.height);
        text = line;
        for (std::uint32_t slot = 0; draw.views && slot < 8; ++slot) {
            const VertexBufferView& v = draw.views->vertex_buffers[slot];
            if (!v.va) continue;
            float f[16] = {};
            Resolved r;
            bool read = false;
            if (va(v.va, std::min<std::uint64_t>(v.size, 64), r, TitleVaAliases::vertex_view) && r.buffer &&
                r.buffer->cpu_pointer() && r.bytes) {
                std::memcpy(f, static_cast<const std::uint8_t*>(r.buffer->cpu_pointer()) + r.offset,
                            std::size_t(std::min<std::uint64_t>(r.bytes, sizeof f)));
                read = true;
            }
            std::snprintf(line, sizeof line, "; vb%u va 0x%llx size %u stride %u%s:", slot,
                          static_cast<unsigned long long>(v.va), v.size, v.stride, read ? "" : " (not read)");
            text += line;
            for (int i = 0; read && i < 16; ++i) {
                std::snprintf(line, sizeof line, " %g", f[i]);
                text += line;
            }
        }
        std::fprintf(stderr, "[rt] d3d12-metal: draw trace %u %s\n", n, text.c_str());
        trace_table(draw);
    }
    bool read_va(std::uint64_t address, void* to, std::uint64_t bytes) {
        std::uint64_t n = bytes;
        gpu::Buffer* b = nullptr;
        std::uint64_t off = 0;
        if (!context_.buffer_at(address, n, b, off) || !b || !b->cpu_pointer() || n < bytes) return false;
        std::memcpy(to, static_cast<const std::uint8_t*>(b->cpu_pointer()) + off, std::size_t(bytes));
        return true;
    }
    void trace_table(const Draw& draw) {
        static const char* offsets = std::getenv("GTA6RT_METAL_DRAW_TRACE_TABLE");
        if (!offsets) return;
        const std::uint64_t table = draw.user_data[8] & ~3u;
        std::string text;
        char line[160];
        std::snprintf(line, sizeof line, "user s0..s15:");
        text = line;
        for (int i = 0; i < 16; ++i) {
            std::snprintf(line, sizeof line, " %08x", draw.user_data[i]);
            text += line;
        }
        std::uint32_t words[64] = {};
        const bool ok = read_va(table, words, sizeof words);
        std::snprintf(line, sizeof line, "; table 0x%llx%s", static_cast<unsigned long long>(table), ok ? ":" : " (not read)");
        text += line;
        for (int i = 0; ok && i < 64; ++i) {
            std::snprintf(line, sizeof line, "%s%08x", i % 4 ? " " : " | ", words[i]);
            text += line;
        }
        std::fprintf(stderr, "[rt] d3d12-metal: draw trace table %s\n", text.c_str());
        for (const char* at = offsets; ok && *at;) {
            char* end = nullptr;
            const unsigned long o = std::strtoul(at, &end, 16);
            if (end == at) break;
            at = *end ? end + 1 : end;
            if (o + 16 > sizeof words) continue;
            gfx10::BufferWords w{words[o / 4], words[o / 4 + 1], words[o / 4 + 2], words[o / 4 + 3]};
            gfx10::Buffer v{};
            if (!gfx10::decode(w, v)) {
                std::fprintf(stderr, "[rt] d3d12-metal: draw trace table +0x%lx: not a V#\n", o);
                continue;
            }
            const std::uint64_t base = std::uint64_t(std::int64_t(v.address << 16) >> 16);
            std::uint32_t cb[32] = {};
            std::uint64_t n = sizeof cb;
            gpu::Buffer* holder = nullptr;
            std::uint64_t holder_offset = 0;
            const bool found = v.num_records && context_.buffer_at(base, n, holder, holder_offset) && holder;
            const bool read = found && holder->cpu_pointer() && n >= 16;
            if (read)
                std::memcpy(cb, static_cast<const std::uint8_t*>(holder->cpu_pointer()) + holder_offset,
                            std::size_t(std::min<std::uint64_t>(n, sizeof cb)));
            const auto floats = [&](const std::uint32_t* words, int count) {
                std::string s;
                for (int i = 0; i < count; ++i) {
                    float f;
                    std::memcpy(&f, &words[i], 4);
                    std::snprintf(line, sizeof line, "%s%g", i % 4 ? " " : " | ", f);
                    s += line;
                }
                return s;
            };
            text = read ? floats(cb, int(std::min<std::uint64_t>(n, sizeof cb) / 4)) : "";
            std::uint64_t lo = 0, hi = 0, alias = 0, room = 0;
            auto& aliases = title_va_aliases();
            const bool title = aliases.title_memory(base, lo, hi);
            const bool mapped = aliases.range_of(base, alias, room);
            std::fprintf(stderr,
                         "[rt] d3d12-metal: draw trace table +0x%lx: V# base 0x%llx stride %u records %u; replay finds %s "
                         "(%llu bytes at +0x%llx)%s; title memory %d [0x%llx, 0x%llx), alias %d -> 0x%llx (%llu)%s\n",
                         o, static_cast<unsigned long long>(base), v.stride, v.num_records,
                         found ? (holder == context_.arena_buffer() ? "the arena" : "a buffer") : "nothing",
                         static_cast<unsigned long long>(found ? n : 0), static_cast<unsigned long long>(holder_offset),
                         text.c_str(), int(title),
                         static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), int(mapped),
                         static_cast<unsigned long long>(alias), static_cast<unsigned long long>(room),
                         title && hi - base >= 128 && texture_memory_readable(std::uintptr_t(base), 128) ? (" title bytes:" + floats(reinterpret_cast<const std::uint32_t*>(
                                                                                  static_cast<std::uintptr_t>(base)), 32))
                                                         .c_str()
                                                   : "");
        }
    }

    struct Cached {
        MetalBinding b;
        bool ok = false;
    };
    const CommandList& list_;
    gpu::CommandList& out_;
    MetalContext& context_;
    gpu::QueueType queue_;
    std::vector<label::Write>* labels_;
    std::vector<std::unique_ptr<gpu::Buffer>>* temps_;
    std::unordered_map<ResourceId, Cached> bindings_;
    std::array<gpu::TargetView, 8> bound_rts_{};
    std::uint32_t bound_rt_count_ = ~0u;
    gpu::TargetView bound_depth_{};
    bool has_depth_ = false;
    bool rt_dirty_ = true;
    gpu::ComputePipeline* bound_compute_ = nullptr;
    std::uint32_t event_depth_ = 0;
};

}  // namespace

ReplayResult metal_replay(const CommandList& list, gpu::CommandList& out, MetalContext& context, gpu::QueueType queue,
                          std::vector<label::Write>* labels, std::vector<std::unique_ptr<gpu::Buffer>>* temps) {
    return Replay(list, out, context, queue, labels, temps).run();
}

}  // namespace leonida
