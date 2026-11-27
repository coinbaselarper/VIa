#include <gta6rt/abi.h>

/* The SDK returns BOOL; compat/threadpoolapiset.h declares void, so this TU defines it without that header. */
GTA6RT_GUEST_ABI int CallbackMayRunLong(void* instance) {
    (void)instance;
    return 1;
}
