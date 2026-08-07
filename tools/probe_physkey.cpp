// Isolated empirical probe for CGEventSourceKeyState — the API behind
// t5::physicalKeyDown (PhysicalKeyState.cpp, added in a9376a83).
//
// Observation from v1: keycode 0x00 (kVK_ANSI_A) reads DOWN with nothing
// pressed, and never clears — while 0x01/0x0D/... read correctly. This probe
// pins that down: it snapshots a range of keycodes for BOTH source states
// (HIDSystemState vs CombinedSessionState) so we can see (a) whether keycode 0
// is uniquely stuck-true, and (b) whether one source state is reliable.
//
// Build: clang++ -std=c++17 -O0 -o probe tools/probe_physkey.cpp -framework CoreGraphics
// (this variant calls CGEventSourceKeyState directly — no PhysicalKeyState.cpp).
//
// ANSWERED, 2026-08-07. (a) keycode 0x00 is uniquely stuck-true: a sweep of
// 0x00…0x7F reports exactly one key down with nothing pressed. (b) NEITHER source
// state is reliable — HIDSystemState and CombinedSessionState both say DN, and
// Carbon GetKeys() agrees, so all three read one shared, global, latchable map
// that this process cannot clear. The synth therefore stopped reading system key
// state altogether: t5::physicalKeyDown is now fed by the app's own AppKit key
// events (PhysicalKeyStateMac.mm). This probe stays as the measurement.

#include <CoreGraphics/CoreGraphics.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

static bool keyDown (CGEventSourceStateID src, int vk)
{
    return CGEventSourceKeyState (src, (CGKeyCode) vk);
}

int main (int argc, char** argv)
{
    int seconds = 6;
    if (argc > 1) seconds = std::atoi (argv[1]);

    // Note-relevant keycodes plus a couple of neighbours.
    const int codes[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0D, 0x0E };
    const int n = (int) (sizeof (codes) / sizeof (codes[0]));

    std::printf ("keycode |  HID  | Combined   (baseline snapshot, nothing pressed)\n");
    for (int i = 0; i < n; ++i)
    {
        const bool hid = keyDown (kCGEventSourceStateHIDSystemState, codes[i]);
        const bool comb = keyDown (kCGEventSourceStateCombinedSessionState, codes[i]);
        std::printf ("  0x%02X  |  %s  |  %s\n", codes[i],
                     hid ? "DN" : "..", comb ? "DN" : "..");
    }
    std::printf ("\nNow watching 0x00(A) 0x01(S) 0x0D(W) for %ds — press/release to test.\n",
                 seconds);
    std::fflush (stdout);

    struct W { int vk; const char* l; bool hid, comb; };
    W w[] = { {0x00,"A",false,false}, {0x01,"S",false,false}, {0x0D,"W",false,false} };
    const int wn = 3;
    for (int t = 0; t < seconds * 50; ++t)
    {
        for (int k = 0; k < wn; ++k)
        {
            const bool hid = keyDown (kCGEventSourceStateHIDSystemState, w[k].vk);
            const bool comb = keyDown (kCGEventSourceStateCombinedSessionState, w[k].vk);
            if (hid != w[k].hid || comb != w[k].comb)
            {
                std::printf ("[t=%5.2f] %s HID=%s Combined=%s\n", t / 50.0, w[k].l,
                             hid ? "DN" : "..", comb ? "DN" : "..");
                std::fflush (stdout);
                w[k].hid = hid; w[k].comb = comb;
            }
        }
        usleep (20000);
    }
    std::printf ("done.\n");
    return 0;
}
