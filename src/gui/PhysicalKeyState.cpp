#include "PhysicalKeyState.h"

// macOS lives in PhysicalKeyStateMac.mm: it needs AppKit, and its key map is fed
// by this application's own key EVENTS instead of the system key state, which is
// global and can latch a key "down" forever (see PhysicalKeyState.h).
#if ! defined(__APPLE__)

#include <JuceHeader.h>

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

// No separate strike channel here: JUCE's key state is all these platforms offer,
// so a note begins on "the key reads down", exactly as it did before.
bool physicalKeyWasStruck (int virtualKeyCode) { return physicalKeyDown (virtualKeyCode); }
void drainPhysicalKeyStrikes() {}

// No monitor here — but not because this branch is safe. On Windows JUCE's
// isKeyCurrentlyDown goes to GetAsyncKeyState, which is the same kind of global
// key state macOS had to stop trusting, so a key held in another application is
// visible here too. Untouched for now: this path is already layout-DEPENDENT and
// needs its own scancode rewrite, and that is where to fix both at once. Linux
// (X11, per-peer tracking) is app-local and does not have the problem.
void startPhysicalKeyMonitor() {}
void stopPhysicalKeyMonitor()  {}
}

#endif
