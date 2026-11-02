#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <functional>
#include <vector>
#include "BinaryData.h"
#include "GuiHelpers.h"

// ── Word pools + the dice that rolls an A/B pair out of them ──────────────────
// The pools are resources/t5_prompt_pairs.json, compiled into BinaryData so they
// are there without any library download. A CATEGORY names the pools each side
// draws from; a roll takes one category, draws one entry per side and lets the
// same roll decide which side becomes A. Nothing is a fixed pair: the
// combination is new on every roll. Two shapes need more than two pools:
// "one attribute changed" puts one of two contrasting words in front of ONE
// drawn source on each side, and "groove" appends one drawn tempo to both.
// The dice's right-click menu narrows the roll to one category.
namespace PromptPairs
{
struct Pair
{
    juce::String a, b;
};

struct Pool
{
    juce::String key, suffix;   // the suffix goes after every entry drawn from it
    juce::StringArray entries;
};

struct Category
{
    juce::String key, label;
    std::vector<int> a, b;      // pool indices per side ("one attribute changed": unused)
    std::vector<int> sources;   // "one attribute changed": where the shared source comes from
    std::vector<std::pair<juce::String, juce::String>> words;   // ... and its contrasting words
    juce::Array<int> tempos;    // non-empty: one drawn per roll, appended to both sides
    bool rollable = false;      // can draw at least two different pairs
};

struct Bank
{
    std::vector<Pool> pools;
    std::vector<Category> categories;
};

/** A side with several pools picks one of them first, each equally likely,
 *  then an entry - so a small pool is not drowned by a large one next to it. */
inline juce::String drawFrom(const Bank& bank, const std::vector<int>& poolIds, juce::Random& rng)
{
    const auto& pool = bank.pools[(size_t) poolIds[(size_t) rng.nextInt((int) poolIds.size())]];
    return pool.entries[rng.nextInt(pool.entries.size())] + pool.suffix;
}

/** One pair from category `c`, sides not yet decided. `c` must be rollable. */
inline Pair drawPair(const Bank& bank, const Category& c, juce::Random& rng)
{
    Pair p;
    if (! c.words.empty())
    {
        const auto& pool = bank.pools[(size_t) c.sources[(size_t) rng.nextInt((int) c.sources.size())]];
        const auto source = pool.entries[rng.nextInt(pool.entries.size())] + pool.suffix;
        const auto& w = c.words[(size_t) rng.nextInt((int) c.words.size())];
        p = { w.first + " " + source, w.second + " " + source };
    }
    else
    {
        p = { drawFrom(bank, c.a, rng), drawFrom(bank, c.b, rng) };
    }
    if (! c.tempos.isEmpty())
    {
        const auto tempo = ", " + juce::String(c.tempos[rng.nextInt(c.tempos.size())]) + "bpm";
        p.a << tempo;
        p.b << tempo;
    }
    return p;
}

/** True when the category can produce two different pairs, so that a roll can
 *  always avoid repeating the last one. Counted the way roll() compares: an
 *  entry cannot face itself, and A/B swapped is the same pair. */
inline bool canDrawTwoPairs(const Bank& bank, const Category& c)
{
    juce::StringArray seen;
    auto distinctPairs = [&seen](const juce::String& x, const juce::String& y)
    {
        if (x != y)
            seen.addIfNotAlreadyThere(x < y ? x + "\n" + y : y + "\n" + x);
        return seen.size() >= 2;
    };
    auto sideTexts = [&bank](const std::vector<int>& ids)
    {
        juce::StringArray texts;
        for (int id : ids)
            for (const auto& e : bank.pools[(size_t) id].entries)
                texts.addIfNotAlreadyThere(e + bank.pools[(size_t) id].suffix);
        return texts;
    };

    if (! c.words.empty())
    {
        for (const auto& source : sideTexts(c.sources))
            for (const auto& w : c.words)
                if (distinctPairs(w.first + " " + source, w.second + " " + source))
                    return true;
        return false;
    }
    const auto sideA = sideTexts(c.a), sideB = sideTexts(c.b);
    for (const auto& x : sideA)
        for (const auto& y : sideB)
            if (distinctPairs(x, y))
                return true;
    return false;
}

inline Bank parseBank()
{
    Bank bank;
    const auto root = juce::JSON::parse(juce::String::fromUTF8(BinaryData::t5_prompt_pairs_json,
                                                               BinaryData::t5_prompt_pairs_jsonSize));
    // Every text that reaches a prompt, a menu or a key match is read through
    // this: a value of another type is empty, never stringified into the prompt.
    auto text = [](const juce::var& v) { return v.isString() ? v.toString() : juce::String(); };

    if (auto* pools = root["pools"].getArray())
        for (const auto& p : *pools)
        {
            Pool pool;
            pool.key = text(p["key"]).trim();
            pool.suffix = text(p["suffix"]);
            if (auto* entries = p["entries"].getArray())
                for (const auto& e : *entries)
                    if (const auto entry = text(e).trim(); entry.isNotEmpty())
                        pool.entries.addIfNotAlreadyThere(entry);
            if (pool.key.isNotEmpty())
                bank.pools.push_back(std::move(pool));
        }

    // A pool key the file does not define, or a pool with no entries, is left
    // out of the side that names it, so a typo can only shrink a category, and
    // drawFrom never meets an empty pool. A key is matched to the first pool of
    // that name with entries, and named twice it counts once, so it cannot
    // outweigh the side's other pools.
    auto resolve = [&bank, &text](const juce::var& keys)
    {
        std::vector<int> ids;
        if (auto* arr = keys.getArray())
            for (const auto& k : *arr)
                for (int i = 0; i < (int) bank.pools.size(); ++i)
                    if (bank.pools[(size_t) i].key == text(k).trim() && ! bank.pools[(size_t) i].entries.isEmpty())
                    {
                        if (std::find(ids.begin(), ids.end(), i) == ids.end())
                            ids.push_back(i);
                        break;
                    }
        return ids;
    };

    if (auto* categories = root["categories"].getArray())
        for (const auto& c : *categories)
        {
            Category cat;
            cat.key = text(c["key"]).trim();
            cat.label = text(c["label"]).trim();
            cat.a = resolve(c["a"]);
            cat.b = resolve(c["b"]);
            cat.sources = resolve(c["source"]);
            if (auto* words = c["words"].getArray())
                for (const auto& w : *words)
                {
                    // Only a list of two strings is a word pair. var's index operator
                    // does not check its bounds, so the shape is tested before it.
                    const auto* pair = w.getArray();
                    if (pair == nullptr || pair->size() != 2)
                        continue;
                    const auto first = text(pair->getReference(0)).trim();
                    const auto second = text(pair->getReference(1)).trim();
                    if (first.isNotEmpty() && second.isNotEmpty() && first != second)
                        cat.words.emplace_back(first, second);
                }
            // A tempo is a number of beats per minute in a playable range, rounded
            // to a whole one; anything else (a bool, a string, a huge double) is skipped.
            if (auto* tempos = c["tempo"].getArray())
                for (const auto& t : *tempos)
                    if (t.isInt() || t.isInt64() || t.isDouble())
                        if (const auto bpm = (double) t; bpm >= 20.0 && bpm <= 400.0)
                            cat.tempos.addIfNotAlreadyThere(juce::roundToInt(bpm));

            const bool shaped = cat.words.empty() ? (! cat.a.empty() && ! cat.b.empty())
                                                  : ! cat.sources.empty();
            cat.rollable = shaped && cat.label.isNotEmpty() && canDrawTwoPairs(bank, cat);
            bank.categories.push_back(std::move(cat));
        }
    return bank;
}

/** Parsed once, on first use. Message thread only (the dice is its only user). */
inline const Bank& bank()
{
    static const Bank b = parseBank();
    return b;
}
} // namespace PromptPairs

