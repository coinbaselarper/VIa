#ifndef GTA6RT_AUDIO_OUT_H
#define GTA6RT_AUDIO_OUT_H

/* Host audio output for the title's render endpoint (silent_audio's IAudioClient and spatial stream).
   Plain C, no windows.h. Producers never block; the device callback never blocks or allocates. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTA6RT_AUDIO_OUT_API_VERSION 1

typedef enum gta6rt_audio_mode {
    GTA6RT_AUDIO_OFF = 0,  /* GTA6RT_AUDIO unset or "off": the caller keeps its silent path; nothing is opened */
    GTA6RT_AUDIO_ON = 1,   /* "on": default output device through CoreAudio (HAL output unit) */
    GTA6RT_AUDIO_NULL = 2, /* "null": no device; frames are consumed on the host clock (and still reach the WAV tap) */
} gta6rt_audio_mode;

typedef struct gta6rt_audio_stream gta6rt_audio_stream;

typedef struct gta6rt_audio_stream_desc {
    uint32_t sample_rate;   /* must be 48000 */
    uint32_t channels;      /* 1..8 */
    uint32_t channel_mask;  /* WAVE speaker mask; 0 = default for the channel count */
    uint32_t buffer_frames; /* ring capacity */
    const char* name;
} gta6rt_audio_stream_desc;

typedef struct gta6rt_audio_stats {
    uint64_t frames_written;
    uint64_t frames_consumed;
    uint64_t frames_dropped;   /* writes refused because the ring was full */
    uint64_t underruns;        /* device pulls that found fewer frames than asked, after the first write */
    uint64_t underrun_frames;
    uint64_t clocked_frames;   /* consumed by the host clock while no device pulled */
} gta6rt_audio_stats;

typedef struct gta6rt_audio_device_stats {
    uint64_t callbacks;
    uint64_t frames;
    uint64_t device_changes;
    uint64_t tap_frames;
    uint64_t tap_dropped;
    uint64_t limiter_frames;   /* frames in which the output limiter reduced gain */
    uint32_t device_channels;
    uint32_t device_rate;      /* nominal device rate; the output unit converts 48 kHz to it */
    uint32_t io_frames;
    uint32_t latency_frames;   /* device latency + safety offset + io buffer, at the device rate */
    double callback_p50_us, callback_p90_us, callback_p99_us, callback_p999_us, callback_max_us;
    double callback_interval_p50_us, callback_interval_p99_us, callback_interval_max_us;
    double callback_cpu_us_total; /* summed wall time inside the device callback */
    double service_cpu_us_total;  /* CPU time of the service thread (clock, tap writer, device changes) */
} gta6rt_audio_device_stats;

/* GTA6RT_AUDIO=off|on|null, read on every call (default off). */
gta6rt_audio_mode gta6rt_audio_out_mode(void);
const char* gta6rt_audio_mode_name(gta6rt_audio_mode);

/* Opens the shared output (device or null clock) on first use. Returns 0 on success, negative on failure. A failing
   device in mode on falls back to the null clock with a log line. */
int gta6rt_audio_stream_open(const gta6rt_audio_stream_desc* desc, gta6rt_audio_stream** out);
/* Never waits for the device: the stream is detached and freed by the service thread. */
void gta6rt_audio_stream_close(gta6rt_audio_stream* s);

int gta6rt_audio_stream_start(gta6rt_audio_stream* s);
int gta6rt_audio_stream_stop(gta6rt_audio_stream* s);
/* Drops queued frames and zeroes the counters; only while stopped. */
int gta6rt_audio_stream_reset(gta6rt_audio_stream* s);

uint32_t gta6rt_audio_stream_capacity(const gta6rt_audio_stream* s);
uint32_t gta6rt_audio_stream_padding(const gta6rt_audio_stream* s);
/* Interleaved float frames in the stream's layout. Returns the frames taken (fewer only when the ring is full). */
uint32_t gta6rt_audio_stream_write(gta6rt_audio_stream* s, const float* frames, uint32_t count);
/* Writes `count` frames of silence. */
uint32_t gta6rt_audio_stream_write_silence(gta6rt_audio_stream* s, uint32_t count);

/* Frames consumed by the output, interpolated within the last device pull so it is monotonic and sample-accurate;
   `host_ns` (optional) is the host monotonic time of the reading. */
uint64_t gta6rt_audio_stream_position(gta6rt_audio_stream* s, uint64_t* host_ns);
/* Output latency in stream frames (device + io buffer + safety offset). */
uint32_t gta6rt_audio_stream_latency(const gta6rt_audio_stream* s);

/* Linear gains 0..1. */
void gta6rt_audio_stream_set_volume(gta6rt_audio_stream* s, float volume, int mute);
void gta6rt_audio_stream_set_channel_volume(gta6rt_audio_stream* s, uint32_t channel, float volume);

/* For the caller's pacing thread (never the title's render thread): waits until the output consumed frames since
   the last call or `timeout_ms` passed. Returns 1 on progress, 0 on timeout. */
int gta6rt_audio_stream_wait_progress(gta6rt_audio_stream* s, uint32_t timeout_ms);

void gta6rt_audio_stream_stats(const gta6rt_audio_stream* s, gta6rt_audio_stats* out);
/* Sum over the open streams. */
void gta6rt_audio_out_streams_stats(gta6rt_audio_stats* out);
void gta6rt_audio_device_stats_get(gta6rt_audio_device_stats* out);

/* CPU time of the calling thread in microseconds (for callers' own accounting). */
double gta6rt_audio_out_thread_cpu_us(void);

/* Logs one summary line of the device and every open stream ("[audio_out] ..."). */
void gta6rt_audio_out_log_summary(const char* why);

/* Tests and tools. */
void gta6rt_audio_out_set_master(float gain, int mute);
/* Starts or replaces the WAV tap (32-bit float, device layout, before master gain). NULL stops it. */
int gta6rt_audio_out_set_tap(const char* path);
/* Simulates a device change: the output unit is rebuilt on the service thread. */
void gta6rt_audio_out_force_reconfigure(void);
/* Stops the shared output and frees retired streams (all streams must be closed). */
void gta6rt_audio_out_shutdown(void);

/* Opus packet decoder (AudioToolbox), for the console APU's Opus decode commands (xapu.dll). */
typedef struct gta6rt_opus_decoder gta6rt_opus_decoder;
int gta6rt_opus_decoder_create(uint32_t rate, uint32_t channels, gta6rt_opus_decoder** out);
/* One packet to interleaved float; returns the frames written (0 while the decoder primes) or a negative error. */
int gta6rt_opus_decode(gta6rt_opus_decoder* d, const void* packet, uint32_t bytes, float* out, uint32_t max_frames);
void gta6rt_opus_decoder_reset(gta6rt_opus_decoder* d);
void gta6rt_opus_decoder_destroy(gta6rt_opus_decoder* d);

#ifdef __cplusplus
}
#endif

#endif
