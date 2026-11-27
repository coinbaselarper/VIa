// UCRT math imports (api-ms-win-crt-math). The host libm answers; only the Windows-specific classification helpers
// and the LLP64 `long` results (lroundf) are written out.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "shims.h"

namespace rt::crt {
namespace {

#define MATH_D1(n) double WINAPI w_##n(double x) { return ::n(x); }
#define MATH_D2(n) double WINAPI w_##n(double x, double y) { return ::n(x, y); }
#define MATH_F1(n) float WINAPI w_##n(float x) { return ::n(x); }
#define MATH_F2(n) float WINAPI w_##n(float x, float y) { return ::n(x, y); }

MATH_D1(acos) MATH_D1(asin) MATH_D1(atan) MATH_D2(atan2) MATH_D1(cos) MATH_D1(exp) MATH_D1(exp2) MATH_D2(fmod)
MATH_D1(log) MATH_D1(log2) MATH_D2(pow) MATH_D1(sin) MATH_D1(tan)
MATH_F1(acosf) MATH_F1(asinf) MATH_F1(atanf) MATH_F2(atan2f) MATH_F1(cbrtf) MATH_F1(ceilf) MATH_F1(cosf) MATH_F1(erff)
MATH_F1(exp2f) MATH_F1(expf) MATH_F2(fmodf) MATH_F1(log10f) MATH_F1(log2f) MATH_F1(logf) MATH_F2(powf)
MATH_F2(remainderf) MATH_F1(sinf) MATH_F1(sinhf) MATH_F1(tanf)

double WINAPI w_modf(double x, double* ip) { return ::modf(x, ip); }
float WINAPI w_modff(float x, float* ip) { return ::modff(x, ip); }
float WINAPI w_nextafterf(float x, float y) { return ::nextafterf(x, y); }
// Windows long double is 64-bit, so the second argument arrives as a double.
float WINAPI w_nexttowardf(float x, double y) {
    if (std::isnan(x) || std::isnan(y)) return x + static_cast<float>(y);
    if (static_cast<double>(x) == y) return static_cast<float>(y);
    return ::nextafterf(x, y > x ? INFINITY : -INFINITY);
}

int32_t WINAPI w_lroundf(float x) {
    const long long r = ::llroundf(x);
    if (r > INT32_MAX) return INT32_MAX;
    if (r < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(r);
}

constexpr int16_t kDenorm = -2, kFinite = -1, kInf = 1, kNan = 2;

int16_t classify(int c) {
    switch (c) {
    case FP_NAN: return kNan;
    case FP_INFINITE: return kInf;
    case FP_SUBNORMAL: return kDenorm;
    case FP_ZERO: return 0;
    default: return kFinite;
    }
}
int16_t WINAPI w_dtest(const double* p) { return classify(std::fpclassify(*p)); }
int16_t WINAPI w_fdtest(const float* p) { return classify(std::fpclassify(*p)); }
int16_t WINAPI w_fdsign(float x) { return std::signbit(x) ? static_cast<int16_t>(0x8000) : 0; }
int16_t WINAPI w_dsign(double x) { return std::signbit(x) ? static_cast<int16_t>(0x8000) : 0; }

void WINAPI w_setusermatherr(void* handler) { (void)handler; }

}  // namespace
}  // namespace rt::crt

namespace {
using namespace rt::crt;
#define MR(n) SHIM("ucrtbase.dll", #n, w_##n)
}

SHIM_TABLE(crt_math,
    MR(acos), MR(asin), MR(atan), MR(atan2), MR(cos), MR(exp), MR(exp2), MR(fmod), MR(log), MR(log2), MR(pow), MR(sin),
    MR(tan), MR(acosf), MR(asinf), MR(atanf), MR(atan2f), MR(cbrtf), MR(ceilf), MR(cosf), MR(erff), MR(exp2f), MR(expf),
    MR(fmodf), MR(log10f), MR(log2f), MR(logf), MR(powf), MR(remainderf), MR(sinf), MR(sinhf), MR(tanf), MR(modf),
    MR(modff), MR(nextafterf), MR(nexttowardf), MR(lroundf),
    SHIM("ucrtbase.dll", "_dtest", w_dtest),
    SHIM("ucrtbase.dll", "_fdtest", w_fdtest),
    SHIM("ucrtbase.dll", "_fdsign", w_fdsign),
    SHIM("ucrtbase.dll", "_dsign", w_dsign),
    SHIM("ucrtbase.dll", "__setusermatherr", w_setusermatherr),
);
