#include "PhysicalKeyState.h"

// Isolated translation unit: AppKit WITHOUT JuceHeader, so the Carbon `Point`
// type (MacTypes.h) cannot collide with juce::Point.
#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>

#include <atomic>
#include <cstdint>

#if __has_feature(objc_arc)
 #error "PhysicalKeyStateMac.mm is written for manual retain/release (the JUCE default)."
#endif

namespace t5
{
namespace
{
// Two bits per macOS virtual keycode (0…127), both written from the event monitor
// and read from the GUI timer — both the message thread, so relaxed ordering is
// enough; they are atomics so a read can never observe a torn word.
//   gHeld   — this application saw the key go down and has not seen it come up.
//   gStruck — a key-down event has arrived that no scan has acted on yet.
std::atomic<std::uint64_t> gHeld[2]   { {0ull}, {0ull} };
std::atomic<std::uint64_t> gStruck[2] { {0ull}, {0ull} };

id  gMonitor      = nil;
int gMonitorRefs  = 0;
bool gCommandDown = false;

NSArray* gObservers = nil;

void clearAll()
{
    for (int w = 0; w < 2; ++w)
    {
        gHeld[w].store (0ull, std::memory_order_relaxed);
        gStruck[w].store (0ull, std::memory_order_relaxed);
    }
}

bool testBit (const std::atomic<std::uint64_t>* mask, int vk)
{
    const auto word = mask[vk >> 6].load (std::memory_order_relaxed);
    return ((word >> (vk & 63)) & 1ull) != 0ull;
}

void setBits (unsigned short vk, bool down)
{
    if (vk >= 128)
        return;

    const std::uint64_t bit = 1ull << (vk & 63u);
    if (down)
    {
        gHeld[vk >> 6].fetch_or (bit, std::memory_order_relaxed);
        gStruck[vk >> 6].fetch_or (bit, std::memory_order_relaxed);
    }
    else
    {
        gHeld[vk >> 6].fetch_and (~bit, std::memory_order_relaxed);
        gStruck[vk >> 6].fetch_and (~bit, std::memory_order_relaxed);
    }
}

void observe (NSMutableArray* into, NSNotificationName name)
{
    id token = [[NSNotificationCenter defaultCenter]
        addObserverForName: name
                    object: nil
                     queue: [NSOperationQueue mainQueue]
                usingBlock: ^(NSNotification*)
    {
        // Delivery via the queue is asynchronous, so this can still fire after the
        // last editor handed its reference back.
        if (gMonitorRefs > 0)
        {
            gCommandDown = false;
            clearAll();
        }
    }];
    [into addObject: token];
}
}

// "Held" is the AND of two independent sources, because each one covers exactly
// the other's failure — and both failures are the same thing, a note that will
// not stop.
//
//   the event map  cannot report a release it never received. AppKit delivers no
//                  key-up while Command is held, and AppKit's own header states a
//                  local monitor "will not be called for events that are consumed
//                  by nested event-tracking loops such as control tracking, menu
//                  tracking, or window dragging". Both are handled below, but the
//                  map alone would turn either into a key stuck down forever.
//
//   the system state cannot miss a release, and it is the one that latched. It is
//                  global: a synthetic key-down posted by any process with no
//                  matching key-up leaves that keycode reading held forever, for
//                  every application, and nothing here can clear it. Measured on
//                  the maintainer Mac 2026-08-07: keycode 0x00 (physical A, the
//                  typing keyboard's C) read down permanently in a bare 25-line
//                  probe with nothing pressed, from HIDSystemState and from
//                  CombinedSessionState, and Carbon GetKeys agreed
//                  (tools/probe_physkey.cpp).
//
// Where a keycode is latched the AND degenerates to the event map alone, so this
// is a floor, not a guarantee — which is why starting a note does not depend on it
// (see physicalKeyWasStruck). Held-state is what ENDS a note, and there either
// source saying "up" is enough.
bool physicalKeyDown (int virtualKeyCode)
{
    if (virtualKeyCode < 0 || virtualKeyCode >= 128)
        return false;

    return testBit (gHeld, virtualKeyCode)
        && CGEventSourceKeyState (kCGEventSourceStateCombinedSessionState,
                                  static_cast<CGKeyCode> (virtualKeyCode));
}

// A note may only BEGIN on an actual key-down event. This is what makes a stuck
// bit — from either source, from any cause, present or future — unable to invent a
// note: it can at worst delay a release, never manufacture an attack. It is also
// why a very short tap still sounds, where asking "is it down now?" would already
// answer no and drop the note entirely.
bool physicalKeyWasStruck (int virtualKeyCode)
{
    if (virtualKeyCode < 0 || virtualKeyCode >= 128)
        return false;

    return testBit (gStruck, virtualKeyCode);
}

void drainPhysicalKeyStrikes()
{
    gStruck[0].store (0ull, std::memory_order_relaxed);
    gStruck[1].store (0ull, std::memory_order_relaxed);
}

void startPhysicalKeyMonitor()
{
    if (++gMonitorRefs > 1)
        return;

    // A LOCAL monitor sees only events delivered to THIS application, and it needs
    // no Input-Monitoring permission. AppKit's header: it "receives events before
    // they are dispatched by -[NSApplication sendEvent:]", so the map is already up
    // to date by the time the very same key press reaches JUCE and
    // MainPanel::keyPressed reads it. The handler returns the event untouched: it
    // observes, it never swallows.
    gMonitor = [[NSEvent addLocalMonitorForEventsMatchingMask:
                             (NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged)
                                                      handler: ^NSEvent* (NSEvent* ev)
    {
        // While Command is held there are no key-ups to record, so record nothing at
        // all and forget what came before — on BOTH edges, because a key held into
        // ⌘ and a key pressed under ⌘ each lose their release, and the second only
        // surfaces on the way back out. Taken from the event's own flags rather than
        // from a latched flag of ours, and healed by the next key event, so a
        // flagsChanged swallowed by a tracking loop cannot strand this either way.
        // A ⌘ chord is never a note anyway (plainKeyboardCommand, MainPanel).
        const bool cmd = ([ev modifierFlags] & NSEventModifierFlagCommand) != 0;
        if (cmd || gCommandDown)
        {
            gCommandDown = cmd;
            clearAll();
            if (cmd)
                return ev;
        }

        const NSEventType type = [ev type];
        if (type == NSEventTypeKeyDown || type == NSEventTypeKeyUp)
            setBits ([ev keyCode], type == NSEventTypeKeyDown);

        return ev;
    }] retain];

    // Where key-ups stop arriving. Owned here because each is a property of the
    // application, not of any one editor — a second plugin editor must never clear
    // the map the first one plays from.
    //   resign-active — keys released while another application has the keyboard.
    //   menu tracking — one of the nested event loops AppKit's header names as
    //   bypassing local monitors. Its notification carries a real cost, so state it:
    //   `object: nil` means ANY NSMenu in the process, which inside a DAW is every
    //   host menu, and clearing there cuts a typing-keyboard note the user is
    //   holding at that moment. Kept anyway — menu tracking is exactly where a
    //   key-up goes missing, and a note that stops early is recoverable where a note
    //   that never stops is not.
    //
    // Window dragging is the loop deliberately NOT covered. The obvious observer,
    // NSWindowDidMoveNotification, fires for every window in the process moving for
    // any reason — and JUCE builds each PopupMenu/ComboBox dropdown by adding a
    // window to the desktop and THEN positioning it (juce_PopupMenu.cpp), so every
    // dropdown in this UI would post one and kill any held chord. Losing a key-up to
    // a title-bar drag is the smaller harm, and it can only hang a keycode the
    // system state has latched (see physicalKeyDown); it heals on the next press of
    // that key, on a menu, or on the app going inactive.
    NSMutableArray* tokens = [[NSMutableArray alloc] init];
    observe (tokens, NSApplicationDidResignActiveNotification);
    observe (tokens, NSMenuDidEndTrackingNotification);
    gObservers = tokens;
}

void stopPhysicalKeyMonitor()
{
    if (gMonitorRefs == 0 || --gMonitorRefs > 0)
        return;

    if (gMonitor != nil)
    {
        [NSEvent removeMonitor: gMonitor];
        [gMonitor release];
        gMonitor = nil;
    }
    if (gObservers != nil)
    {
        for (id token in gObservers)
            [[NSNotificationCenter defaultCenter] removeObserver: token];
        [gObservers release];
        gObservers = nil;
    }
    gCommandDown = false;
    clearAll();
}
}
