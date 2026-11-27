#include <gta6rt/media.h>

__attribute__((constructor)) static void gta6rt_media_autoregister() { gta6rt_media_register(); }
