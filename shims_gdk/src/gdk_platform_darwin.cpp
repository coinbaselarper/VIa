#include "gdk_platform.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <mutex>
#include <unordered_map>

namespace rt::gdk {

namespace {
std::mutex g_mutex;
std::unordered_map<void*, IOPMAssertionID> g_live;
}  // namespace

void* display_deferral_acquire() {
    IOPMAssertionID id = kIOPMNullAssertionID;
    const IOReturn r = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                                   kIOPMAssertionLevelOn, CFSTR("Title display timeout deferral"), &id);
    if (r != kIOReturnSuccess) return nullptr;
    auto* token = new char;
    std::lock_guard lock(g_mutex);
    g_live.emplace(token, id);
    return token;
}

void display_deferral_release(void* handle) {
    IOPMAssertionID id;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_live.find(handle);
        if (it == g_live.end()) return;
        id = it->second;
        g_live.erase(it);
    }
    IOPMAssertionRelease(id);
    delete static_cast<char*>(handle);
}

}  // namespace rt::gdk
