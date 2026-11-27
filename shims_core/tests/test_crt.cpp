#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "shim_call.h"
#include "test.h"

using tcall::call;

namespace {
constexpr const char* kU = "ucrtbase.dll";
}

TEST(crt_string_basics) {
    CHECK_EQ(call<size_t>(kU, "strlen", "hello"), 5u);
    CHECK_EQ(call<int>(kU, "strcmp", "a", "b"), -1);
    CHECK_EQ(call<int>(kU, "strcmp", "b", "a"), 1);
    CHECK_EQ(call<int>(kU, "strcmp", "abc", "abc"), 0);
    CHECK_EQ(call<int>(kU, "_stricmp", "HeLLo", "hello"), 0);
    CHECK_EQ(call<int>(kU, "_strnicmp", "HELLO", "help", 3), 0);
    CHECK_EQ(call<int>(kU, "_strnicmp", "HELLO", "help", 4), -1);
    CHECK_EQ(call<int>(kU, "toupper", 'a'), 'A');
    CHECK_EQ(call<int>(kU, "tolower", 'Q'), 'q');
    CHECK_EQ(call<int>(kU, "isdigit", '7') != 0, 1);
    CHECK_EQ(call<int>(kU, "isdigit", 0xe9), 0);
    CHECK_EQ(call<int>(kU, "isalpha", -1), 0);
    char buf[16];
    CHECK_EQ(call<int>(kU, "strcpy_s", buf, size_t{16}, "abc"), 0);
    CHECK(std::strcmp(buf, "abc") == 0);
    CHECK_EQ(call<int>(kU, "strcat_s", buf, size_t{16}, "def"), 0);
    CHECK(std::strcmp(buf, "abcdef") == 0);
    CHECK_EQ(call<int>(kU, "strcpy_s", buf, size_t{3}, "abcdef"), 34);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(call<int>(kU, "strncpy_s", buf, size_t{16}, "abcdef", size_t{3}), 0);
    CHECK(std::strcmp(buf, "abc") == 0);
    CHECK_EQ(call<int>(kU, "strncpy_s", buf, size_t{4}, "abcdef", static_cast<size_t>(-1)), 80);
    CHECK(std::strcmp(buf, "abc") == 0);
    CHECK_EQ(call<int>(kU, "strncpy_s", buf, size_t{2}, "abcdef", size_t{5}), 34);
    char tok[] = "a,b;c";
    char* ctx = nullptr;
    char* t1 = call<char*>(kU, "strtok_s", tok, ",;", &ctx);
    char* t2 = call<char*>(kU, "strtok_s", static_cast<char*>(nullptr), ",;", &ctx);
    char* t3 = call<char*>(kU, "strtok_s", static_cast<char*>(nullptr), ",;", &ctx);
    CHECK(t1 && t2 && t3 && !std::strcmp(t1, "a") && !std::strcmp(t2, "b") && !std::strcmp(t3, "c"));
}

TEST(crt_memory_functions) {
    char a[8] = "abcdefg";
    char b[8];
    call<void*>("vcruntime140.dll", "memcpy", b, a, size_t{8});
    CHECK(std::memcmp(a, b, 8) == 0);
    CHECK_EQ(call<int>("vcruntime140.dll", "memcmp", a, b, size_t{8}), 0);
    call<void*>("vcruntime140.dll", "memset", b, 0x41, size_t{4});
    CHECK(std::memcmp(b, "AAAAefg", 7) == 0);
    CHECK(call<void*>("vcruntime140.dll", "memchr", a, 'd', size_t{8}) == a + 3);
    CHECK(call<char*>("vcruntime140.dll", "strchr", a, 'c') == a + 2);
    CHECK(call<char*>("vcruntime140.dll", "strrchr", "abcabc", 'b') != nullptr);
    CHECK(call<char*>("vcruntime140.dll", "strstr", a, "cde") == a + 2);
}

