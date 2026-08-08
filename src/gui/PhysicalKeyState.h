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

// True if a real key-DOWN event for this key has arrived and no pass has drained it
// yet. A note may only BEGIN on this; physicalKeyDown only ever
// ENDS one. That split is the point: a key stuck "down" — by either source, from
// any cause — can then at worst delay a release, never manufacture an attack.
//
// On macOS this is one strike per physical keystroke, taken from the event stream:
// OS auto-repeat arrives as further key-DOWNs with no key-up between them and is
// excluded, and so is a Command/Control/Option chord, which is never a note. A
// strike on a key that is already sounding therefore means the player released it
// and hit it again, and nothing else.
bool physicalKeyWasStruck (int virtualKeyCode);

// TRUE only where the above holds. Elsewhere there is no key-event stream to draw
// on and a strike degrades to "the key reads down", which repeats for as long as it
// is held — so the caller must supply the edge FROM ITS OWN per-instance state.
// It has to be the caller's: an edge kept here would be process-global, and two
// plugin editors polling it would mark each other's keystrokes as already handled
// and swallow them. That is not hypothetical — it is the same shape as the bug that
// made a second editor wipe the key map the first one was playing from.
bool physicalKeyStrikesAreEvents();

// End of a pass: a strike that has been acted on must not be acted on twice. Runs
// on EVERY pass, the release-only poll included, so a strike no pass could use is
// swept rather than left waiting to become an attack nobody played. Nothing to do
// where strikes are not events — there a strike holds no state.
void drainPhysicalKeyStrikes();

// A state in which nothing may become a note at all — the typing keyboard switched
// off, an overlay up, the window not frontmost. Drops everything pending, so a key
// held THROUGH such a state is not read as a fresh keystroke on the way out of it.
// Likewise nothing to do where strikes are not events.
void discardPhysicalKeyStrikes();

// Starts/stops the key-event monitor that feeds physicalKeyDown (macOS only;
// no-ops elsewhere). Reference-counted and message-thread only, so several plugin
// editors inside one host share a single monitor.
//
// The map mirrors ONE thing: which keys this application currently has down. It is
// not a place to express per-editor policy — a second editor deciding it does not
// want typing-keyboard notes must not wipe the key state the first one is playing
// from. What the map forgets, and when, is therefore owned here and not by any
// caller: the app going inactive, Command being held, and menu tracking ending.
// The last of those costs something (it fires whether or not a key-up was actually
// lost, so a note held across a menu stops) and the .mm says why it is still right.
void startPhysicalKeyMonitor();
void stopPhysicalKeyMonitor();
}
