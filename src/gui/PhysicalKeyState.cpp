#include "PhysicalKeyState.h"

// macOS lives in PhysicalKeyStateMac.mm: it needs AppKit, and its key map is fed
// by this application's own key EVENTS instead of the system key state, which is
// global and can latch a key "down" forever (see PhysicalKeyState.h).
#if ! defined(__APPLE__)

#include <JuceHeader.h>
#include <cstdint>

namespace t5
{
// Map a macOS physical keycode back to its US-QWERTY label, then use JUCE's
// character-based key state. Layout-dependent on these platforms (macOS is the
// primary target; a Win/X11 scancode path can replace this later).
static juce_wchar physicalToAscii (int vk)
{
    switch (vk)
    {
        case 0x00: return 'a'; case 0x01: return 's'; case 0x02: return 'd';
        case 0x03: return 'f'; case 0x04: return 'h'; case 0x05: return 'g';
        case 0x06: return 'z'; case 0x07: return 'x'; case 0x0D: return 'w';
        case 0x0E: return 'e'; case 0x10: return 'y'; case 0x11: return 't';
        case 0x1E: return ']'; case 0x1F: return 'o'; case 0x20: return 'u';
        case 0x21: return '['; case 0x23: return 'p'; case 0x25: return 'l';
        case 0x26: return 'j'; case 0x27: return '\''; case 0x28: return 'k';
        case 0x29: return ';'; case 0x2A: return '\\';
        default:   return 0;
    }
}

bool physicalKeyDown (int virtualKeyCode)
{
    const juce_wchar ch = physicalToAscii (virtualKeyCode);
    if (ch == 0)
        return false;
    return juce::KeyPress::isKeyCurrentlyDown ((int) juce::CharacterFunctions::toLowerCase (ch))
        || juce::KeyPress::isKeyCurrentlyDown ((int) juce::CharacterFunctions::toUpperCase (ch));
}

// No key EVENTS to draw on here, so a strike can only be "the key reads down", and
// it therefore repeats for as long as the key is held. Whoever calls it owns the
// edge — see physicalKeyStrikesAreEvents in the header for why that edge may not
// live here. Nothing in this file keeps state, deliberately: every attempt to hold
// the edge process-wide (a snapshot at each drain, a mark set on first use) either
// swallowed a keystroke that arrived in the gap between GetAsyncKeyState going true
// and the key message being dispatched, or let a second plugin editor's 20 Hz poll
// mark the keystroke the first one was about to play.
bool physicalKeyWasStruck (int virtualKeyCode)  { return physicalKeyDown (virtualKeyCode); }
bool physicalKeyStrikesAreEvents()              { return false; }
void drainPhysicalKeyStrikes()                  {}
void discardPhysicalKeyStrikes()                {}

// No monitor here — but not because this branch is safe. On Windows JUCE's
// isKeyCurrentlyDown goes to GetAsyncKeyState, which is the same kind of global
// key state macOS had to stop trusting, so a key held in another application is
// visible here too. Untouched for now: this path is already layout-DEPENDENT and
// needs its own scancode rewrite, and that is where to fix both at once. Linux is
// not the exception it looks like: JUCE keeps its own key table there, but it
// selects KeymapStateMask and memcpy's the SERVER's key_vector into that table on
// every KeymapNotify — which X sends after every FocusIn and every EnterNotify — so
// simply moving the pointer into the window republishes whatever the whole machine
// is holding. A key held in another window can therefore begin a note here, exactly
// as on Windows. Both are pre-existing and unchanged; both end with that rewrite.
void startPhysicalKeyMonitor() {}
void stopPhysicalKeyMonitor()  {}
}

#endif