TEST(crt_wide_strings_are_utf16) {
    const char16_t* w = u"hello\U0001F600";
    CHECK_EQ(call<size_t>(kU, "wcslen", w), 7u);
    CHECK_EQ(call<int>(kU, "wcscmp", u"abc", u"abd"), -1);
    CHECK_EQ(call<int>(kU, "wcscmp", u"abc", u"abc"), 0);
    CHECK_EQ(call<int>(kU, "_wcsnicmp", u"ABCdef", u"abcDEG", size_t{5}), 0);
    CHECK_EQ(call<int>(kU, "_wcsnicmp", u"ABCdef", u"abcDEG", size_t{6}), -1);
    CHECK_EQ(call<int>(kU, "_wcsicoll", u"Zed", u"zed"), 0);
    char16_t buf[8] = u"ab";
    CHECK_EQ(call<int>(kU, "wcscat_s", buf, size_t{8}, u"cd"), 0);
    CHECK_EQ(call<size_t>(kU, "wcslen", static_cast<const char16_t*>(buf)), 4u);
    CHECK_EQ(call<int>(kU, "wcscat_s", buf, size_t{5}, u"xyz"), 34);
    char16_t dst[6];
    call<char16_t*>(kU, "wcsncpy", dst, u"ab", size_t{6});
    CHECK(dst[0] == u'a' && dst[1] == u'b' && dst[2] == 0 && dst[5] == 0);
    const char16_t* p = u"a/b/c";
    char16_t* last = call<char16_t*>("vcruntime140.dll", "wcsrchr", p, static_cast<char16_t>(u'/'));
    CHECK(last == p + 3);
}

TEST(crt_convert_is_llp64) {
    char* end = nullptr;
    CHECK_EQ(call<int32_t>(kU, "strtol", "123", &end, 10), 123);
    CHECK_EQ(*end, 0);
    errno = 0;
    CHECK_EQ(call<int32_t>(kU, "strtol", "4294967296", &end, 10), INT32_MAX);
    CHECK_EQ(errno, 34);
    CHECK_EQ(call<int32_t>(kU, "strtol", "-4294967296", &end, 10), INT32_MIN);
    CHECK_EQ(call<int32_t>(kU, "strtol", "0x7f", &end, 16), 127);
    CHECK_EQ(call<uint32_t>(kU, "strtoul", "4294967295", &end, 10), 0xFFFFFFFFu);
    CHECK_EQ(call<uint32_t>(kU, "strtoul", "-1", &end, 10), 0xFFFFFFFFu);
    errno = 0;
    CHECK_EQ(call<uint32_t>(kU, "strtoul", "4294967296", &end, 10), 0xFFFFFFFFu);
    CHECK_EQ(errno, 34);
    CHECK_EQ(call<int64_t>(kU, "strtoll", "-9000000000", &end, 10), -9000000000ll);
    CHECK_EQ(call<int64_t>(kU, "_strtoi64", "9000000000", &end, 10), 9000000000ll);
    CHECK_EQ(call<uint64_t>(kU, "_strtoui64", "18446744073709551615", &end, 10), 18446744073709551615ull);
    CHECK_EQ(call<int>(kU, "atoi", "  -42x"), -42);
    CHECK_EQ(call<int64_t>(kU, "atoll", "5000000000"), 5000000000ll);
    CHECK(std::fabs(call<double>(kU, "atof", "3.25") - 3.25) < 1e-12);
    CHECK(std::fabs(call<double>(kU, "strtod", "1e3", &end) - 1000.0) < 1e-9);
    CHECK(std::fabs(call<float>(kU, "strtof", "0.5", &end) - 0.5f) < 1e-6f);
    char buf[40];
    call<char*>(kU, "_itoa", -255, buf, 10);
    CHECK(!std::strcmp(buf, "-255"));
    call<char*>(kU, "_itoa", 255, buf, 16);
    CHECK(!std::strcmp(buf, "ff"));
    call<char*>(kU, "_itoa", -1, buf, 16);
    CHECK(!std::strcmp(buf, "ffffffff"));
}

TEST(crt_rand_matches_msvc_lcg) {
    call<void>(kU, "srand", 1u);
    CHECK_EQ(call<int>(kU, "rand"), 41);
    CHECK_EQ(call<int>(kU, "rand"), 18467);
    CHECK_EQ(call<int>(kU, "rand"), 6334);
    call<void>(kU, "srand", 1u);
    CHECK_EQ(call<int>(kU, "rand"), 41);
}

