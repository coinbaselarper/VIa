#include <windows.h>
#include <intrin.h>
#include <gta6rt/x64_state.h>

_Static_assert(sizeof(WCHAR) == 2, "WCHAR");
_Static_assert(sizeof(LONG) == 4, "LONG");
_Static_assert(sizeof(CONTEXT) == 1232, "CONTEXT");
_Static_assert(sizeof(gta6rt_x64_context) == sizeof(CONTEXT), "gta6rt_x64_context");
_Static_assert(sizeof(OVERLAPPED) == 32, "OVERLAPPED");
_Static_assert(sizeof(CRITICAL_SECTION) == 40, "CRITICAL_SECTION");

int gta6rt_compat_c_check(void) {
    GUID a = {1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    GUID b = a;
    return IsEqualGUID(&a, &b) ? 1 : 0;
}
