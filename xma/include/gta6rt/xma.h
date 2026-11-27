#ifndef GTA6RT_XMA_H
#define GTA6RT_XMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTA6RT_XMA_API_VERSION 1

/* Frame decoder (rust/src/lib.rs over the vendored xma crate, a port of FFmpeg's wmaprodec). */
typedef struct Gta6rtXmaDecoder Gta6rtXmaDecoder;

enum {
  GTA6RT_XMA_OK = 0,
  GTA6RT_XMA_INVALID_ARGUMENT = 1,
  GTA6RT_XMA_INVALID_FORMAT = 2,
  GTA6RT_XMA_OUTPUT_TOO_SMALL = 3,
  GTA6RT_XMA_DECODE_ERROR = 4,
  GTA6RT_XMA_PANIC = 5,
};

int32_t gta6rt_xma_decoder_create(uint32_t sample_rate, uint32_t channels, Gta6rtXmaDecoder** out);
void gta6rt_xma_decoder_destroy(Gta6rtXmaDecoder* decoder);
int32_t gta6rt_xma_decoder_reset(Gta6rtXmaDecoder* decoder);
/* One 2048-byte XMA2 packet in, `*frames` frames of 512 interleaved s16 samples per channel out. */
int32_t gta6rt_xma_decoder_packet(Gta6rtXmaDecoder* decoder, const uint8_t* packet, int16_t* out,
                                  size_t out_samples, uint32_t* frames);
size_t gta6rt_xma_decoder_state_len(const Gta6rtXmaDecoder* decoder);
int32_t gta6rt_xma_decoder_export(const Gta6rtXmaDecoder* decoder, float* state, size_t len);
int32_t gta6rt_xma_decoder_import(Gta6rtXmaDecoder* decoder, const float* state, size_t len);

/* SHAPE XMA context engine: decodes the 64-byte contexts the title owns, the way the console's XMA block does
   (frame-accurate read offsets, loop points and subframe skips, two input buffers, a 256-byte block output ring,
   decoder state carried through the context's work buffer). */

/* Translates an APU address range to host memory, or returns NULL. */
typedef uint8_t* (*gta6rt_xma_span_fn)(void* user, uint32_t apu_address, size_t bytes);

/* 1 when the engine should replace the silent path: GTA6RT_XMA=1, or GTA6RT_XMA unset and GTA6RT_AUDIO is on or
   null. 0 otherwise (GTA6RT_XMA=0, or GTA6RT_AUDIO unset/off). */
int gta6rt_xma_wanted(void);

/* Binds the title's context array (count contexts of 16 words). Starts no thread until a voice is enabled. */
int gta6rt_xma_bind(uint32_t* context_words, uint32_t count, gta6rt_xma_span_fn span, void* user);
/* Stops the workers and forgets the array. Safe to call when nothing is bound. */
void gta6rt_xma_unbind(void);
/* AcpHal SubmitCommand payloads: 5 ENABLE_XMA, 6 ENABLE_XMAS, 7 DISABLE_XMA, 8 DISABLE_XMAS, 18 ADVANCE_XMA_READ.
   Returns 1 when the command is an XMA command (handled here), 0 otherwise. */
int gta6rt_xma_command(uint32_t command, const void* config);

/* Activity log for the silent path (no decode): with GTA6RT_XMA_LOG=1 reports the XMA commands the title submits
   and the contexts it enables; does nothing otherwise. `context_words` may be NULL. */
void gta6rt_xma_observe(uint32_t command, const void* config, const uint32_t* context_words, uint32_t count);

struct gta6rt_xma_stats {
  uint64_t enables;
  uint64_t disables;
  uint64_t advances;
  uint64_t frames;
  uint64_t samples_out;
  uint64_t loops;
  uint64_t decode_errors;
  uint64_t state_imports;
  uint64_t starved;
  uint64_t decode_ns;
  uint32_t active;
  uint32_t peak_active;
  uint32_t workers;
};
void gta6rt_xma_get_stats(struct gta6rt_xma_stats* out);

#ifdef __cplusplus
}
#endif

#endif
