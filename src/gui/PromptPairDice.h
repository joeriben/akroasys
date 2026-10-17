#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>
#include "BinaryData.h"
#include "GuiHelpers.h"

// ── Tested A/B impulse pairs + the dice that rolls one ─────────────────────
// The bank is resources/t5_prompt_pairs.json, compiled into BinaryData so it
// is there without any library download. Every pair in it was rendered in the
// built standalone across the mixing range (nine A/B positions, two seeds) and
// passed; the dice only ever draws from that list. Each prompt carries one tag
// per axis (time / material / source, "" = no value fits), and a CROSSING is the
// set of pairs whose A and B take two different values on one axis — that is
// what the dice's right-click menu narrows the draw to.
namespace PromptPairs
{
struct Prompt
{
    juce::String text;
    juce::StringArray tags;   // one per axis, in Bank::axes order; "" = untagged
};

struct Pair { Prompt a, b; };

struct Axis
{
    juce::String key, label;
    juce::StringArray valueKeys, valueLabels;
};

struct Bank
{
    std::vector<Axis> axes;
    std::vector<Pair> pairs;
};

/** axis < 0: no constraint. Otherwise v1/v2 are two value keys of that axis,
 *  v1 before v2 in the axis's own value order. */
struct Crossing
{
    int axis = -1;
    juce::String v1, v2;
    bool operator== (const Crossing& o) const { return axis == o.axis && v1 == o.v1 && v2 == o.v2; }
};

inline Bank parseBank()
{
    Bank bank;
    const auto root = juce::JSON::parse(juce::String::fromUTF8(BinaryData::t5_prompt_pairs_json,
                                                               BinaryData::t5_prompt_pairs_jsonSize));
    if (auto* axes = root["axes"].getArray())
        for (const auto& a : *axes)
        {
            Axis axis;
            axis.key = a["key"].toString();
            axis.label = a["label"].toString();
            if (auto* values = a["values"].getArray())
                for (const auto& v : *values)
                {
                    axis.valueKeys.add(v["key"].toString());
                    axis.valueLabels.add(v["label"].toString());
                }
            bank.axes.push_back(std::move(axis));
        }

    auto readPrompt = [&bank](const juce::var& p)
    {
        Prompt prompt;
        prompt.text = p["text"].toString().trim();
        for (const auto& axis : bank.axes)
        {
            // A tag the axis does not list reads as untagged, so a typo in the
            // JSON can only drop a pair out of a crossing, never invent one.
            const auto tag = p[juce::Identifier(axis.key)].toString();
            prompt.tags.add(axis.valueKeys.contains(tag) ? tag : juce::String());
        }
        return prompt;
    };

    if (auto* pairs = root["pairs"].getArray())
        for (const auto& p : *pairs)
        {
            Pair pair { readPrompt(p["a"]), readPrompt(p["b"]) };
            if (pair.a.text.isNotEmpty() && pair.b.text.isNotEmpty())
                bank.pairs.push_back(std::move(pair));
        }
    return bank;
}

/** Parsed once, on first use. Message thread only (the dice is its only user). */
inline const Bank& bank()
{
    static const Bank b = parseBank();
    return b;
}

/** The crossing this pair makes on `axis`, or an unconstrained Crossing when
 *  either side is untagged there or both take the same value. */
inline Crossing crossingOf(const Bank& b, const Pair& p, int axis)
{
    const auto& ta = p.a.tags[axis];
    const auto& tb = p.b.tags[axis];
    if (ta.isEmpty() || tb.isEmpty() || ta == tb)
        return {};
    const auto& order = b.axes[(size_t) axis].valueKeys;
    return order.indexOf(ta) < order.indexOf(tb) ? Crossing { axis, ta, tb }
                                                 : Crossing { axis, tb, ta };
}

inline bool matches(const Bank& b, const Pair& p, const Crossing& c)
{
    return c.axis < 0 || crossingOf(b, p, c.axis) == c;
}
} // namespace PromptPairs

/** The dice next to the impulse fields. Left click draws a tested pair (within
 *  the chosen crossing, never the pair it drew last - which is why the menu only
 *  offers crossings of two pairs or more) and hands it to onRoll;
 *  right-click (ctrl-click on macOS) opens the crossing menu. Paints only on
 *  hover/press/roll — no timer. */
class PromptPairDice : public juce::Button
{
public:
    PromptPairDice() : juce::Button("Roll a tested pair")
    {
        setWantsKeyboardFocus(false);
        refreshTooltip();
    }

    /** Called on the message thread with the drawn pair. Returns false when the
     *  owner cannot take a pair right now; the dice then keeps its last draw. */
    std::function<bool(const PromptPairs::Pair&)> onRoll;

