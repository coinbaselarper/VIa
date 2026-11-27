#include <windows.h>
#include <d3d12.h>
#include <dxguids.h>
#include <intrin.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <gta6rt/x64_state.h>
#include <gta6rt/nt_layout.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

#define SZ(T, n) static_assert(sizeof(T) == (n), #T " size")
#define OFF(T, f, n) static_assert(offsetof(T, f) == (n), #T "." #f " offset")

SZ(BYTE, 1);
SZ(WORD, 2);
SZ(DWORD, 4);
SZ(LONG, 4);
SZ(ULONG, 4);
SZ(BOOL, 4);
SZ(BOOLEAN, 1);
SZ(WCHAR, 2);
SZ(LONGLONG, 8);
SZ(DWORD64, 8);
SZ(ULONG_PTR, 8);
SZ(SIZE_T, 8);
SZ(HANDLE, 8);
SZ(HRESULT, 4);
SZ(GUID, 16);
SZ(LARGE_INTEGER, 8);
SZ(ULARGE_INTEGER, 8);
SZ(FILETIME, 8);
SZ(SYSTEMTIME, 16);
SZ(LUID, 8);
SZ(RECT, 16);
SZ(POINT, 8);
SZ(LIST_ENTRY, 16);
SZ(SLIST_HEADER, 16);
SZ(M128A, 16);
SZ(XSAVE_FORMAT, 512);
SZ(CONTEXT, 1232);
static_assert(alignof(CONTEXT) == 16, "CONTEXT alignment");
OFF(CONTEXT, ContextFlags, 0x30);
OFF(CONTEXT, MxCsr, 0x34);
OFF(CONTEXT, SegCs, 0x38);
OFF(CONTEXT, EFlags, 0x44);
OFF(CONTEXT, Dr0, 0x48);
OFF(CONTEXT, Rax, 0x78);
OFF(CONTEXT, Rsp, 0x98);
OFF(CONTEXT, R8, 0xB8);
OFF(CONTEXT, Rip, 0xF8);
OFF(CONTEXT, FltSave, 0x100);
OFF(CONTEXT, Xmm0, 0x1A0);
OFF(CONTEXT, VectorRegister, 0x300);
OFF(CONTEXT, VectorControl, 0x4A0);
OFF(CONTEXT, LastExceptionFromRip, 0x4C8);
SZ(EXCEPTION_RECORD, 152);
OFF(EXCEPTION_RECORD, ExceptionAddress, 0x10);
OFF(EXCEPTION_RECORD, NumberParameters, 0x18);
OFF(EXCEPTION_RECORD, ExceptionInformation, 0x20);
SZ(EXCEPTION_POINTERS, 16);
SZ(RUNTIME_FUNCTION, 12);
SZ(DISPATCHER_CONTEXT, 80);
SZ(UNWIND_HISTORY_TABLE, 216);
SZ(KNONVOLATILE_CONTEXT_POINTERS, 256);
SZ(MEMORY_BASIC_INFORMATION, 48);
OFF(MEMORY_BASIC_INFORMATION, RegionSize, 24);
SZ(SYSTEM_INFO, 48);
SZ(MEMORYSTATUSEX, 64);
SZ(SYSTEM_LOGICAL_PROCESSOR_INFORMATION, 32);
SZ(SECURITY_ATTRIBUTES, 24);
SZ(OVERLAPPED, 32);
SZ(CRITICAL_SECTION, 40);
SZ(SRWLOCK, 8);
SZ(CONDITION_VARIABLE, 8);
SZ(INIT_ONCE, 8);
SZ(WIN32_FIND_DATAW, 592);
SZ(WIN32_FIND_DATAA, 320);
SZ(WIN32_FILE_ATTRIBUTE_DATA, 36);
SZ(BY_HANDLE_FILE_INFORMATION, 52);
SZ(FILE_SEGMENT_ELEMENT, 8);
SZ(STARTUPINFOW, 104);
SZ(PROCESS_INFORMATION, 24);
SZ(PROCESS_MEMORY_COUNTERS, 72);
SZ(THREADENTRY32, 28);
SZ(PROCESS_MEMORY_COUNTERS_EX, 80);
OFF(PROCESS_MEMORY_COUNTERS_EX, PrivateUsage, 72);
SZ(MODULEINFO, 24);
SZ(OVERLAPPED_ENTRY, 32);
OFF(OVERLAPPED_ENTRY, Internal, 16);
OFF(OVERLAPPED_ENTRY, dwNumberOfBytesTransferred, 24);
SZ(FILE_STANDARD_INFO, 24);
OFF(FILE_STANDARD_INFO, NumberOfLinks, 16);
OFF(FILE_STANDARD_INFO, Directory, 21);
SZ(WIN32_MEMORY_RANGE_ENTRY, 16);
SZ(CREATEFILE2_EXTENDED_PARAMETERS, 32);
OFF(CREATEFILE2_EXTENDED_PARAMETERS, lpSecurityAttributes, 16);
SZ(GROUP_AFFINITY, 16);
SZ(PROCESSOR_NUMBER, 4);
SZ(OSVERSIONINFOEXW, 284);
OFF(OSVERSIONINFOEXW, wProductType, 282);
SZ(UNICODE_STRING, 16);
SZ(IMAGE_DOS_HEADER, 64);
SZ(IMAGE_FILE_HEADER, 20);
SZ(IMAGE_OPTIONAL_HEADER64, 240);
SZ(IMAGE_NT_HEADERS64, 264);
SZ(IMAGE_SECTION_HEADER, 40);
SZ(IMAGE_IMPORT_DESCRIPTOR, 20);
SZ(IMAGE_THUNK_DATA64, 8);
SZ(IMAGE_TLS_DIRECTORY64, 40);
SZ(IMAGE_EXPORT_DIRECTORY, 40);
SZ(IMAGE_BASE_RELOCATION, 8);
SZ(IMAGE_DELAYLOAD_DESCRIPTOR, 32);

