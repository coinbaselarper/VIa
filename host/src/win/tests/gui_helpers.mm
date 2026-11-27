// AppKit helpers of win_gui_test.cpp (kept apart: windows.h and AppKit headers do not mix).
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gui_helpers.h"
#include "mac_window.h"

static NSWindow* window_of(gta6rt_mac_window* win) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)gta6rt_mac_window_metal_layer(win);
    for (NSWindow* cand in NSApp.windows)
        if (cand.contentView.layer == layer) return cand;
    return nil;
}

extern "C" {
int gui_has_display(void) { return NSScreen.mainScreen != nil; }

int gui_make_key(gta6rt_mac_window* win) {
    NSWindow* ns = window_of(win);
    if (!ns) return 0;
    [ns makeKeyAndOrderFront:nil];
    return 1;
}

int gui_menu_items(void) { return int(NSApp.mainMenu.itemArray.count); }

void gui_key(gta6rt_mac_window* win, const char* characters, unsigned short key_code, int ctrl, int cmd) {
    NSWindow* ns = window_of(win);
    if (!ns) return;
    NSString* chars = characters[0] == 'F' && characters[1] == '1' ? [NSString stringWithFormat:@"%C", (unichar)NSF11FunctionKey]
                                                                   : [NSString stringWithUTF8String:characters];
    NSEventModifierFlags flags = (ctrl ? NSEventModifierFlagControl : 0) | (cmd ? NSEventModifierFlagCommand : 0);
    NSEvent* e = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                  location:NSZeroPoint
                             modifierFlags:flags
                                 timestamp:0
                              windowNumber:ns.windowNumber
                                   context:nil
                                characters:chars
               charactersIgnoringModifiers:chars
                                 isARepeat:NO
                                   keyCode:key_code];
    [NSApp sendEvent:e];
}
}
