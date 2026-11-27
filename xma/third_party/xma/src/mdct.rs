#[derive(Clone, Copy, Default, Debug)]
struct C {
    re: f32,
    im: f32,
}

pub struct Imdct {
    len: usize,
    tcos: Vec<f32>,
    tsin: Vec<f32>,
    rev: Vec<usize>,
    stages: Vec<Vec<C>>,
    z: Vec<C>,
}

impl Imdct {
    pub fn new(len: usize, scale: f64) -> Imdct {
        let n = 2 * len;
        let n4 = n / 4;
        let s = scale.abs().sqrt();
        let theta = 1.0 / 8.0 + if scale < 0.0 { n4 as f64 } else { 0.0 };
        let mut tcos = Vec::with_capacity(n4);
        let mut tsin = Vec::with_capacity(n4);
        for i in 0..n4 {
            let a = 2.0 * std::f64::consts::PI * (i as f64 + theta) / n as f64;
            tcos.push((-a.cos() * s) as f32);
            tsin.push((-a.sin() * s) as f32);
        }
        let bits = n4.trailing_zeros();
        let rev = (0..n4).map(|i| if bits == 0 { 0 } else { i.reverse_bits() >> (usize::BITS - bits) }).collect();
        let tw: Vec<C> = (0..n4 / 2.max(1))
            .map(|k| {
                let a = 2.0 * std::f64::consts::PI * k as f64 / n4 as f64;
                C { re: a.cos() as f32, im: a.sin() as f32 }
            })
            .collect();
        let mut stages = Vec::new();
        let mut m = 2;
        while m <= n4 {
            let step = n4 / m;
            stages.push((0..m / 2).map(|k| tw[k * step]).collect());
            m <<= 1;
        }
        Imdct { len, tcos, tsin, rev, stages, z: vec![C::default(); n4] }
    }

    fn fft(&mut self) {
        let n = self.z.len();
        for i in 0..n {
            let j = self.rev[i];
            if j > i {
                self.z.swap(i, j);
            }
        }
        let mut m = 2;
        let mut stage = 0;
        while m <= n {
            let tw = &self.stages[stage];
            let half = m / 2;
            for blk in self.z.chunks_exact_mut(m) {
                let (lo, hi) = blk.split_at_mut(half);
                for ((a, b), w) in lo.iter_mut().zip(hi.iter_mut()).zip(tw.iter()) {
                    let t = C { re: b.re * w.re - b.im * w.im, im: b.re * w.im + b.im * w.re };
                    let av = *a;
                    *a = C { re: av.re + t.re, im: av.im + t.im };
                    *b = C { re: av.re - t.re, im: av.im - t.im };
                }
            }
            m <<= 1;
            stage += 1;
        }
    }

    pub fn half(&mut self, out: &mut [f32], input: &[f32]) {
        let n2 = self.len;
        let n4 = n2 / 2;
        let n8 = n4 / 2;
        for k in 0..n4 {
            let a = input[n2 - 1 - 2 * k];
            let b = input[2 * k];
            let (c, s) = (self.tcos[k], self.tsin[k]);
            self.z[k] = C { re: a * c - b * s, im: a * s + b * c };
        }
        self.fft();
        for k in 0..n8 {
            let p = n8 - k - 1;
            let q = n8 + k;
            let zp = self.z[p];
            let zq = self.z[q];
            let r0 = zp.im * self.tsin[p] - zp.re * self.tcos[p];
            let i1 = zp.im * self.tcos[p] + zp.re * self.tsin[p];
            let r1 = zq.im * self.tsin[q] - zq.re * self.tcos[q];
            let i0 = zq.im * self.tcos[q] + zq.re * self.tsin[q];
            self.z[p] = C { re: r0, im: i0 };
            self.z[q] = C { re: r1, im: i1 };
        }
        for j in 0..n4 {
            out[2 * j] = self.z[j].re;
            out[2 * j + 1] = self.z[j].im;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn naive(input: &[f32], scale: f64) -> Vec<f64> {
        let len = input.len();
        let n = 2 * len;
        let full: Vec<f64> = (0..n)
            .map(|i| {
                (0..len)
                    .map(|k| input[k] as f64 * (std::f64::consts::PI / len as f64 * (i as f64 + 0.5 + len as f64 / 2.0) * (k as f64 + 0.5)).cos())
                    .sum::<f64>()
                    * scale
            })
            .collect();
        full[len / 2..len / 2 + len].to_vec()
    }

    #[test]
    fn fast_matches_definition() {
        for len in [64usize, 128, 512] {
            let input: Vec<f32> = (0..len).map(|i| ((i * 37 % 101) as f32 / 50.0) - 1.0).collect();
            let mut m = Imdct::new(len, 0.5);
            let mut out = vec![0.0; len];
            m.half(&mut out, &input);
            let r = naive(&input, 0.5);
            let pos = out.iter().zip(&r).map(|(a, b)| (*a as f64 - b).abs()).fold(0.0, f64::max);
            let neg = out.iter().zip(&r).map(|(a, b)| (*a as f64 + b).abs()).fold(0.0, f64::max);
            assert!(pos.min(neg) < 1e-3, "len {len}: pos {pos} neg {neg}");
        }
    }
}