SZ(MEM_EXTENDED_PARAMETER, 16);
OFF(MEM_EXTENDED_PARAMETER, Pointer, 8);
SZ(MEM_ADDRESS_REQUIREMENTS, 24);
OFF(MEM_ADDRESS_REQUIREMENTS, Alignment, 16);
SZ(CPINFO, 20);
OFF(CPINFO, LeadByte, 6);

SZ(gta6rt_x64_context, GTA6RT_X64_CONTEXT_SIZE);
static_assert(alignof(gta6rt_x64_context) == 16, "gta6rt_x64_context alignment");
static_assert(offsetof(gta6rt_x64_context, context_flags) == offsetof(CONTEXT, ContextFlags), "ctx flags");
static_assert(offsetof(gta6rt_x64_context, eflags) == offsetof(CONTEXT, EFlags), "ctx eflags");
static_assert(offsetof(gta6rt_x64_context, rax) == offsetof(CONTEXT, Rax), "ctx rax");
static_assert(offsetof(gta6rt_x64_context, r15) == offsetof(CONTEXT, R15), "ctx r15");
static_assert(offsetof(gta6rt_x64_context, rip) == offsetof(CONTEXT, Rip), "ctx rip");
static_assert(offsetof(gta6rt_x64_context, fp) == offsetof(CONTEXT, FltSave), "ctx fltsave");
static_assert(offsetof(gta6rt_x64_context, fp.v.xmm) == offsetof(CONTEXT, Xmm0), "ctx xmm0");
static_assert(offsetof(gta6rt_x64_context, vector_register) == offsetof(CONTEXT, VectorRegister), "ctx vreg");
static_assert(offsetof(gta6rt_x64_context, last_exception_from_rip) == offsetof(CONTEXT, LastExceptionFromRip), "ctx tail");
SZ(gta6rt_x64_exception_record, GTA6RT_X64_EXCEPTION_RECORD_SIZE);
static_assert(offsetof(gta6rt_x64_exception_record, address) == offsetof(EXCEPTION_RECORD, ExceptionAddress), "rec addr");
static_assert(offsetof(gta6rt_x64_exception_record, n_params) == offsetof(EXCEPTION_RECORD, NumberParameters), "rec n");
static_assert(offsetof(gta6rt_x64_exception_record, params) == offsetof(EXCEPTION_RECORD, ExceptionInformation), "rec p");

