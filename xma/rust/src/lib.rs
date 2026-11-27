//! C ABI over the vendored `xma` crate for port/xma: one decoder per SHAPE XMA context, fed one 2048-byte XMA2
//! packet at a time (the engine builds a single-frame packet per frame), output as interleaved s16 into a buffer
//! owned by the caller. No allocation crosses the boundary and no panic leaves it.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;

use xma::wmapro::{Codec, Config, WmaPro};
use xma::{f32_to_s16, XMA_FRAME_SAMPLES, XMA_PACKET_BYTES};

pub const GTA6RT_XMA_OK: i32 = 0;
pub const GTA6RT_XMA_INVALID_ARGUMENT: i32 = 1;
pub const GTA6RT_XMA_INVALID_FORMAT: i32 = 2;
pub const GTA6RT_XMA_OUTPUT_TOO_SMALL: i32 = 3;
pub const GTA6RT_XMA_DECODE_ERROR: i32 = 4;
pub const GTA6RT_XMA_PANIC: i32 = 5;

pub struct Gta6rtXmaDecoder {
    dec: WmaPro,
    channels: usize,
    sample_rate: u32,
}

fn make(sample_rate: u32, channels: usize) -> Option<WmaPro> {
    let mut dec = WmaPro::new(&Config {
        codec: Codec::Xma2,
        sample_rate,
        channels,
        block_align: XMA_PACKET_BYTES,
        decode_flags: 0x10d6,
        bits_per_sample: 16,
    })
    .ok()?;
    dec.set_skip_first_frame(false);
    Some(dec)
}

/// # Safety
/// `out` must be writable for one pointer.
#[no_mangle]
pub unsafe extern "C" fn gta6rt_xma_decoder_create(sample_rate: u32, channels: u32, out: *mut *mut Gta6rtXmaDecoder) -> i32 {
    if out.is_null() {
        return GTA6RT_XMA_INVALID_ARGUMENT;
    }
    unsafe { *out = ptr::null_mut() };
    if sample_rate == 0 || !(channels == 1 || channels == 2) {
        return GTA6RT_XMA_INVALID_FORMAT;
    }
    match catch_unwind(|| make(sample_rate, channels as usize)) {
        Ok(Some(dec)) => {
            let boxed = Box::new(Gta6rtXmaDecoder { dec, channels: channels as usize, sample_rate });
            unsafe { *out = Box::into_raw(boxed) };
            GTA6RT_XMA_OK
        }
        Ok(None) => GTA6RT_XMA_INVALID_FORMAT,
        Err(_) => GTA6RT_XMA_PANIC,
    }
}

/// # Safety
/// `decoder` is null or a live handle not in use by another call.
#[no_mangle]
pub unsafe extern "C" fn gta6rt_xma_decoder_destroy(decoder: *mut Gta6rtXmaDecoder) {
    if !decoder.is_null() {
        drop(unsafe { Box::from_raw(decoder) });
    }
}

/// Back to the state of a new decoder (no overlap, no saved bits).
/// # Safety
/// `decoder` is a live handle not in use by another call.
#[no_mangle]
pub unsafe extern "C" fn gta6rt_xma_decoder_reset(decoder: *mut Gta6rtXmaDecoder) -> i32 {
    let Some(d) = (unsafe { decoder.as_mut() }) else { return GTA6RT_XMA_INVALID_ARGUMENT };
    match catch_unwind(AssertUnwindSafe(|| make(d.sample_rate, d.channels))) {
        Ok(Some(dec)) => {
            d.dec = dec;
            GTA6RT_XMA_OK
        }
        Ok(None) => GTA6RT_XMA_INVALID_FORMAT,
        Err(_) => GTA6RT_XMA_PANIC,
    }
}

