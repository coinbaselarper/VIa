// Guest-facing XAudio2 2.9 COM objects: IXAudio2 (+ IXAudio2Extension), IXAudio2SourceVoice, IXAudio2SubmixVoice,
// IXAudio2MasteringVoice. Vtable order is xaudio2.h's. The work is done by Engine (xaudio2_engine.cpp).
#include "media_common.h"
#include "media_exports.h"
#include "xaudio2_engine.h"

#include <atomic>
#include <cstring>

namespace rt::xa2 {
namespace {

using namespace rt::media;
#define API GTA6RT_GUEST_ABI

std::atomic<bool> g_logged_flags{false}, g_logged_debug{false}, g_logged_effect_chain{false};

struct GuestEngine {
    const void* const* vtbl;
    const void* const* ext_vtbl;
    std::atomic<uint32_t> refs;
    Engine* engine;
};

GuestEngine* from_ext(void* ext) {
    return reinterpret_cast<GuestEngine*>(static_cast<uint8_t*>(ext) - offsetof(GuestEngine, ext_vtbl));
}

Voice* V(GuestVoice* g) { return g->voice; }
Engine* E(GuestVoice* g) { return g->voice->engine; }

// ---- IXAudio2 ----
API HRESULT x_query_interface(GuestEngine* self, const Guid* riid, void** ppv) {
    if (!ppv) return kEPointer;
    *ppv = nullptr;
    if (!riid) return kEInvalidArg;
    if (*riid == kIidIUnknown || *riid == kIidIXAudio2) *ppv = self;
    else if (*riid == kIidIXAudio2Extension) *ppv = &self->ext_vtbl;
    else return kENoInterface;
    self->refs.fetch_add(1);
    return kSOk;
}
API uint32_t x_add_ref(GuestEngine* self) { return self->refs.fetch_add(1) + 1; }
API uint32_t x_release(GuestEngine* self) {
    const uint32_t n = self->refs.fetch_sub(1) - 1;
    if (n == 0) {
        delete self->engine;
        delete self;
    }
    return n;
}
API HRESULT x_register_for_callbacks(GuestEngine* self, uint64_t cb) { return self->engine->register_callback(cb); }
API void x_unregister_for_callbacks(GuestEngine* self, uint64_t cb) { self->engine->unregister_callback(cb); }

extern const void* const kSourceVtbl[];
extern const void* const kSubmixVtbl[];
extern const void* const kMasterVtbl[];

API HRESULT x_create_source_voice(GuestEngine* self, uint64_t* out, const WaveFormatEx* fmt, uint32_t flags,
                                  float max_ratio, uint64_t callback, const XAudio2VoiceSends* sends,
                                  const XAudio2EffectChain* chain) {
    if (!out) return kEInvalidArg;
    Voice* v = nullptr;
    const HRESULT h = self->engine->create_source(fmt, flags, max_ratio, callback, sends,
                                                  reinterpret_cast<uint64_t>(chain), &v);
    if (h < 0) {
        *out = 0;
        return h;
    }
    v->guest.vtbl = kSourceVtbl;
    *out = reinterpret_cast<uint64_t>(&v->guest);
    return kSOk;
}
API HRESULT x_create_submix_voice(GuestEngine* self, uint64_t* out, uint32_t channels, uint32_t rate, uint32_t flags,
                                  uint32_t stage, const XAudio2VoiceSends* sends, const XAudio2EffectChain* chain) {
    if (!out) return kEInvalidArg;
    Voice* v = nullptr;
    const HRESULT h =
        self->engine->create_submix(channels, rate, flags, stage, sends, reinterpret_cast<uint64_t>(chain), &v);
    if (h < 0) {
        *out = 0;
        return h;
    }
    v->guest.vtbl = kSubmixVtbl;
    *out = reinterpret_cast<uint64_t>(&v->guest);
    return kSOk;
}
API HRESULT x_create_mastering_voice(GuestEngine* self, uint64_t* out, uint32_t channels, uint32_t rate,
                                     uint32_t flags, const char16_t* device, const XAudio2EffectChain* chain,
                                     uint32_t category) {
    if (!out) return kEInvalidArg;
    Voice* v = nullptr;
    const HRESULT h = self->engine->create_mastering(channels, rate, flags, reinterpret_cast<uint64_t>(device),
                                                     reinterpret_cast<uint64_t>(chain), category, &v);
    if (h < 0) {
        *out = 0;
        return h;
    }
    v->guest.vtbl = kMasterVtbl;
    *out = reinterpret_cast<uint64_t>(&v->guest);
    return kSOk;
}
API HRESULT x_start_engine(GuestEngine* self) { return self->engine->start_engine(); }
API void x_stop_engine(GuestEngine* self) { self->engine->stop_engine(); }
API HRESULT x_commit_changes(GuestEngine* self, uint32_t op) { return self->engine->commit(op); }
API void x_get_performance_data(GuestEngine* self, XAudio2PerformanceData* out) {
    if (out) self->engine->performance(out);
}
API void x_set_debug_configuration(GuestEngine*, const XAudio2DebugConfiguration* cfg, void*) {
    if (cfg)
        MLOG_ONCE(g_logged_debug, INFO, "xaudio2", "SetDebugConfiguration(trace 0x%x, break 0x%x): no debug engine",
                  cfg->TraceMask, cfg->BreakMask);
}

const void* const kEngineVtbl[] = {
    reinterpret_cast<const void*>(&x_query_interface),
    reinterpret_cast<const void*>(&x_add_ref),
    reinterpret_cast<const void*>(&x_release),
    reinterpret_cast<const void*>(&x_register_for_callbacks),
    reinterpret_cast<const void*>(&x_unregister_for_callbacks),
    reinterpret_cast<const void*>(&x_create_source_voice),
    reinterpret_cast<const void*>(&x_create_submix_voice),
    reinterpret_cast<const void*>(&x_create_mastering_voice),
    reinterpret_cast<const void*>(&x_start_engine),
    reinterpret_cast<const void*>(&x_stop_engine),
    reinterpret_cast<const void*>(&x_commit_changes),
    reinterpret_cast<const void*>(&x_get_performance_data),
    reinterpret_cast<const void*>(&x_set_debug_configuration),
};
static_assert(sizeof(kEngineVtbl) / sizeof(void*) == 13);

// ---- IXAudio2Extension ----
API HRESULT e_query_interface(void* self, const Guid* riid, void** ppv) {
    return x_query_interface(from_ext(self), riid, ppv);
}
API uint32_t e_add_ref(void* self) { return x_add_ref(from_ext(self)); }
API uint32_t e_release(void* self) { return x_release(from_ext(self)); }
API void e_get_processing_quantum(void* self, uint32_t* num, uint32_t* den) {
    Engine* e = from_ext(self)->engine;
    if (num) *num = e->quantum();
    if (den) *den = e->engine_rate();
}
API void e_get_processor(void*, uint32_t* processor) {
    if (processor) *processor = 1;
}

const void* const kExtensionVtbl[] = {
    reinterpret_cast<const void*>(&e_query_interface),        reinterpret_cast<const void*>(&e_add_ref),
    reinterpret_cast<const void*>(&e_release),                reinterpret_cast<const void*>(&e_get_processing_quantum),
    reinterpret_cast<const void*>(&e_get_processor),
};

// ---- IXAudio2Voice ----
API void v_get_voice_details(GuestVoice* g, XAudio2VoiceDetails* out) {
    if (out) E(g)->details(V(g), out);
}
API HRESULT v_set_output_voices(GuestVoice* g, const XAudio2VoiceSends* sends) {
    return E(g)->set_output_voices(V(g), sends);
}
API HRESULT v_set_effect_chain(GuestVoice*, const XAudio2EffectChain* chain) {
    if (!chain || chain->EffectCount == 0) return kSOk;
    MLOG_ONCE(g_logged_effect_chain, ERROR, "xaudio2", "SetEffectChain with %u effects: XAPO effects are not implemented: E_NOTIMPL",
              chain->EffectCount);
    return kENotImpl;
}
API HRESULT v_enable_effect(GuestVoice*, uint32_t, uint32_t) { return kXAudio2EInvalidCall; }
API HRESULT v_disable_effect(GuestVoice*, uint32_t, uint32_t) { return kXAudio2EInvalidCall; }
API void v_get_effect_state(GuestVoice*, uint32_t, int32_t* enabled) {
    if (enabled) *enabled = 0;
}
API HRESULT v_set_effect_parameters(GuestVoice*, uint32_t, const void*, uint32_t, uint32_t) {
    return kXAudio2EInvalidCall;
}
API HRESULT v_get_effect_parameters(GuestVoice*, uint32_t, void*, uint32_t) { return kXAudio2EInvalidCall; }
API HRESULT v_set_filter_parameters(GuestVoice*, const XAudio2FilterParameters*, uint32_t) {
    return kXAudio2EInvalidCall;
}
API void v_get_filter_parameters(GuestVoice*, XAudio2FilterParameters* out) {
    if (out) *out = {0, 1.0f, 1.0f};
}
API HRESULT v_set_output_filter_parameters(GuestVoice*, uint64_t, const XAudio2FilterParameters*, uint32_t) {
    return kXAudio2EInvalidCall;
}
API void v_get_output_filter_parameters(GuestVoice*, uint64_t, XAudio2FilterParameters* out) {
    if (out) *out = {0, 1.0f, 1.0f};
}
API HRESULT v_set_volume(GuestVoice* g, float volume, uint32_t op) { return E(g)->set_volume(V(g), volume, op); }
API void v_get_volume(GuestVoice* g, float* out) {
    if (out) E(g)->get_volume(V(g), out);
}
API HRESULT v_set_channel_volumes(GuestVoice* g, uint32_t count, const float* volumes, uint32_t op) {
    return E(g)->set_channel_volumes(V(g), count, volumes, op);
}
API void v_get_channel_volumes(GuestVoice* g, uint32_t count, float* out) {
    E(g)->get_channel_volumes(V(g), count, out);
}
API HRESULT v_set_output_matrix(GuestVoice* g, uint64_t dest, uint32_t src, uint32_t dst, const float* m,
                                uint32_t op) {
    return E(g)->set_output_matrix(V(g), dest, src, dst, m, op);
}
API void v_get_output_matrix(GuestVoice* g, uint64_t dest, uint32_t src, uint32_t dst, float* m) {
    E(g)->get_output_matrix(V(g), dest, src, dst, m);
}
API void v_destroy_voice(GuestVoice* g) { E(g)->destroy_voice(V(g)); }

// ---- IXAudio2SourceVoice ----
API HRESULT s_start(GuestVoice* g, uint32_t flags, uint32_t op) { return E(g)->start(V(g), flags, op); }
API HRESULT s_stop(GuestVoice* g, uint32_t flags, uint32_t op) { return E(g)->stop(V(g), flags, op); }
API HRESULT s_submit_source_buffer(GuestVoice* g, const XAudio2Buffer* buf, const XAudio2BufferWma* wma) {
    return E(g)->submit(V(g), buf, wma);
}
API HRESULT s_flush_source_buffers(GuestVoice* g) { return E(g)->flush(V(g)); }
API HRESULT s_discontinuity(GuestVoice* g) { return E(g)->discontinuity(V(g)); }
API HRESULT s_exit_loop(GuestVoice* g, uint32_t op) { return E(g)->exit_loop(V(g), op); }
API void s_get_state(GuestVoice* g, XAudio2VoiceState* out, uint32_t flags) {
    if (out) E(g)->get_state(V(g), out, flags);
}
API HRESULT s_set_frequency_ratio(GuestVoice* g, float ratio, uint32_t op) {
    return E(g)->set_frequency_ratio(V(g), ratio, op);
}
API void s_get_frequency_ratio(GuestVoice* g, float* out) {
    if (out) E(g)->get_frequency_ratio(V(g), out);
}
API HRESULT s_set_source_sample_rate(GuestVoice* g, uint32_t rate) { return E(g)->set_source_sample_rate(V(g), rate); }

// ---- IXAudio2MasteringVoice ----
API HRESULT m_get_channel_mask(GuestVoice* g, uint32_t* mask) { return E(g)->get_channel_mask(V(g), mask); }

#define VOICE_METHODS                                                                                             \
    reinterpret_cast<const void*>(&v_get_voice_details), reinterpret_cast<const void*>(&v_set_output_voices),     \
        reinterpret_cast<const void*>(&v_set_effect_chain), reinterpret_cast<const void*>(&v_enable_effect),      \
        reinterpret_cast<const void*>(&v_disable_effect), reinterpret_cast<const void*>(&v_get_effect_state),     \
        reinterpret_cast<const void*>(&v_set_effect_parameters),                                                  \
        reinterpret_cast<const void*>(&v_get_effect_parameters),                                                  \
        reinterpret_cast<const void*>(&v_set_filter_parameters),                                                  \
        reinterpret_cast<const void*>(&v_get_filter_parameters),                                                  \
        reinterpret_cast<const void*>(&v_set_output_filter_parameters),                                           \
        reinterpret_cast<const void*>(&v_get_output_filter_parameters),                                           \
        reinterpret_cast<const void*>(&v_set_volume), reinterpret_cast<const void*>(&v_get_volume),               \
        reinterpret_cast<const void*>(&v_set_channel_volumes), reinterpret_cast<const void*>(&v_get_channel_volumes), \
        reinterpret_cast<const void*>(&v_set_output_matrix), reinterpret_cast<const void*>(&v_get_output_matrix), \
        reinterpret_cast<const void*>(&v_destroy_voice)

}  // namespace

namespace {
const void* const kSourceVtbl[] = {
    VOICE_METHODS,
    reinterpret_cast<const void*>(&s_start),
    reinterpret_cast<const void*>(&s_stop),
    reinterpret_cast<const void*>(&s_submit_source_buffer),
    reinterpret_cast<const void*>(&s_flush_source_buffers),
    reinterpret_cast<const void*>(&s_discontinuity),
    reinterpret_cast<const void*>(&s_exit_loop),
    reinterpret_cast<const void*>(&s_get_state),
    reinterpret_cast<const void*>(&s_set_frequency_ratio),
    reinterpret_cast<const void*>(&s_get_frequency_ratio),
    reinterpret_cast<const void*>(&s_set_source_sample_rate),
};
const void* const kSubmixVtbl[] = {VOICE_METHODS};
const void* const kMasterVtbl[] = {VOICE_METHODS, reinterpret_cast<const void*>(&m_get_channel_mask)};
static_assert(sizeof(kSourceVtbl) / sizeof(void*) == 29);
static_assert(sizeof(kSubmixVtbl) / sizeof(void*) == 19);
static_assert(sizeof(kMasterVtbl) / sizeof(void*) == 20);
}  // namespace

Engine* engine_of(uint64_t guest_ixaudio2) {
    return guest_ixaudio2 ? reinterpret_cast<GuestEngine*>(guest_ixaudio2)->engine : nullptr;
}

}  // namespace rt::xa2

namespace rt::media {

GTA6RT_GUEST_ABI HRESULT XAudio2Create(uint64_t* out, uint32_t flags, uint32_t processor) {
    if (!out) return kEInvalidArg;
    *out = 0;
    constexpr uint32_t known = kXa2DebugEngine | kXa2StopEngineWhenIdle | kXa21024Quantum | kXa2NoVirtualAudioClient;
    if (flags & ~known)
        MLOG_ONCE(rt::xa2::g_logged_flags, INFO, "xaudio2",
                  "XAudio2Create flags 0x%x: 0x%x not defined by the Windows 2.9 headers (console flags), accepted", flags,
                  flags & ~known);
    auto* g = new xa2::GuestEngine;
    g->vtbl = xa2::kEngineVtbl;
    g->ext_vtbl = xa2::kExtensionVtbl;
    g->refs.store(1);
    g->engine = new xa2::Engine(flags, processor, xa2::default_output_mode());
    *out = reinterpret_cast<uint64_t>(g);
    MLOG(INFO, "xaudio2", "XAudio2Create(flags 0x%x, processor 0x%x) -> engine %p", flags, processor,
         static_cast<void*>(g));
    return kSOk;
}

}  // namespace rt::media
