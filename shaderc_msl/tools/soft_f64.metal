static inline float spvF64ToF32(ulong x)
{
    uint s = uint(x >> 63) << 31;
    int e = int((x >> 52) & 0x7FFul);
    ulong m = x & 0xFFFFFFFFFFFFFul;
    if (e == 0x7FF)
        return as_type<float>(s | 0x7F800000u | (m != 0 ? (0x00400000u | uint(m >> 29)) : 0u));
    if (e == 0)
        return as_type<float>(s);
    int E = e - 1023;
    ulong M = m | (1ul << 52);
    int fe = E + 127;
    if (fe >= 255)
        return as_type<float>(s | 0x7F800000u);
    int shift = fe >= 1 ? 29 : 30 - fe;
    if (shift >= 54)
        return as_type<float>(s);
    ulong keep = M >> shift;
    ulong rem = M & ((1ul << shift) - 1ul);
    ulong hw = 1ul << (shift - 1);
    if (rem > hw || (rem == hw && (keep & 1ul) != 0))
        keep += 1;
    if (fe >= 1)
    {
        if ((keep >> 24) != 0)
        {
            keep >>= 1;
            fe += 1;
        }
        if (fe >= 255)
            return as_type<float>(s | 0x7F800000u);
        return as_type<float>(s | (uint(fe) << 23) | uint(keep & 0x7FFFFFul));
    }
    return as_type<float>(s | uint(keep));
}

struct spvSoftF64
{
    ulong v;
    explicit operator float() const { return spvF64ToF32(v); }
};

static inline bool spvF64IsNaN(ulong x)
{
    return (x & 0x7FFFFFFFFFFFFFFFul) > 0x7FF0000000000000ul;
}

static inline ulong spvF64Add(ulong a, ulong b)
{
    if (spvF64IsNaN(a))
        return a | 0x0008000000000000ul;
    if (spvF64IsNaN(b))
        return b | 0x0008000000000000ul;
    ulong ea = (a >> 52) & 0x7FFul, eb = (b >> 52) & 0x7FFul;
    ulong sa = a >> 63, sb = b >> 63;
    if (ea == 0x7FFul)
        return (eb == 0x7FFul && sa != sb) ? 0x7FF8000000000000ul : a;
    if (eb == 0x7FFul)
        return b;
    if ((a << 1) == 0 && (b << 1) == 0)
        return (sa & sb) << 63;
    if ((a << 1) == 0)
        return b;
    if ((b << 1) == 0)
        return a;
    ulong ma = a & 0xFFFFFFFFFFFFFul, mb = b & 0xFFFFFFFFFFFFFul;
    if (ea != 0)
        ma |= 1ul << 52;
    else
        ea = 1;
    if (eb != 0)
        mb |= 1ul << 52;
    else
        eb = 1;
    if (ea < eb || (ea == eb && ma < mb))
    {
        ulong t = ea; ea = eb; eb = t;
        t = ma; ma = mb; mb = t;
        t = sa; sa = sb; sb = t;
    }
    ulong d = ea - eb;
    ma <<= 3;
    mb <<= 3;
    if (d >= 64)
        mb = mb != 0 ? 1ul : 0ul;
    else if (d > 0)
        mb = (mb >> d) | ((mb & ((1ul << d) - 1ul)) != 0 ? 1ul : 0ul);
    ulong e = ea;
    ulong m;
    if (sa == sb)
    {
        m = ma + mb;
        if ((m >> 56) != 0)
        {
            m = (m >> 1) | (m & 1ul);
            e += 1;
        }
    }
    else
    {
        m = ma - mb;
        if (m == 0)
            return 0;
        ulong lz = clz(m);
        ulong want = lz > 8 ? lz - 8 : 0;
        ulong room = e - 1;
        ulong sh = want < room ? want : room;
        m <<= sh;
        e -= sh;
    }
    ulong low = m & 7ul;
    m >>= 3;
    if (low > 4 || (low == 4 && (m & 1ul) != 0))
    {
        m += 1;
        if ((m >> 53) != 0)
        {
            m >>= 1;
            e += 1;
        }
    }
    if (e >= 0x7FFul)
        return (sa << 63) | 0x7FF0000000000000ul;
    if ((m >> 52) == 0)
        e = 0;
    return (sa << 63) | (e << 52) | (m & 0xFFFFFFFFFFFFFul);
}

static inline long spvF64Key(ulong x)
{
    return (x >> 63) != 0 ? -long(x & 0x7FFFFFFFFFFFFFFFul) : long(x);
}

static inline spvSoftF64 spvSoftF64_from(uint2 x) { return spvSoftF64{ (ulong(x.y) << 32) | ulong(x.x) }; }
static inline spvSoftF64 spvSoftF64_from(ulong x) { return spvSoftF64{ x }; }
static inline uint2 spv_as_uint2(spvSoftF64 x) { return uint2(uint(x.v), uint(x.v >> 32)); }
template<typename T> static inline uint2 spv_as_uint2(T x) { return as_type<uint2>(x); }
static inline spvSoftF64 operator+(spvSoftF64 a, spvSoftF64 b) { return spvSoftF64{ spvF64Add(a.v, b.v) }; }
static inline spvSoftF64 operator-(spvSoftF64 a) { return spvSoftF64{ a.v ^ 0x8000000000000000ul }; }
static inline spvSoftF64 operator-(spvSoftF64 a, spvSoftF64 b) { return spvSoftF64{ spvF64Add(a.v, b.v ^ 0x8000000000000000ul) }; }
static inline bool operator<(spvSoftF64 a, spvSoftF64 b) { return !spvF64IsNaN(a.v) && !spvF64IsNaN(b.v) && spvF64Key(a.v) < spvF64Key(b.v); }
static inline bool operator>(spvSoftF64 a, spvSoftF64 b) { return b < a; }
static inline bool operator<=(spvSoftF64 a, spvSoftF64 b) { return !spvF64IsNaN(a.v) && !spvF64IsNaN(b.v) && spvF64Key(a.v) <= spvF64Key(b.v); }
static inline bool operator>=(spvSoftF64 a, spvSoftF64 b) { return b <= a; }
static inline bool operator==(spvSoftF64 a, spvSoftF64 b) { return !spvF64IsNaN(a.v) && !spvF64IsNaN(b.v) && spvF64Key(a.v) == spvF64Key(b.v); }
static inline bool operator!=(spvSoftF64 a, spvSoftF64 b) { return !(a == b); }
static inline spvSoftF64 spvFAdd(spvSoftF64 l, spvSoftF64 r) { return l + r; }
static inline spvSoftF64 spvFSub(spvSoftF64 l, spvSoftF64 r) { return l - r; }
static inline bool isnan(spvSoftF64 x) { return spvF64IsNaN(x.v); }
static inline bool isinf(spvSoftF64 x) { return (x.v & 0x7FFFFFFFFFFFFFFFul) == 0x7FF0000000000000ul; }
static inline bool isfinite(spvSoftF64 x) { return (x.v & 0x7FF0000000000000ul) != 0x7FF0000000000000ul; }
static inline spvSoftF64 abs(spvSoftF64 x) { return spvSoftF64{ x.v & 0x7FFFFFFFFFFFFFFFul }; }