// Title SDK 26100 layouts (spike/s1-host-build-scout REVIEW item 4: DirectX-Headers v1.618.2).
SZ(D3D12_UNORDERED_ACCESS_VIEW_DESC, 40);
SZ(D3D12_SHADER_RESOURCE_VIEW_DESC, 40);
SZ(D3D12_RESOURCE_DESC, 56);
SZ(D3D12_HEAP_PROPERTIES, 20);
SZ(D3D12_CPU_DESCRIPTOR_HANDLE, 8);
SZ(D3D12_GPU_DESCRIPTOR_HANDLE, 8);

static_assert(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) == E_ACCESSDENIED, "HRESULT_FROM_WIN32");
static_assert(sizeof(TEXT("ab")) == 6, "TEXT() is UTF-16 under UNICODE");

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>
#include <spatialaudioclient.h>

SZ(WAVEFORMATEX, 18);
SZ(WAVEFORMATEXTENSIBLE, 40);
OFF(WAVEFORMATEXTENSIBLE, SubFormat, 24);
SZ(PROPERTYKEY, 20);
SZ(PROPVARIANT, 24);
OFF(PROPVARIANT, pwszVal, 8);
SZ(AudioClientProperties, 16);
SZ(SpatialAudioObjectRenderStreamActivationParams, 40);

extern "C" int gta6rt_compat_c_check(void);

static long long WINAPI winabi_mix(int a, long long b, double c, int d, long long e, double f, int g) {
    return a + b + (long long)c + d + e + (long long)f + g;
}
static long long (WINAPI* volatile g_winabi_mix)(int, long long, double, int, long long, double, int) = winabi_mix;

int main() {
    const GUID& dev = __uuidof(ID3D12Device);
    const GUID& blob = __uuidof(ID3DBlob);
    WCHAR wbuf[16];
    char nbuf[16];
    size_t n16 = gta6rt_utf8_to_u16("h\xc3\xa9llo", wbuf, 16);
    size_t n8 = gta6rt_u16_to_utf8(wbuf, nbuf, 16);
    LONG v = 41;
    LONG inc = InterlockedIncrement(&v);
    unsigned long idx = 0;
    _BitScanReverse64(&idx, 0x8000ull);
#if defined(GTA6RT_ARCH_X86_64)
    const char* arch = "x86_64";
#else
    const char* arch = "arm64";
#endif
    std::printf("arch=%s sizeof(CONTEXT)=%zu sizeof(EXCEPTION_RECORD)=%zu sizeof(WCHAR)=%zu sizeof(LONG)=%zu "
                "sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC)=%zu\n",
                arch, sizeof(CONTEXT), sizeof(EXCEPTION_RECORD), sizeof(WCHAR), sizeof(LONG),
                sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC));
    std::printf("uuid(ID3D12Device)=%08x uuid(ID3DBlob)=%08x IID_ID3D12Device=%08x\n", dev.Data1, blob.Data1,
                IID_ID3D12Device.Data1);
    std::printf("utf8->u16=%zu u16->utf8=%zu roundtrip=%s interlocked=%d bsr64=%lu c_check=%d\n", n16, n8,
                std::strcmp(nbuf, "h\xc3\xa9llo") == 0 ? "ok" : "FAIL", inc, idx, gta6rt_compat_c_check());
    bool ok = dev.Data1 == 0x189819f1 && blob.Data1 == 0x8ba5fb08 && IID_ID3D12Device.Data1 == 0x189819f1 && n16 == 5 &&
              n8 == 6 && std::strcmp(nbuf, "h\xc3\xa9llo") == 0 && inc == 42 && idx == 15 && gta6rt_compat_c_check() == 1;
    long long mix = g_winabi_mix(1, 20, 300.0, 4000, 50000, 600000.0, 7000000);
    std::printf("winabi_mix=%lld (expect 7654321)\n", mix);
    ok = ok && mix == 7654321;
    std::printf("compat_abi_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
