#[derive(Default, Clone)]
pub struct Bits {
    pub buf: Vec<u8>,
    pub pos: usize,
    pub size: usize,
}

impl Bits {
    pub fn new(buf: Vec<u8>, size_bits: usize) -> Bits {
        Bits { buf, pos: 0, size: size_bits }
    }

    fn bit_at(&self, p: usize) -> u32 {
        if p >= self.size {
            return 0;
        }
        match self.buf.get(p >> 3) {
            Some(b) => ((b >> (7 - (p & 7))) & 1) as u32,
            None => 0,
        }
    }

    pub fn peek(&self, n: u32) -> u32 {
        if n == 0 {
            return 0;
        }
        let byte = self.pos >> 3;
        let mut w = [0u8; 8];
        if byte < self.buf.len() {
            let avail = (self.buf.len() - byte).min(8);
            w[..avail].copy_from_slice(&self.buf[byte..byte + avail]);
        }
        let mut v = u64::from_be_bytes(w) << (self.pos & 7);
        if self.pos + n as usize > self.size {
            let valid = self.size.saturating_sub(self.pos);
            if valid == 0 {
                return 0;
            }
            v &= u64::MAX << (64 - valid);
        }
        (v >> (64 - n)) as u32
    }

    pub fn read(&mut self, n: u32) -> u32 {
        if n == 0 {
            return 0;
        }
        let v = self.peek(n);
        self.pos += n as usize;
        v
    }

    pub fn read1(&mut self) -> u32 {
        let v = self.bit_at(self.pos);
        self.pos += 1;
        v
    }

    pub fn sread(&mut self, n: u32) -> i32 {
        let v = self.read(n);
        ((v << (32 - n)) as i32) >> (32 - n)
    }

    pub fn skip(&mut self, n: i64) {
        if n > 0 {
            self.pos += n as usize;
        }
    }

    pub fn count(&self) -> i64 {
        self.pos as i64
    }

    pub fn left(&self) -> i64 {
        self.size as i64 - self.pos as i64
    }
}

#[derive(Default)]
pub struct BitWriter {
    pub buf: Vec<u8>,
    pub bits: usize,
}

impl BitWriter {
    pub fn reset(&mut self) {
        self.buf.clear();
        self.bits = 0;
    }

    pub fn put(&mut self, n: u32, v: u32) {
        let mut n = n;
        while n > 0 {
            let used = (self.bits & 7) as u32;
            if used == 0 {
                self.buf.push(0);
            }
            let room = 8 - used;
            let take = room.min(n);
            let chunk = (v >> (n - take)) & ((1u32 << take) - 1);
            let last = self.buf.len() - 1;
            self.buf[last] |= (chunk << (room - take)) as u8;
            self.bits += take as usize;
            n -= take;
        }
    }

    pub fn copy_from(&mut self, src: &[u8], start_byte: usize, nbits: usize) {
        let mut done = 0;
        let mut i = start_byte;
        while done + 8 <= nbits {
            let b = src.get(i).copied().unwrap_or(0) as u32;
            if self.bits & 7 == 0 {
                self.buf.push(b as u8);
                self.bits += 8;
            } else {
                self.put(8, b);
            }
            i += 1;
            done += 8;
        }
        let rem = nbits - done;
        if rem > 0 {
            let b = src.get(i).copied().unwrap_or(0) as u32;
            self.put(rem as u32, b >> (8 - rem));
        }
    }
}

const VLC_BITS: u32 = 10;

pub struct Vlc {
    codes: Vec<(u64, u32, i32)>,
    fast: Vec<(i32, u8)>,
}

impl Vlc {
    pub fn from_lengths(lens: &[u8], syms: &[i32], offset: i32) -> Vlc {
        let mut code: u64 = 0;
        let mut codes = Vec::with_capacity(lens.len());
        for (i, &l) in lens.iter().enumerate() {
            let len = l as u32;
            if len == 0 {
                continue;
            }
            codes.push((code, len, syms[i] + offset));
            code += 1u64 << (32 - len);
        }
        let mut fast = vec![(0i32, 0u8); 1 << VLC_BITS];
        for &(c, len, sym) in &codes {
            if len > VLC_BITS {
                continue;
            }
            let first = (c >> (32 - VLC_BITS)) as usize;
            let span = 1usize << (VLC_BITS - len);
            for e in fast.iter_mut().skip(first).take(span) {
                *e = (sym, len as u8);
            }
        }
        Vlc { codes, fast }
    }

    pub fn decode(&self, gb: &mut Bits) -> i32 {
        let v = gb.peek(32) as u64;
        let (sym, len) = self.fast[(v >> (32 - VLC_BITS)) as usize];
        if len != 0 {
            gb.pos += len as usize;
            return sym;
        }
        let i = self.codes.partition_point(|c| c.0 <= v);
        if i == 0 {
            gb.skip(1);
            return -1;
        }
        let (code, len, sym) = self.codes[i - 1];
        if v - code < (1u64 << (32 - len)) {
            gb.skip(len as i64);
            sym
        } else {
            gb.skip(1);
            -1
        }
    }
}
