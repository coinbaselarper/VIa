#pragma once

/*
 * gta6rt windows.h: Windows x64 type/ABI surface for the macOS host. Not the Windows SDK.
 * Layout: one header per SDK header name (minwindef.h, winnt.h, synchapi.h, ...); add declarations to the
 * header the SDK puts them in. Functions declared here are implemented by the module named in each header.
 */

#include <stdarg.h>
#include <gta6rt/wincompat_config.h>
#include <sdkddkver.h>
#include <winapifamily.h>
#include <sal.h>
#include <minwindef.h>
#include <windef.h>
#include <winnt.h>
#include <winerror.h>
#include <minwinbase.h>
#include <errhandlingapi.h>
#include <handleapi.h>
#include <memoryapi.h>
#include <heapapi.h>
#include <synchapi.h>
#include <processthreadsapi.h>
#include <processenv.h>
#include <fileapi.h>
#include <ioapiset.h>
#include <libloaderapi.h>
#include <sysinfoapi.h>
#include <profileapi.h>
#include <stringapiset.h>
#include <debugapi.h>
#include <threadpoolapiset.h>
#include <utilapiset.h>
#include <winnls.h>
#include <datetimeapi.h>
#include <winbase.h>
#include <winuser.h>
#include <winreg.h>
#include <gta6rt/msvc_crt.h>
#ifdef __cplusplus
#include <unknwnbase.h>
#endif
