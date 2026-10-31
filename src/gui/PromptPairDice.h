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
// set of pairs whose A and B take two given values on one axis - two different
// ones, or the same one on both sides. Each pair also carries a CONTRAST, how A
// and B stand to each other, which no tag of a single prompt can say. The dice's
// right-click menu narrows the draw to one crossing or one contrast.
namespace PromptPairs
{
struct Prompt
{
    juce::String text;
    juce::StringArray tags;   // one per axis, in Bank::axes order; "" = untagged
};

struct Pair
{
    Prompt a, b;
    juce::String contrast;    // a value key of Bank::contrast; "" = none given
};

struct Axis
{
    juce::String key, label;
    juce::StringArray valueKeys, valueLabels;
};

struct Bank
{
    std::vector<Axis> axes;
    Axis contrast;            // a property of the pair, read like an axis
    std::vector<Pair> pairs;
};

/** What the dice draws from. Unconstrained while axis < 0 and contrast is empty.
 *  A crossing: v1/v2 are value keys of `axis`, v1 not after v2 in the axis's own
 *  value order (v1 == v2 when both prompts take that value). A contrast: one value
 *  key of Bank::contrast, with axis left at -1. */
struct Filter
{
    int axis = -1;
    juce::String v1, v2;
    juce::String contrast;
    bool isNarrowed() const { return axis >= 0 || contrast.isNotEmpty(); }
    bool operator== (const Filter& o) const
    {
        return axis == o.axis && v1 == o.v1 && v2 == o.v2 && contrast == o.contrast;
    }
};

inline Axis parseAxis(const juce::var& a)
{
    Axis axis;
    axis.key = a["key"].toString();
    axis.label = a["label"].toString();
    if (auto* values = a["values"].getArray())
        for (const auto& v : *values)
        {
            // An empty key is skipped: "" is how an untagged prompt and the
            // unconstrained Filter read, so as a value it would match everything.
            const auto key = v["key"].toString();
            if (key.isEmpty()) continue;
            axis.valueKeys.add(key);
            axis.valueLabels.add(v["label"].toString());
        }
    return axis;
}

inline Bank parseBank()
{
    Bank bank;
    const auto root = juce::JSON::parse(juce::String::fromUTF8(BinaryData::t5_prompt_pairs_json,
                                                               BinaryData::t5_prompt_pairs_jsonSize));
    if (auto* axes = root["axes"].getArray())
        for (const auto& a : *axes)
            bank.axes.push_back(parseAxis(a));
    bank.contrast = parseAxis(root["contrast"]);

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
            // Same rule as a tag: a contrast the bank does not list reads as none.
            const auto contrast = p["contrast"].toString();
            Pair pair { readPrompt(p["a"]), readPrompt(p["b"]),
                        bank.contrast.valueKeys.contains(contrast) ? contrast : juce::String() };
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

/** The crossing this pair makes on `axis`, or an unconstrained Filter when
 *  either side is untagged there. */
inline Filter crossingOf(const Bank& b, const Pair& p, int axis)
{
    const auto& ta = p.a.tags[axis];
    const auto& tb = p.b.tags[axis];
    if (ta.isEmpty() || tb.isEmpty())
        return {};
    const auto& order = b.axes[(size_t) axis].valueKeys;
    return order.indexOf(ta) <= order.indexOf(tb) ? Filter { axis, ta, tb, {} }
                                                  : Filter { axis, tb, ta, {} };
}

inline bool matches(const Bank& b, const Pair& p, const Filter& f)
{
    if (f.contrast.isNotEmpty())
        return p.contrast == f.contrast;
    return f.axis < 0 || crossingOf(b, p, f.axis) == f;
}
} // namespace PromptPairs

/** The dice next to the impulse fields. Left click draws a tested pair (within
 *  the chosen crossing or contrast, never the pair it drew last - which is why
 *  the menu only offers entries of two pairs or more) and hands it to onRoll;
 *  right-click (ctrl-click on macOS) opens the menu. Paints only on
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
        g.setColour(filter_.isNarrowed() ? kOscCol.brighter(0.25f) : ink);
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
            if (PromptPairs::matches(b, b.pairs[(size_t) i], filter_) && (i != lastPair_))
                candidates.push_back(i);
        if (candidates.empty()) return;   // the menu offers no entry under two pairs

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

    static const PromptPairs::Axis& sectionOf(const PromptPairs::Filter& f)
    {
        const auto& b = PromptPairs::bank();
        return f.contrast.isNotEmpty() ? b.contrast : b.axes[(size_t) f.axis];
    }

    static juce::String filterLabel(const PromptPairs::Filter& f)
    {
        const auto& axis = sectionOf(f);
        if (f.contrast.isNotEmpty())
            return axis.valueLabels[axis.valueKeys.indexOf(f.contrast)];
        return axis.valueLabels[axis.valueKeys.indexOf(f.v1)]
             + juce::String::fromUTF8(" \xc3\x97 ")
             + axis.valueLabels[axis.valueKeys.indexOf(f.v2)];
    }

    void refreshTooltip()
    {
        const auto n = (int) PromptPairs::bank().pairs.size();
        juce::String tip = "Roll one of " + juce::String(n)
            + " tested A/B pairs into both impulses and generate it";
        if (filter_.isNarrowed())
            tip << " (" << sectionOf(filter_).label.toLowerCase()
                << ": " << filterLabel(filter_) << ")";
        tip << ". Right-click to choose which crossing or contrast it rolls.";
        if (unavailableReason_.isNotEmpty())
            tip << " Unavailable now: " << unavailableReason_ << ".";
        setTooltip(tip);
    }

    void showCrossingMenu()
    {
        const auto& b = PromptPairs::bank();
        juce::PopupMenu m;
        std::vector<PromptPairs::Filter> ids { PromptPairs::Filter {} };   // id 1 = any
        m.addItem(1, "Any tested pair (" + juce::String((int) b.pairs.size()) + ")",
                  true, ! filter_.isNarrowed());

        // An entry that holds a single pair is left out everywhere: rolling it
        // would draw the same pair every time, which is no roll at all.
        auto addIfRollable = [&](const PromptPairs::Filter& f)
        {
            int count = 0;
            for (const auto& p : b.pairs)
                if (PromptPairs::matches(b, p, f)) ++count;
            if (count < kMinPairsPerEntry) return;
            ids.push_back(f);
            m.addItem((int) ids.size(), filterLabel(f) + "  (" + juce::String(count) + ")",
                      true, filter_ == f);
        };

        for (int ax = 0; ax < (int) b.axes.size(); ++ax)
        {
            // Every crossing on this axis, in the axis's value order; j starts at
            // i so that a value crossed with itself (nature x nature) is offered too.
            const auto& order = b.axes[(size_t) ax].valueKeys;
            m.addSectionHeader(b.axes[(size_t) ax].label);
            for (int i = 0; i < order.size(); ++i)
                for (int j = i; j < order.size(); ++j)
                    addIfRollable({ ax, order[i], order[j], {} });
        }

        if (! b.contrast.valueKeys.isEmpty())
        {
            m.addSectionHeader(b.contrast.label);
            for (const auto& key : b.contrast.valueKeys)
            {
                PromptPairs::Filter f;
                f.contrast = key;
                addIfRollable(f);
            }
        }

        juce::Component::SafePointer<PromptPairDice> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                        [safe, ids](int id)
                        {
                            if (safe == nullptr || id <= 0 || id > (int) ids.size()) return;
                            safe->filter_ = ids[(size_t) id - 1];
                            safe->refreshTooltip();
                            safe->repaint();
                        });
    }

    static constexpr int kMinPairsPerEntry = 2;
    PromptPairs::Filter filter_;   // session-only, like the panel's other view state
    juce::String unavailableReason_;
    int lastPair_ = -1;
    int face_ = 5;
};
