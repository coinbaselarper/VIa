use super::*;
use crate::bits::{BitWriter, Bits, Vlc};
use std::path::PathBuf;

const HEADER_GUID: [u8; 16] = [0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C];
const DATA_GUID: [u8; 16] = [0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C];
const FILE_PROPS_GUID: [u8; 16] = [0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65];
const STREAM_PROPS_GUID: [u8; 16] = [0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65];

fn samples_dir() -> PathBuf {
    if let Ok(p) = std::env::var("TOMBOX_FATE_SAMPLES") {
        return PathBuf::from(p);
    }
    PathBuf::from(r"D:\xbox-pc-port\emulator\third_party\fate-samples")
}

fn u16le(b: &[u8], o: usize) -> u32 {
    u16::from_le_bytes([b[o], b[o + 1]]) as u32
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

struct Asf {
    rate: u32,
    channels: usize,
    block_align: usize,
    extradata: Vec<u8>,
    payload: Vec<u8>,
}

fn read_var(b: &[u8], p: &mut usize, ty: u32) -> u32 {
    let v = match ty {
        1 => b[*p] as u32,
        2 => u16le(b, *p),
        3 => u32le(b, *p),
        _ => 0,
    };
    *p += [0, 1, 2, 4][ty as usize];
    v
}

fn parse_asf(f: &[u8]) -> Asf {
    assert_eq!(&f[..16], &HEADER_GUID);
    let hsize = u64le(f, 16) as usize;
    let mut p = 30;
    let mut packet_size = 0usize;
    let mut asf = Asf { rate: 0, channels: 0, block_align: 0, extradata: Vec::new(), payload: Vec::new() };
    let mut stream_no = 0u8;
    while p + 24 <= hsize {
        let g = &f[p..p + 16];
        let sz = u64le(f, p + 16) as usize;
        if g == FILE_PROPS_GUID {
            packet_size = u32le(f, p + 92) as usize;
        } else if g == STREAM_PROPS_GUID {
            let flags = u16le(f, p + 72);
            let w = p + 78;
            if u16le(f, w) == 0x162 {
                stream_no = (flags & 0x7f) as u8;
                asf.channels = u16le(f, w + 2) as usize;
                asf.rate = u32le(f, w + 4);
                asf.block_align = u16le(f, w + 12) as usize;
                let cb = u16le(f, w + 16) as usize;
                asf.extradata = f[w + 18..w + 18 + cb].to_vec();
            }
        }
        p += sz;
    }
    assert!(packet_size > 0 && asf.block_align > 0);
    assert_eq!(&f[hsize..hsize + 16], &DATA_GUID);
    let dsize = u64le(f, hsize + 16) as usize;
    let mut pk = hsize + 50;
    let end = (hsize + dsize).min(f.len());
    while pk + packet_size <= end {
        let b = &f[pk..pk + packet_size];
        let mut q = 0;
        if b[0] & 0x80 != 0 {
            q += 1 + (b[0] & 0x0f) as usize;
        }
        let ltf = b[q] as u32;
        let pf = b[q + 1] as u32;
        q += 2;
        let _plen = read_var(b, &mut q, (ltf >> 5) & 3);
        let _seq = read_var(b, &mut q, (ltf >> 1) & 3);
        let pad = read_var(b, &mut q, (ltf >> 3) & 3) as usize;
        q += 6;
        let multiple = ltf & 1 != 0;
        let (count, plt) = if multiple {
            let v = b[q] as u32;
            q += 1;
            (v & 0x3f, v >> 6)
        } else {
            (1, 0)
        };
        for _ in 0..count {
            let sn = b[q] & 0x7f;
            q += 1;
            read_var(b, &mut q, (pf >> 4) & 3);
            read_var(b, &mut q, (pf >> 2) & 3);
            let rlen = read_var(b, &mut q, pf & 3) as usize;
            let compressed = rlen == 1;
            q += rlen;
            if compressed {
                q += 1;
            }
            let len = if multiple { read_var(b, &mut q, plt) as usize } else { packet_size - q - pad };
            let data = &b[q..q + len];
            q += len;
            if sn != stream_no {
                continue;
            }
            if compressed {
                let mut s = 0;
                while s < data.len() {
                    let l = data[s] as usize;
                    asf.payload.extend_from_slice(&data[s + 1..s + 1 + l]);
                    s += 1 + l;
                }
            } else {
                asf.payload.extend_from_slice(data);
            }
        }
        pk += packet_size;
    }
    asf
}

#[test]
fn bit_reader_and_writer_roundtrip() {
    let mut w = BitWriter::default();
    w.put(3, 5);
    w.put(13, 0x1abc);
    w.put(1, 1);
    w.copy_from(&[0xde, 0xad, 0xbe], 0, 20);
    let mut r = Bits::new(w.buf.clone(), w.bits);
    assert_eq!(r.read(3), 5);
    assert_eq!(r.read(13), 0x1abc);
    assert_eq!(r.read1(), 1);
    assert_eq!(r.read(20), 0xdeadb);
    assert_eq!(r.read(8), 0);
    let mut s = Bits::new(vec![0xf0], 8);
    assert_eq!(s.sread(4), -1);
}

#[test]
fn vlc_canonical_codes() {
    let v = Vlc::from_lengths(&[1, 2, 3, 3], &[10, 11, 12, 13], 0);
    let mut r = Bits::new(vec![0b0101_1011, 0b1000_0000], 16);
    assert_eq!(v.decode(&mut r), 10);
    assert_eq!(v.decode(&mut r), 11);
    assert_eq!(v.decode(&mut r), 12);
    assert_eq!(v.decode(&mut r), 13);
    assert_eq!(v.decode(&mut r), 10);
}

#[test]
fn xma2_rejects_bad_config_and_accepts_valid() {
    assert!(Xma2Stream::new(48000, 3).is_err());
    assert!(Xma2Stream::new(48000, 0).is_err());
    for r in [24000, 32000, 44100, 48000] {
        for c in [1, 2] {
            assert!(Xma2Stream::new(r, c).is_ok());
        }
    }
}

#[test]
fn xma2_garbage_packet_is_safe() {
    let mut s = Xma2Stream::new(48000, 2).unwrap();
    let mut out = Vec::new();
    for seed in 0..32u32 {
        let mut pkt = vec![0u8; XMA_PACKET_BYTES];
        let mut x = seed.wrapping_mul(2654435761).wrapping_add(1);
        for b in pkt.iter_mut() {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            *b = x as u8;
        }
        s.decode_packet(&pkt, &mut out);
    }
    assert!(out.len() % 2 == 0);
}

#[test]
fn packet_header_fields() {
    let pkt = [0x10u8 << 2 | 0, 0, 0, 7];
    assert_eq!(packet_frame_count(&pkt), 0x10);
    assert_eq!(packet_skip_count(&pkt), 7);
}

#[test]
fn wmapro_fate_beethoven_matches_reference() {
    let dir = samples_dir().join("wmapro");
    let wma = std::fs::read(dir.join("Beethovens_9th-1_small.wma")).expect("fate sample");
    let reference = std::fs::read(dir.join("Beethovens_9th-1_small.pcm")).expect("fate reference");
    let asf = parse_asf(&wma);
    assert_eq!(asf.channels, 2);
    let mut dec = wmapro_from_extradata(asf.rate, asf.channels, asf.block_align, &asf.extradata).unwrap();
    let mut pcm: Vec<i16> = Vec::new();
    let mut frames_out = 0;
    for chunk in asf.payload.chunks(asf.block_align) {
        if chunk.len() < asf.block_align || frames_out >= 43 {
            break;
        }
        for f in dec.decode_packet(chunk) {
            if frames_out >= 43 {
                break;
            }
            for i in 0..f[0].len() {
                for c in 0..f.len() {
                    pcm.push(f32_to_s16(f[c][i]));
                }
            }
            frames_out += 1;
        }
    }
    let refs: Vec<i16> = reference.chunks_exact(2).map(|b| i16::from_le_bytes([b[0], b[1]])).collect();
    let n = pcm.len().min(refs.len());
    assert!(n > refs.len() * 9 / 10, "decoded {} of {} samples", pcm.len(), refs.len());
    let mut max_diff = 0i32;
    let mut energy = 0f64;
    let mut err = 0f64;
    for i in 0..n {
        let d = (pcm[i] as i32 - refs[i] as i32).abs();
        max_diff = max_diff.max(d);
        energy += (refs[i] as f64).powi(2);
        err += (d as f64).powi(2);
    }
    assert!(energy > 0.0);
    let snr = 10.0 * (energy / err.max(1.0)).log10();
    eprintln!("wmapro fate: decoded {} ref {} max_diff {} snr {:.1} dB rms {:.0}", pcm.len(), refs.len(), max_diff, snr, (energy / n as f64).sqrt());
    assert!(max_diff <= 2, "max diff {max_diff}, snr {snr:.1} dB over {n} samples");
}

#[test]
#[ignore]
fn xma2_matches_ffmpeg_on_title_streams() {
    let dir = PathBuf::from(std::env::var("TOMBOX_XMA_REF").expect("TOMBOX_XMA_REF"));
    let mut checked = 0;
    for e in std::fs::read_dir(&dir).unwrap() {
        let p = e.unwrap().path();
        if p.extension().and_then(|x| x.to_str()) != Some("xma") {
            continue;
        }
        let name = p.file_stem().unwrap().to_str().unwrap().to_string();
        let ch: usize = if name.contains("_2ch_") { 2 } else { 1 };
        let rate: u32 = name.rsplit('_').next().unwrap().parse().unwrap();
        let data = std::fs::read(&p).unwrap();
        let reference: Vec<i16> = std::fs::read(p.with_extension("pcm")).unwrap().chunks_exact(2).map(|b| i16::from_le_bytes([b[0], b[1]])).collect();
        let mut s = Xma2Stream::new(rate, ch).unwrap();
        let mut out = Vec::new();
        for pkt in data.chunks(XMA_PACKET_BYTES) {
            s.decode_packet(pkt, &mut out);
        }
        let n = out.len().min(reference.len());
        let mut max_diff = 0;
        let mut energy = 0f64;
        let mut err = 0f64;
        for i in 0..n {
            let d = (out[i] as i32 - reference[i] as i32).abs();
            max_diff = max_diff.max(d);
            energy += (reference[i] as f64).powi(2);
            err += (d as f64).powi(2);
        }
        eprintln!("{name}: ours {} ref {} compared {n} max_diff {max_diff} snr {:.1} dB rms {:.0}", out.len(), reference.len(), 10.0 * (energy / err.max(1.0)).log10(), (energy / n.max(1) as f64).sqrt());
        assert!(n * 10 >= reference.len() * 9, "{name}: short output");
        assert!(max_diff <= 2, "{name}: max diff {max_diff}");
        let mut x = 0x2545_f491u32;
        let mut f = Xma2Stream::new(rate, ch).unwrap();
        let mut sink = Vec::new();
        for (k, pkt) in data.chunks(XMA_PACKET_BYTES).enumerate() {
            let mut q = pkt.to_vec();
            for _ in 0..(k % 7) {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                let b = x as usize % q.len();
                q[b] ^= 1 << (x >> 29);
            }
            if k % 5 == 3 {
                q.truncate(x as usize % q.len());
            }
            f.decode_packet(&q, &mut sink);
            sink.clear();
        }
        checked += 1;
    }
    assert!(checked > 0);
}