/// Decodes one 2048-byte XMA2 packet. Writes `*frames` frames of 512 interleaved s16 samples per channel into
/// `out` (capacity `out_samples` s16 values). A frame that does not fit is an error, never truncated.
/// # Safety
/// `decoder` is a live handle not in use by another call; `packet` holds 2048 readable bytes; `out` holds
/// `out_samples` writable s16; `frames` is writable.
#[no_mangle]
pub unsafe extern "C" fn gta6rt_xma_decoder_packet(
    decoder: *mut Gta6rtXmaDecoder,
    packet: *const u8,
    out: *mut i16,
    out_samples: usize,
    frames: *mut u32,
) -> i32 {
    if frames.is_null() {
        return GTA6RT_XMA_INVALID_ARGUMENT;
    }
    unsafe { *frames = 0 };
    let Some(d) = (unsafe { decoder.as_mut() }) else { return GTA6RT_XMA_INVALID_ARGUMENT };
    if packet.is_null() || (out.is_null() && out_samples != 0) {
        return GTA6RT_XMA_INVALID_ARGUMENT;
    }
    let input = unsafe { slice::from_raw_parts(packet, XMA_PACKET_BYTES) };
    let result = catch_unwind(AssertUnwindSafe(|| {
        d.dec.packet_done = true;
        d.dec.decode_packet(input)
    }));
    let decoded = match result {
        Ok(f) => f,
        Err(_) => return GTA6RT_XMA_PANIC,
    };
    let ch = d.channels;
    let need = decoded.len() * XMA_FRAME_SAMPLES * ch;
    if need > out_samples {
        return GTA6RT_XMA_OUTPUT_TOO_SMALL;
    }
    let dst = unsafe { slice::from_raw_parts_mut(out, out_samples) };
    let mut at = 0;
    for frame in &decoded {
        if frame.len() != ch || frame.iter().any(|c| c.len() != XMA_FRAME_SAMPLES) {
            return GTA6RT_XMA_DECODE_ERROR;
        }
        for i in 0..XMA_FRAME_SAMPLES {
            for c in 0..ch {
                dst[at] = f32_to_s16(frame[c][i]);
                at += 1;
            }
        }
    }
    unsafe { *frames = decoded.len() as u32 };
    GTA6RT_XMA_OK
}

/// Number of f32 values in an exported state.
/// # Safety
/// `decoder` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn gta6rt_xma_decoder_state_len(decoder: *const Gta6rtXmaDecoder) -> usize {
    unsafe { decoder.as_ref() }.map_or(0, |d| d.dec.state_len())
}

/// # Safety
/// `decoder` is a live handle; `state` holds `len` writable f32.
#[no_mangle]
pub unsafe extern "C" fn gta6rt_xma_decoder_export(decoder: *const Gta6rtXmaDecoder, state: *mut f32, len: usize) -> i32 {
    let Some(d) = (unsafe { decoder.as_ref() }) else { return GTA6RT_XMA_INVALID_ARGUMENT };
    if state.is_null() {
        return GTA6RT_XMA_INVALID_ARGUMENT;
    }
    let s = unsafe { slice::from_raw_parts_mut(state, len) };
    if d.dec.export_state(s) { GTA6RT_XMA_OK } else { GTA6RT_XMA_OUTPUT_TOO_SMALL }
}

/// # Safety
/// `decoder` is a live handle not in use by another call; `state` holds `len` readable f32.
#[no_mangle]
pub unsafe extern "C" fn gta6rt_xma_decoder_import(decoder: *mut Gta6rtXmaDecoder, state: *const f32, len: usize) -> i32 {
    let Some(d) = (unsafe { decoder.as_mut() }) else { return GTA6RT_XMA_INVALID_ARGUMENT };
    if state.is_null() {
        return GTA6RT_XMA_INVALID_ARGUMENT;
    }
    let s = unsafe { slice::from_raw_parts(state, len) };
    if d.dec.import_state(s) { GTA6RT_XMA_OK } else { GTA6RT_XMA_INVALID_FORMAT }
}
