// Media Foundation subset, driven the way the title's reader (exe+0x33a4100..0x33a5620) uses it, plus the rest of
// IMFAttributes / IMFMediaType.
#include "media_common.h"
#include "media_exports.h"
#include "test_util.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

using namespace rt::media;
using mt::call;

namespace {

constexpr Guid kMajorType{0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
constexpr Guid kSubtype{0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
constexpr Guid kAudio{0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr Guid kVideo{0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr Guid kPcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr Guid kNv12{0x3231564e, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr Guid kSamplesPerSecond{0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
constexpr Guid kHardwareTransforms{0xa634a91c, 0x822b, 0x41b9, {0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0}};
constexpr Guid kFrameSize{0x1652c33d, 0xd6b2, 0x4012, {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
constexpr Guid kUserData{0xb6bc765f, 0x4c3b, 0x40a4, {0xbd, 0x51, 0x25, 0x35, 0xb6, 0x6f, 0xe0, 0x9d}};
constexpr Guid kIidIMFMediaType{0x44ae0fa8, 0xea31, 0x4109, {0x8d, 0x2e, 0x4c, 0xae, 0x49, 0x97, 0xc5, 0x55}};
constexpr Guid kIidIMFAttributes{0x2cd2d921, 0xc447, 0x44a7, {0xa1, 0x3c, 0x4a, 0xda, 0xbf, 0xc2, 0x47, 0xe3}};

enum {
    QI = 0, AddRef = 1, Release = 2, GetItem = 3, GetItemType = 4, CompareItem = 5, Compare = 6, GetUINT32 = 7,
    GetUINT64 = 8, GetDouble = 9, GetGUID = 10, GetStringLength = 11, GetString = 12, GetAllocatedString = 13,
    GetBlobSize = 14, GetBlob = 15, GetAllocatedBlob = 16, GetUnknown = 17, SetItem = 18, DeleteItem = 19,
    DeleteAllItems = 20, SetUINT32 = 21, SetUINT64 = 22, SetDouble = 23, SetGUID = 24, SetString = 25, SetBlob = 26,
    SetUnknown = 27, LockStore = 28, UnlockStore = 29, GetCount = 30, GetItemByIndex = 31, CopyAllItems = 32,
    GetMajorType = 33, IsCompressedFormat = 34, IsEqual = 35, GetRepresentation = 36,
    CreateObjectFromURL = 3, CreateObjectFromByteStream = 4, BeginCreateObjectFromURL = 5,
};

struct FakeUnknown {
    const void* const* vtbl;
    int refs;
};
GTA6RT_GUEST_ABI int32_t fu_qi(FakeUnknown* s, const Guid*, uint64_t* out) {
    ++s->refs;
    *out = reinterpret_cast<uint64_t>(s);
    return 0;
}
GTA6RT_GUEST_ABI uint32_t fu_add_ref(FakeUnknown* s) { return static_cast<uint32_t>(++s->refs); }
GTA6RT_GUEST_ABI uint32_t fu_release(FakeUnknown* s) { return static_cast<uint32_t>(--s->refs); }
const void* const kFuVtbl[] = {reinterpret_cast<const void*>(&fu_qi), reinterpret_cast<const void*>(&fu_add_ref),
                               reinterpret_cast<const void*>(&fu_release)};

bool g_file_exists = false;
std::u16string g_probed;
bool fake_probe(const char16_t* p) {
    g_probed = p;
    return g_file_exists;
}

void test_title_sequence() {
    CHECK_HR(MFStartup(0x20070, 1), kSOk);
    CHECK_HR(MFStartup(0x30070, 1), kMfEBadStartupVersion);
    CHECK_HR(MFStartup(0x20080, 0), kMfEBadStartupVersion);
    CHECK_HR(MFStartup(0x20040, 0), kSOk);
    CHECK_HR(MFShutdown(), kSOk);

    // exe+0x33a4410: MFCreateAttributes(&a, 3); a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1).
    uint64_t attrs = 0;
    CHECK_HR(MFCreateAttributes(&attrs, 3), kSOk);
    CHECK_HR(call<int32_t>(attrs, SetUINT32, &kHardwareTransforms, 1u), kSOk);

    // exe+0x33a4d25: MFCreateSourceResolver, CreateObjectFromURL(url, 0x10001, NULL, &type, &source).
    uint64_t resolver = 0;
    CHECK_HR(MFCreateSourceResolver(&resolver), kSOk);
    uint32_t type = 99;
    uint64_t object = 1;
    set_file_probe(&fake_probe);
    g_file_exists = false;
    CHECK_HR(call<int32_t>(resolver, CreateObjectFromURL, u"G:\\movies\\intro.mp4", 0x10001u, uint64_t{0}, &type,
                           &object),
             kEFileNotFound);
    CHECK(object == 0 && type == 2);
    CHECK(g_probed == u"G:\\movies\\intro.mp4");
    g_file_exists = true;
    CHECK_HR(call<int32_t>(resolver, CreateObjectFromURL, u"file:///G:/a%20b/c.wmv", 0x10001u, uint64_t{0}, &type,
                           &object),
             kMfEUnsupportedByteStreamType);
    CHECK(g_probed == u"G:\\a b\\c.wmv");
    CHECK_HR(call<int32_t>(resolver, CreateObjectFromURL, u"https://example.invalid/x.mp4", 0x10001u, uint64_t{0},
                           &type, &object),
             kMfEUnsupportedScheme);
    CHECK_HR(call<int32_t>(resolver, CreateObjectFromURL, u"G:\\x.mp4", 0x10000u, uint64_t{0}, &type, &object),
             kEInvalidArg);
    CHECK_HR(call<int32_t>(resolver, BeginCreateObjectFromURL), kENotImpl);
    CHECK_HR((call<int32_t>(resolver, CreateObjectFromByteStream, uint64_t{1}, static_cast<const char16_t*>(nullptr),
                            0x10001u, uint64_t{0}, &type, &object)),
             kMfEUnsupportedByteStreamType);
    CHECK(call<uint32_t>(resolver, Release) == 0);

    uint64_t reader = 1;
    CHECK_HR(MFCreateSourceReaderFromMediaSource(0x1234, attrs, &reader), kENotImpl);
    CHECK(reader == 0);
    CHECK_HR(MFCreateSourceReaderFromMediaSource(0, attrs, &reader), kEInvalidArg);
    CHECK(call<uint32_t>(attrs, Release) == 0);

    // exe+0x33a4250 / +0x33a42b0: the output types the reader would be given.
    uint64_t video = 0, audio = 0;
    CHECK_HR(MFCreateMediaType(&video), kSOk);
    CHECK_HR(call<int32_t>(video, SetGUID, &kMajorType, &kVideo), kSOk);
    CHECK_HR(call<int32_t>(video, SetGUID, &kSubtype, &kNv12), kSOk);
    CHECK_HR(MFCreateMediaType(&audio), kSOk);
    CHECK_HR(call<int32_t>(audio, SetGUID, &kMajorType, &kAudio), kSOk);
    CHECK_HR(call<int32_t>(audio, SetGUID, &kSubtype, &kPcm), kSOk);
    CHECK_HR(call<int32_t>(audio, SetUINT32, &kSamplesPerSecond, 48000u), kSOk);
    Guid major{};
    CHECK_HR(call<int32_t>(audio, GetMajorType, &major), kSOk);
    CHECK(major == kAudio);
    uint32_t rate = 0;
    CHECK_HR(call<int32_t>(audio, GetUINT32, &kSamplesPerSecond, &rate), kSOk);
    CHECK(rate == 48000);
    uint32_t flags = 0;
    CHECK_HR(call<int32_t>(audio, IsEqual, video, &flags), kSFalse);
    CHECK(flags == 0);
    uint64_t audio2 = 0;
    CHECK_HR(MFCreateMediaType(&audio2), kSOk);
    CHECK_HR(call<int32_t>(audio, CopyAllItems, audio2), kSOk);
    CHECK_HR(call<int32_t>(audio, IsEqual, audio2, &flags), kSOk);
    CHECK(flags == 0xF);
    CHECK_HR(call<int32_t>(audio2, SetUINT32, &kSamplesPerSecond, 44100u), kSOk);
    CHECK_HR(call<int32_t>(audio, IsEqual, audio2, &flags), kSFalse);
    CHECK(flags == (1 | 2 | 8));
    int32_t compressed = -1;
    CHECK_HR(call<int32_t>(audio, IsCompressedFormat, &compressed), kSOk);
    CHECK(compressed == 1);
    uint64_t repr = 0;
    CHECK_HR(call<int32_t>(audio, GetRepresentation, &kAudio, &repr), kENotImpl);
    uint64_t qi = 0;
    CHECK_HR(call<int32_t>(audio, QI, &kIidIMFMediaType, &qi), kSOk);
    CHECK(qi == audio && call<uint32_t>(audio, Release) == 1);
    CHECK(call<uint32_t>(video, Release) == 0);
    CHECK(call<uint32_t>(audio, Release) == 0);
    CHECK(call<uint32_t>(audio2, Release) == 0);

    const int64_t t0 = MFGetSystemTime();
    usleep(50000);
    const int64_t dt = MFGetSystemTime() - t0;
    std::printf("MFGetSystemTime: +%lld (100 ns units) over usleep(50 ms)\n", static_cast<long long>(dt));
    CHECK(dt >= 500000 && dt < 2000000);
}

void test_attributes() {
    uint64_t a = 0;
    CHECK_HR(MFCreateAttributes(&a, 0), kSOk);
    CHECK(is_media_attributes(a));
    uint64_t qi = 0;
    CHECK_HR(call<int32_t>(a, QI, &kIidIMFMediaType, &qi), kENoInterface);
    CHECK_HR(call<int32_t>(a, QI, &kIidIMFAttributes, &qi), kSOk);
    CHECK(call<uint32_t>(a, Release) == 1);
    uint32_t u32 = 0;
    CHECK_HR(call<int32_t>(a, GetUINT32, &kFrameSize, &u32), kMfEAttributeNotFound);
    CHECK_HR(call<int32_t>(a, SetUINT64, &kFrameSize, (uint64_t{1920} << 32) | 1080), kSOk);
    CHECK_HR(call<int32_t>(a, GetUINT32, &kFrameSize, &u32), kMfEInvalidType);
    uint64_t u64 = 0;
    CHECK_HR(call<int32_t>(a, GetUINT64, &kFrameSize, &u64), kSOk);
    CHECK(u64 == ((uint64_t{1920} << 32) | 1080));
    uint32_t t = 0;
    CHECK_HR(call<int32_t>(a, GetItemType, &kFrameSize, &t), kSOk);
    CHECK(t == kVtUi8);
    CHECK_HR(call<int32_t>(a, SetDouble, &kMajorType, 2.5), kSOk);
    double dv = 0;
    CHECK_HR(call<int32_t>(a, GetDouble, &kMajorType, &dv), kSOk);
    CHECK(dv == 2.5);
    CHECK_HR(call<int32_t>(a, SetString, &kSubtype, u"hello"), kSOk);
    uint32_t len = 0;
    CHECK_HR(call<int32_t>(a, GetStringLength, &kSubtype, &len), kSOk);
    CHECK(len == 5);
    char16_t small[4];
    CHECK_HR(call<int32_t>(a, GetString, &kSubtype, small, 4u, &len), kENotSufficientBuffer);
    char16_t buf[8];
    CHECK_HR(call<int32_t>(a, GetString, &kSubtype, buf, 8u, &len), kSOk);
    CHECK(std::u16string(buf) == u"hello");
    uint64_t alloc = 0;
    CHECK_HR(call<int32_t>(a, GetAllocatedString, &kSubtype, &alloc, &len), kSOk);
    CHECK(std::u16string(reinterpret_cast<const char16_t*>(alloc)) == u"hello" && len == 5);
    std::free(reinterpret_cast<void*>(alloc));
    const uint8_t blob[3] = {1, 2, 3};
    CHECK_HR(call<int32_t>(a, SetBlob, &kUserData, blob, 3u), kSOk);
    uint8_t bb[2];
    uint32_t got = 0;
    CHECK_HR(call<int32_t>(a, GetBlob, &kUserData, bb, 2u, &got), kENotSufficientBuffer);
    uint8_t b3[3] = {};
    CHECK_HR(call<int32_t>(a, GetBlob, &kUserData, b3, 3u, &got), kSOk);
    CHECK(got == 3 && b3[2] == 3);
    CHECK_HR(call<int32_t>(a, GetAllocatedBlob, &kUserData, &alloc, &got), kSOk);
    CHECK(got == 3 && reinterpret_cast<uint8_t*>(alloc)[1] == 2);
    std::free(reinterpret_cast<void*>(alloc));
    uint32_t count = 0;
    CHECK_HR(call<int32_t>(a, GetCount, &count), kSOk);
    CHECK(count == 4);
    Guid key{};
    PropVariant pv{};
    CHECK_HR(call<int32_t>(a, GetItemByIndex, 0u, &key, &pv), kSOk);
    CHECK(key == kFrameSize && pv.vt == kVtUi8 && pv.uhVal == u64);
    CHECK_HR(call<int32_t>(a, GetItemByIndex, 9u, &key, &pv), kEInvalidArg);
    CHECK_HR(call<int32_t>(a, GetItem, &kSubtype, &pv), kSOk);
    CHECK(pv.vt == kVtLpwstr && std::u16string(reinterpret_cast<const char16_t*>(pv.ptr)) == u"hello");
    std::free(reinterpret_cast<void*>(pv.ptr));
    PropVariant set{};
    set.vt = kVtUi4;
    set.ulVal = 7;
    CHECK_HR(call<int32_t>(a, SetItem, &kSamplesPerSecond, &set), kSOk);
    int32_t same = 0;
    CHECK_HR(call<int32_t>(a, CompareItem, &kSamplesPerSecond, &set, &same), kSOk);
    CHECK(same == 1);
    set.vt = 3;
    CHECK_HR(call<int32_t>(a, SetItem, &kSamplesPerSecond, &set), kMfEInvalidType);
    FakeUnknown fu{kFuVtbl, 1};
    CHECK_HR(call<int32_t>(a, SetUnknown, &kHardwareTransforms, reinterpret_cast<uint64_t>(&fu)), kSOk);
    CHECK(fu.refs == 2);
    uint64_t back = 0;
    CHECK_HR(call<int32_t>(a, GetUnknown, &kHardwareTransforms, &kIidIUnknown, &back), kSOk);
    CHECK(back == reinterpret_cast<uint64_t>(&fu) && fu.refs == 3);
    --fu.refs;
    uint64_t b = 0;
    CHECK_HR(MFCreateAttributes(&b, 0), kSOk);
    CHECK_HR(call<int32_t>(a, CopyAllItems, b), kSOk);
    CHECK(fu.refs == 3);
    int32_t eq = 0;
    CHECK_HR(call<int32_t>(a, Compare, b, 2u, &eq), kSOk);
    CHECK(eq == 1);
    CHECK_HR(call<int32_t>(b, DeleteItem, &kUserData), kSOk);
    CHECK_HR(call<int32_t>(a, Compare, b, 2u, &eq), kSOk);
    CHECK(eq == 0);
    CHECK_HR(call<int32_t>(a, Compare, b, 1u, &eq), kSOk);
    CHECK(eq == 1);
    CHECK_HR(call<int32_t>(a, Compare, b, 4u, &eq), kSOk);
    CHECK(eq == 1);
    CHECK_HR(call<int32_t>(a, Compare, reinterpret_cast<uint64_t>(&fu), 2u, &eq), kENotImpl);
    CHECK_HR(call<int32_t>(a, LockStore), kSOk);
    CHECK_HR(call<int32_t>(a, UnlockStore), kSOk);
    CHECK(call<uint32_t>(b, Release) == 0);
    CHECK(fu.refs == 2);
    CHECK_HR(call<int32_t>(a, DeleteAllItems), kSOk);
    CHECK(fu.refs == 1);
    CHECK(call<uint32_t>(a, Release) == 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    set_guest_invoker(&direct_guest_invoker);
    test_title_sequence();
    test_attributes();
    return mt::finish("media_mf_test");
}