TEST(crt_math) {
    CHECK(std::fabs(call<double>(kU, "sin", 1.0) - std::sin(1.0)) < 1e-15);
    CHECK(std::fabs(call<double>(kU, "pow", 2.0, 10.0) - 1024.0) < 1e-9);
    CHECK(std::fabs(call<double>(kU, "atan2", 1.0, 1.0) - std::atan2(1.0, 1.0)) < 1e-15);
    CHECK(std::fabs(call<float>(kU, "powf", 2.0f, 3.0f) - 8.0f) < 1e-6f);
    CHECK(std::fabs(call<float>(kU, "fmodf", 7.5f, 2.0f) - 1.5f) < 1e-6f);
    CHECK(call<float>(kU, "ceilf", 1.25f) == 2.0f);
    CHECK_EQ(call<int32_t>(kU, "lroundf", 2.5f), 3);
    CHECK_EQ(call<int32_t>(kU, "lroundf", 1e20f), INT32_MAX);
    double ip = 0;
    const double frac = call<double>(kU, "modf", 3.75, &ip);
    CHECK(ip == 3.0 && std::fabs(frac - 0.75) < 1e-15);
    const double nan = std::nan("");
    const double inf = INFINITY, zero = 0.0, sub = 1e-310, one = 1.0;
    CHECK_EQ(call<int16_t>(kU, "_dtest", &nan), 2);
    CHECK_EQ(call<int16_t>(kU, "_dtest", &inf), 1);
    CHECK_EQ(call<int16_t>(kU, "_dtest", &zero), 0);
    CHECK_EQ(call<int16_t>(kU, "_dtest", &sub), -2);
    CHECK_EQ(call<int16_t>(kU, "_dtest", &one), -1);
    CHECK(call<int16_t>(kU, "_fdsign", -1.0f) != 0);
    CHECK_EQ(call<int16_t>(kU, "_fdsign", 1.0f), 0);
    CHECK(call<float>(kU, "nexttowardf", 1.0f, 2.0) > 1.0f);
}

namespace {

std::u16string wfmt(const char16_t* fmt, std::initializer_list<uint64_t> args = {}, int* rc = nullptr) {
    uint64_t slots[16] = {};
    size_t i = 0;
    for (uint64_t a : args) slots[i++] = a;
    char16_t buf[256] = {};
    const int n = call<int>(kU, "__stdio_common_vsnwprintf_s", uint64_t{0}, buf, size_t{256}, static_cast<size_t>(-1), fmt,
                            static_cast<void*>(nullptr), static_cast<const uint64_t*>(slots));
    if (rc) *rc = n;
    return buf;
}

uint64_t dbits(double d) {
    uint64_t v;
    std::memcpy(&v, &d, 8);
    return v;
}

std::string nfmt(const char* fmt, std::initializer_list<uint64_t> args) {
    uint64_t slots[16] = {};
    size_t i = 0;
    for (uint64_t a : args) slots[i++] = a;
    char buf[256] = {};
    call<int>(kU, "__stdio_common_vsprintf_s", uint64_t{0}, buf, size_t{256}, fmt, static_cast<void*>(nullptr),
              static_cast<const uint64_t*>(slots));
    return buf;
}

}  // namespace

TEST(crt_printf_wide_and_win64_va_list) {
    int rc = 0;
    CHECK(wfmt(u"%d|%5d|%-5d|%05d", {static_cast<uint64_t>(-7), 42, 42, 42}, &rc) == u"-7|   42|42   |00042");
    CHECK_EQ(rc, 20);
    CHECK(wfmt(u"%ls %s", {reinterpret_cast<uintptr_t>(u"wide"), reinterpret_cast<uintptr_t>(u"w2")}) == u"wide w2");
    CHECK(wfmt(u"%hs|%S", {reinterpret_cast<uintptr_t>("narrow"), reinterpret_cast<uintptr_t>("n2")}) == u"narrow|n2");
    CHECK(wfmt(u"%x %X %#x %o", {255, 255, 255, 8}) == u"ff FF 0xff 10");
    CHECK(wfmt(u"%lld %I64d %zd", {static_cast<uint64_t>(-5000000000ll), 5000000000ull, 7}) == u"-5000000000 5000000000 7");
    CHECK(wfmt(u"%ld %u", {0xFFFFFFFFull, 0xFFFFFFFFull}) == u"-1 4294967295");
    CHECK(wfmt(u"%p", {0x140001234ull}) == u"0000000140001234");
    CHECK(wfmt(u"%.2f|%8.3f|%e", {dbits(3.14159), dbits(2.5), dbits(1000.0)}) == u"3.14|   2.500|1.000000e+03");
    CHECK(wfmt(u"%c%c", {u'o', u'k'}) == u"ok");
    CHECK(wfmt(u"%*d|%-*d|", {4, 7, 4, 7}) == u"   7|7   |");
    CHECK(wfmt(u"100%%") == u"100%");
    CHECK(wfmt(u"%.3s", {reinterpret_cast<uintptr_t>(u"abcdef")}) == u"abc");
    CHECK(wfmt(u"%s", {0}) == u"(null)");
    CHECK(wfmt(u"%hhd %hd", {0x1ff, 0x1ffff}) == u"-1 -1");
}

