#pragma once

namespace t5
{
// True if the given macOS virtual keycode (kVK_ANSI_*, the physical key POSITION)
// is currently held. Layout-INDEPENDENT on macOS (identifies keys by position, not
// by the character they produce); falls back to JUCE's layout-dependent character
// state elsewhere.
//
// On macOS this is true only when BOTH of two independent sources agree: an
// app-local map fed by this application's own key EVENTS says we saw the key go
// down, AND the system key state says it is down right now. Neither source is
// trustworthy alone, and both fail the same way — a key that never comes back up,
// i.e. a note that never stops. The .mm carries the full argument and the
// measurement behind it; changing physicalKeyDown to rely on one of them again is
// the bug this exists to prevent.
bool physicalKeyDown (int virtualKeyCode);

// True if a real key-DOWN event for this key has arrived and no note-starting pass
// has consumed it yet. A note may only BEGIN on this; physicalKeyDown only ever
// ENDS one. That split is the point: a key stuck "down" — by either source, from
// any cause — can then at worst delay a release, never manufacture an attack.
// Elsewhere this falls back to physicalKeyDown, which is the current behaviour.
bool physicalKeyWasStruck (int virtualKeyCode);

// Forgets all pending strikes. Called at the end of a pass that was allowed to
// start notes; no-op where there is nothing to consume.
void drainPhysicalKeyStrikes();

// Starts/stops the key-event monitor that feeds physicalKeyDown (macOS only;
// no-ops elsewhere). Reference-counted and message-thread only, so several plugin
// editors inside one host share a single monitor.
//
// The map mirrors ONE thing: which keys this application currently has down. It is
// not a place to express per-editor policy — a second editor deciding it does not
// want typing-keyboard notes must not wipe the key state the first one is playing
// from. The map forgets keys only where key-ups genuinely stop arriving, and both
// of those are properties of the whole application, so they are owned here rather
// than by any caller: the app going inactive, and Command being held (see the .mm).
void startPhysicalKeyMonitor();
void stopPhysicalKeyMonitor();
}
