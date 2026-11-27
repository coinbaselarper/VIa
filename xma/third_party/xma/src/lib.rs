pub mod bits;
pub mod mdct;
pub mod tables;
pub mod wmapro;

pub use wmapro::{Codec, Config, InitError, WmaPro};

pub const XMA_PACKET_BYTES: usize = 2048;
pub const XMA_FRAME_SAMPLES: usize = 512;
pub const XMA_START_SKIP: usize = 64;

pub fn f32_to_s16(v: f32) -> i16 {
    let s = (v * 32768.0).round();
    s.clamp(-32768.0, 32767.0) as i16
}

pub fn packet_frame_count(packet: &[u8]) -> u32 {
    packet.first().map(|b| (b >> 2) as u32).unwrap_or(0)
}

pub fn packet_skip_count(packet: &[u8]) -> u32 {
    packet.get(3).copied().unwrap_or(0) as u32
}

pub fn wmapro_from_extradata(sample_rate: u32, channels: usize, block_align: usize, extradata: &[u8]) -> Result<WmaPro, InitError> {
    if extradata.len() < 18 {
        return Err(InitError::Invalid("extradata"));
    }
    let bps = u16::from_le_bytes([extradata[0], extradata[1]]) as u32;
    let mask = u32::from_le_bytes([extradata[2], extradata[3], extradata[4], extradata[5]]);
    let flags = u16::from_le_bytes([extradata[14], extradata[15]]) as u32;
    let ch = if mask != 0 { mask.count_ones() as usize } else { channels };
    let mut d = WmaPro::new(&Config { codec: Codec::WmaPro, sample_rate, channels: ch, block_align, decode_flags: flags, bits_per_sample: bps })?;
    d.set_lfe_from_mask(mask);
    Ok(d)
}

pub struct Xma2Stream {
    dec: WmaPro,
    skip: usize,
    channels: usize,
}

impl Xma2Stream {
    pub fn new(sample_rate: u32, channels: usize) -> Result<Xma2Stream, InitError> {
        let dec = WmaPro::new(&Config {
            codec: Codec::Xma2,
            sample_rate,
            channels,
            block_align: XMA_PACKET_BYTES,
            decode_flags: 0x10d6,
            bits_per_sample: 16,
        })?;
        Ok(Xma2Stream { dec, skip: XMA_START_SKIP, channels })
    }

    pub fn new_hardware(sample_rate: u32, channels: usize) -> Result<Xma2Stream, InitError> {
        let mut s = Xma2Stream::new(sample_rate, channels)?;
        s.dec.set_skip_first_frame(false);
        s.skip = 0;
        Ok(s)
    }

    pub fn channels(&self) -> usize {
        self.channels
    }

    pub fn decode_packet_planar(&mut self, packet: &[u8]) -> Vec<Vec<Vec<f32>>> {
        let n = packet.len().min(XMA_PACKET_BYTES);
        self.dec.packet_done = true;
        self.dec.decode_packet(&packet[..n])
    }

    pub fn decode_packet(&mut self, packet: &[u8], out: &mut Vec<i16>) -> usize {
        let frames = self.decode_packet_planar(packet);
        let mut produced = 0;
        for f in frames {
            let len = f[0].len();
            let start = self.skip.min(len);
            self.skip -= start;
            for i in start..len {
                for c in 0..self.channels {
                    out.push(f32_to_s16(f[c][i]));
                }
                produced += 1;
            }
        }
        produced
    }
}

#[cfg(test)]
mod tests;