    /** Non-empty while a roll would be refused: the dice dims and its tooltip
     *  names the reason. Dimmed, not disabled - the crossing menu stays open to
     *  a right-click, so the next roll can be set up while this one waits. */
    void setUnavailableReason(const juce::String& reason)
    {
        if (reason == unavailableReason_)
            return;
        unavailableReason_ = reason;
        setAlpha(reason.isEmpty() ? 1.0f : 0.3f);
        refreshTooltip();
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        const float side = (float) juce::jmin(getWidth(), getHeight()) - 2.0f;
        if (side <= 4.0f) return;
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre(side, side);
        if (down) r = r.reduced(0.5f);

        const auto ink = highlighted ? kTextPrimary : kDim;
        g.setColour(ink.withAlpha(highlighted ? 0.18f : 0.10f));
        g.fillRoundedRectangle(r, side * 0.18f);
        g.setColour(ink);
        g.drawRoundedRectangle(r, side * 0.18f, juce::jmax(1.0f, side * 0.07f));

        // Pip grid on thirds of the inner face; a narrowed draw shows its pips in
        // the oscillator accent so the constraint is visible without a label.
        static constexpr int kPips[7][7][2] = {
            {}, {{1,1}}, {{0,0},{2,2}}, {{0,0},{1,1},{2,2}},
            {{0,0},{2,0},{0,2},{2,2}}, {{0,0},{2,0},{1,1},{0,2},{2,2}},
            {{0,0},{2,0},{0,1},{2,1},{0,2},{2,2}} };
        const auto inner = r.reduced(side * 0.22f);
        const float cell = inner.getWidth() / 2.0f;
        const float pip = juce::jmax(1.6f, side * 0.15f);
        g.setColour(crossing_.axis >= 0 ? kOscCol.brighter(0.25f) : ink);
        for (int i = 0; i < face_; ++i)
        {
            const float cx = inner.getX() + cell * (float) kPips[face_][i][0];
            const float cy = inner.getY() + cell * (float) kPips[face_][i][1];
            g.fillEllipse(cx - pip * 0.5f, cy - pip * 0.5f, pip, pip);
        }
    }

private:
    void clicked(const juce::ModifierKeys& mods) override
    {
        if (mods.isPopupMenu()) showCrossingMenu();
        else                    roll();
    }

    void roll()
    {
        const auto& b = PromptPairs::bank();
        std::vector<int> candidates;
        for (int i = 0; i < (int) b.pairs.size(); ++i)
            if (PromptPairs::matches(b, b.pairs[(size_t) i], crossing_) && (i != lastPair_))
                candidates.push_back(i);
        if (candidates.empty()) return;   // the menu offers no crossing under two pairs

        auto& rng = juce::Random::getSystemRandom();
        const int pick = candidates[(size_t) rng.nextInt((int) candidates.size())];
        if (onRoll == nullptr || ! onRoll(b.pairs[(size_t) pick]))
            return;

        lastPair_ = pick;
        int nextFace = 1 + rng.nextInt(5);   // 1..5, then shifted past the current face
        if (nextFace >= face_) ++nextFace;
        face_ = nextFace;
        repaint();
    }

    juce::String crossingLabel(const PromptPairs::Crossing& c) const
    {
        const auto& axis = PromptPairs::bank().axes[(size_t) c.axis];
        return axis.valueLabels[axis.valueKeys.indexOf(c.v1)]
             + juce::String::fromUTF8(" \xc3\x97 ")
             + axis.valueLabels[axis.valueKeys.indexOf(c.v2)];
    }

    void refreshTooltip()
    {
        const auto n = (int) PromptPairs::bank().pairs.size();
        juce::String tip = "Roll one of " + juce::String(n)
            + " tested A/B pairs into both impulses and generate it";
        if (crossing_.axis >= 0)
            tip << " (" << PromptPairs::bank().axes[(size_t) crossing_.axis].label.toLowerCase()
                << ": " << crossingLabel(crossing_) << ")";
        tip << ". Right-click to choose which crossing it rolls.";
        if (unavailableReason_.isNotEmpty())
            tip << " Unavailable now: " << unavailableReason_ << ".";
        setTooltip(tip);
    }

    void showCrossingMenu()
    {
        const auto& b = PromptPairs::bank();
        juce::PopupMenu m;
        std::vector<PromptPairs::Crossing> ids { PromptPairs::Crossing {} };   // id 1 = any
        m.addItem(1, "Any tested pair (" + juce::String((int) b.pairs.size()) + ")",
                  true, crossing_.axis < 0);

        for (int ax = 0; ax < (int) b.axes.size(); ++ax)
        {
            // Every crossing on this axis that holds at least two pairs, in the
            // axis's value order. A single-pair crossing is left out: rolling it
            // would draw the same pair every time, which is no roll at all.
            const auto& order = b.axes[(size_t) ax].valueKeys;
            m.addSectionHeader(b.axes[(size_t) ax].label);
            for (int i = 0; i < order.size(); ++i)
                for (int j = i + 1; j < order.size(); ++j)
                {
                    const PromptPairs::Crossing c { ax, order[i], order[j] };
                    int count = 0;
                    for (const auto& p : b.pairs)
                        if (PromptPairs::matches(b, p, c)) ++count;
                    if (count < kMinPairsPerCrossing) continue;
                    ids.push_back(c);
                    m.addItem((int) ids.size(), crossingLabel(c) + "  (" + juce::String(count) + ")",
                              true, crossing_ == c);
                }
        }

        juce::Component::SafePointer<PromptPairDice> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                        [safe, ids](int id)
                        {
                            if (safe == nullptr || id <= 0 || id > (int) ids.size()) return;
                            safe->crossing_ = ids[(size_t) id - 1];
                            safe->refreshTooltip();
                            safe->repaint();
                        });
    }

    static constexpr int kMinPairsPerCrossing = 2;
    PromptPairs::Crossing crossing_;   // session-only, like the panel's other view state
    juce::String unavailableReason_;
    int lastPair_ = -1;
    int face_ = 5;
};