/** The dice next to the impulse fields. Left click rolls a pair from the chosen
 *  category (or, with none chosen, from a category drawn first) and hands it to
 *  onRoll, never the pair it rolled last; right-click (ctrl-click on macOS)
 *  opens the category menu. Paints only on hover/press/roll — no timer. */
class PromptPairDice : public juce::Button
{
public:
    PromptPairDice() : juce::Button("Roll a pair")
    {
        setWantsKeyboardFocus(false);
        refreshTooltip();
    }

    /** Called on the message thread with the rolled pair. Returns false when the
     *  owner cannot take a pair right now; the dice then keeps its last roll. */
    std::function<bool(const PromptPairs::Pair&)> onRoll;

    /** Non-empty while a roll would be refused: the dice dims and its tooltip
     *  names the reason. Dimmed, not disabled - the category menu stays open to
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

        // Pip grid on thirds of the inner face; a chosen category shows its pips
        // in the oscillator accent so the constraint is visible without a label.
        static constexpr int kPips[7][7][2] = {
            {}, {{1,1}}, {{0,0},{2,2}}, {{0,0},{1,1},{2,2}},
            {{0,0},{2,0},{0,2},{2,2}}, {{0,0},{2,0},{1,1},{0,2},{2,2}},
            {{0,0},{2,0},{0,1},{2,1},{0,2},{2,2}} };
        const auto inner = r.reduced(side * 0.22f);
        const float cell = inner.getWidth() / 2.0f;
        const float pip = juce::jmax(1.6f, side * 0.15f);
        g.setColour(category_ >= 0 ? kOscCol.brighter(0.25f) : ink);
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
        if (mods.isPopupMenu()) showCategoryMenu();
        else                    roll();
    }

    void roll()
    {
        const auto& b = PromptPairs::bank();
        std::vector<int> rollable;
        for (int i = 0; i < (int) b.categories.size(); ++i)
            if (b.categories[(size_t) i].rollable) rollable.push_back(i);
        if (rollable.empty()) return;

        auto& rng = juce::Random::getSystemRandom();
        auto pickCategory = [&]
        {
            return category_ >= 0 ? category_ : rollable[(size_t) rng.nextInt((int) rollable.size())];
        };

        // A draw that puts one text on both sides, or repeats the last pair in
        // either order, is drawn again. Every rollable category can make two
        // different pairs, so this ends; the cap only guards a near-miss streak
        // in a category that can make few.
        PromptPairs::Pair pair;
        bool found = false;
        for (int attempt = 0; attempt < 64 && ! found; ++attempt)
        {
            pair = PromptPairs::drawPair(b, b.categories[(size_t) pickCategory()], rng);
            if (rng.nextBool()) std::swap(pair.a, pair.b);
            const bool repeat = (pair.a == lastA_ && pair.b == lastB_)
                             || (pair.a == lastB_ && pair.b == lastA_);
            found = pair.a != pair.b && ! repeat;
        }
        if (! found || onRoll == nullptr || ! onRoll(pair))
            return;

        lastA_ = pair.a;
        lastB_ = pair.b;
        int nextFace = 1 + rng.nextInt(5);   // 1..5, then shifted past the current face
        if (nextFace >= face_) ++nextFace;
        face_ = nextFace;
        repaint();
    }

    void refreshTooltip()
    {
        juce::String tip = "Roll an A/B pair into both impulses, drawn from ";
        if (category_ >= 0)
            tip << "the category " << PromptPairs::bank().categories[(size_t) category_].label;
        else
            tip << "a category picked at random";
        tip << ". Right-click to choose the category.";
        if (unavailableReason_.isNotEmpty())
            tip << " Unavailable now: " << unavailableReason_ << ".";
        setTooltip(tip);
    }

    void showCategoryMenu()
    {
        const auto& b = PromptPairs::bank();
        juce::PopupMenu m;
        // Item id = category index + 2; id 1 = any category.
        m.addItem(1, "Any category", true, category_ < 0);
        for (int i = 0; i < (int) b.categories.size(); ++i)
            if (b.categories[(size_t) i].rollable)
                m.addItem(i + 2, b.categories[(size_t) i].label, true, category_ == i);

        juce::Component::SafePointer<PromptPairDice> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                        [safe](int id)
                        {
                            if (safe == nullptr || id <= 0) return;
                            const int category = id - 2;
                            if (category >= (int) PromptPairs::bank().categories.size()) return;
                            safe->category_ = category;   // -1 for "Any category"
                            safe->refreshTooltip();
                            safe->repaint();
                        });
    }

    int category_ = -1;            // index into bank().categories; session-only, like the panel's other view state
    juce::String unavailableReason_;
    juce::String lastA_, lastB_;
    int face_ = 5;
};