TEST(crt_printf_narrow) {
    CHECK(nfmt("%s=%d", {reinterpret_cast<uintptr_t>("x"), 5}) == "x=5");
    CHECK(nfmt("%ws", {reinterpret_cast<uintptr_t>(u"wé")}) == "w\xc3\xa9");
    CHECK(nfmt("%5.1f", {dbits(2.25)}) == "  2.2" || nfmt("%5.1f", {dbits(2.25)}) == "  2.3");
}

TEST(crt_printf_buffer_contracts) {
    uint64_t slots[4] = {};
    char16_t small[4];
    CHECK_EQ(call<int>(kU, "__stdio_common_vsnwprintf_s", uint64_t{0}, small, size_t{4}, static_cast<size_t>(-1), u"abcdef",
                       static_cast<void*>(nullptr), static_cast<const uint64_t*>(slots)), -1);
    CHECK(small[0] == u'a' && small[2] == u'c' && small[3] == 0);
    char16_t tiny[4];
    CHECK_EQ(call<int>(kU, "__stdio_common_vswprintf_s", uint64_t{0}, tiny, size_t{4}, u"abcdef", static_cast<void*>(nullptr),
                       static_cast<const uint64_t*>(slots)), -1);
    CHECK_EQ(tiny[0], 0);
    char b[8];
    CHECK_EQ(call<int>(kU, "__stdio_common_vsprintf", uint64_t{2}, b, size_t{4}, "abcdef", static_cast<void*>(nullptr),
                       static_cast<const uint64_t*>(slots)), 6);
    CHECK(!std::strcmp(b, "abc"));
}

TEST(crt_time_win_tm_layout) {
    uint8_t tm_buf[36];
    const int64_t t = 86400 * 365;
    CHECK_EQ(call<int>(kU, "_gmtime64_s", tm_buf, &t), 0);
    int32_t f[9];
    std::memcpy(f, tm_buf, 36);
    CHECK_EQ(f[5], 71);
    CHECK_EQ(f[4], 0);
    CHECK_EQ(f[3], 1);
    CHECK_EQ(f[6], 5);
    const int64_t neg = -1;
    CHECK_EQ(call<int>(kU, "_gmtime64_s", tm_buf, &neg), 22);
    CHECK_EQ(call<int64_t>(kU, "_mkgmtime64", tm_buf) == -1, 1);
    CHECK_EQ(call<int>(kU, "_gmtime64_s", tm_buf, &t), 0);
    CHECK_EQ(call<int64_t>(kU, "_mkgmtime64", tm_buf), t);
    char out[32];
    CHECK(call<size_t>(kU, "strftime", out, size_t{32}, "%Y-%m-%d", tm_buf) == 10);
    CHECK(!std::strcmp(out, "1971-01-01"));
    int64_t now = 0;
    CHECK(call<int64_t>(kU, "_time64", &now) == now && now > 1600000000);
    CHECK(call<double>(kU, "_difftime64", int64_t{10}, int64_t{4}) == 6.0);
}

TEST(crt_locale_and_misc) {
    struct L {
        char *dp, *ts, *gr;
    };
    auto* lc = call<L*>(kU, "localeconv");
    CHECK(lc && lc->dp && lc->dp[0] == '.' && lc->dp[1] == 0);
    void* p = call<void*>(kU, "malloc", size_t{64});
    CHECK(p != nullptr);
    p = call<void*>(kU, "realloc", p, size_t{128});
    CHECK(p != nullptr);
    call<void>(kU, "free", p);
    int* e = call<int*>(kU, "_errno");
    CHECK(e == &errno);
    CHECK_EQ(call<int>(kU, "_set_errno", 2), 0);
    int got = 0;
    CHECK_EQ(call<int>(kU, "_get_errno", &got), 0);
    CHECK_EQ(got, 2);
}
