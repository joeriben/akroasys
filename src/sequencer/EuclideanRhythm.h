#pragma once
#include <array>
#include <algorithm>

/**
 * Euclidean rhythm generator (Bjorklund algorithm).
 *
 * Distributes `pulses` onsets as evenly as possible across `steps` slots,
 * then applies a circular `rotation` offset.
 *
 * Based on: Toussaint, "The Euclidean Algorithm Generates Traditional
 * Musical Rhythms" (2005).
 */
namespace EuclideanRhythm
{

static constexpr int MAX_STEPS = 64;

/**
 * Generate a Euclidean rhythm pattern.
 *
 * @param steps    Total number of steps (1–64)
 * @param pulses   Number of active beats (0–steps)
 * @param rotation Circular shift applied after generation (0–steps-1)
 * @return         Fixed-size array; indices [0, steps) contain the pattern,
 *                 indices [steps, 64) are false.
 */
inline std::array<bool, MAX_STEPS> generate(int steps, int pulses, int rotation)
{
    std::array<bool, MAX_STEPS> result{};

    if (steps <= 0) return result;

    pulses = std::max(0, std::min(pulses, steps));
    if (steps > 0) rotation = ((rotation % steps) + steps) % steps;

    if (pulses == 0) return result;
    if (pulses == steps)
    {
        for (int i = 0; i < steps; ++i) result[static_cast<size_t>(i)] = true;
        return result;
    }

    // Bjorklund's algorithm — iterative grouping
    // We build the pattern in a flat work array, tracking group boundaries.
    // Equivalent to the Euclidean GCD string-construction.

    // Use Bresenham-style approach (simpler, equivalent result):
    // Place a pulse at step i if floor((i+1)*pulses/steps) > floor(i*pulses/steps)
    std::array<bool, MAX_STEPS> raw{};
    for (int i = 0; i < steps; ++i)
        raw[static_cast<size_t>(i)] =
            ((i + 1) * pulses / steps) > (i * pulses / steps);

    // Apply rotation (circular shift right by `rotation`)
    for (int i = 0; i < steps; ++i)
        result[static_cast<size_t>(i)] =
            raw[static_cast<size_t>((i - rotation + steps) % steps)];

    return result;
}

/**
 * The same Euclidean distribution with the onset FIRST in each group: rotation 0
 * puts a pulse on step 0 (16/4 → 0 4 8 12, 8/3 → 0 3 6 = tresillo, 16/4
 * rotated by 2 → 2 6 10 14). generate() above places each onset at the END of
 * its group (16/4 → 3 7 11 15); the Generative sequencer is voiced on that and
 * keeps it. The Phrase sequencer needs a downbeat at rotation 0, because its
 * Bass rule plays the root on the first head (docs/PHRASE_SEQ_CONCEPT.md §3.5).
 * Rotation shifts right, as in generate().
 */
inline std::array<bool, MAX_STEPS> generateOnsetFirst(int steps, int pulses, int rotation)
{
    std::array<bool, MAX_STEPS> result{};
    if (steps <= 0) return result;
    steps = std::min(steps, MAX_STEPS);
    pulses = std::max(0, std::min(pulses, steps));
    rotation = ((rotation % steps) + steps) % steps;
    for (int i = 0; i < steps; ++i)
        if ((i * pulses) % steps < pulses)
            result[static_cast<size_t>((i + rotation) % steps)] = true;
    return result;
}

} // namespace EuclideanRhythm
