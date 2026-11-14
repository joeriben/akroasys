#include "PhraseView.h"
#include "GuiHelpers.h"
#include "../PluginProcessor.h"

namespace
{
juce::String fromUtf8(const std::string& s)
{
    return juce::String::fromUTF8(s.data(), static_cast<int>(s.size()));
}
} // namespace

PhraseView::PhraseView(T5ynthProcessor& p) : processor(p)
{
    // Read-only: clicks go through to the panel.
    setInterceptsMouseClicks(false, false);
}

// ─── Poll ───────────────────────────────────────────────────────────────────

void PhraseView::poll()
{
    const PhraseModel::Config cfg = processor.phraseConfigFromParams();
    pollGrid(cfg.stepsPerBar);
    pollStrip(cfg);
}

void PhraseView::pollGrid(int stepsPerBar)
{
    const auto& seq = processor.getPhraseSequencer();

    const int spq = juce::jmax(1, stepsPerBar / 4);
    bool whole = spq != stepsPerQuarter;
    stepsPerQuarter = spq;

    // The generation before the words: a view read while the sequencer
    // publishes the next one is read under the generation before that
    // publish, so the next poll reads it again.
    const std::uint32_t gen = seq.viewGeneration.load(std::memory_order_acquire);
    int lo = PhraseModel::kMaxBarSteps, hi = -1;
    if (! viewRead || gen != viewGen)
    {
        viewRead = true;
        viewGen = gen;
        std::array<PhraseModel::StepView, PhraseModel::kMaxBarSteps> v {};
        const int n = seq.readView(v);
        if (n != numSteps)
        {
            numSteps = n;
            whole = true;
        }
        for (int k = 0; k < PhraseModel::kMaxBarSteps; ++k)
        {
            StepText t;
            if (k < n)
            {
                const auto& s = v[static_cast<size_t>(k)];
                t.rest = s.note < 0;
                t.glyph = fromUtf8(PhraseModel::stepGlyphText(s));
                // A rest writes its · in both rows.
                t.note = t.rest ? t.glyph : fromUtf8(PhraseModel::noteNameText(s.note));
                t.head = s.head && ! t.rest;
                t.accent = s.accent && ! t.rest;
                if (s.slide && ! t.rest)
                {
                    // The held note glides into the next onset; rests in
                    // between are its sustain. None left in the bar: it
                    // glides into the next bar.
                    t.tieTo = n;
                    for (int j = k + 1; j < n; ++j)
                        if (v[static_cast<size_t>(j)].note >= 0) { t.tieTo = j; break; }
                }
            }
            StepText& old = steps[static_cast<size_t>(k)];
            if (! (t == old))
            {
                lo = juce::jmin(lo, k);
                hi = juce::jmax(hi, k, juce::jmax(old.tieTo, t.tieTo));
                old = t;
            }
        }
    }

    const int cur = seq.currentStepForGui.load(std::memory_order_relaxed);
    const int shown = cur < numSteps ? cur : -1;
    if (shown != currentStep)
    {
        repaintSteps(currentStep, currentStep);
        repaintSteps(shown, shown);
        currentStep = shown;
    }

    if (whole)        repaint(gridRow);
    else if (hi >= lo) repaintSteps(lo, hi);
}

void PhraseView::pollStrip(const PhraseModel::Config& cfg)
{
    const auto& seq = processor.getPhraseSequencer();
    const int bar = seq.currentBarForGui.load(std::memory_order_relaxed);
    const bool keys = cfg.harm == PhraseModel::Harm::Keys;
    const PhraseModel::Chord held = keys ? seq.readChord() : PhraseModel::Chord {};
    const int N = PhraseModel::phraseBarsOf(cfg);
    const int first = bar - PhraseModel::floorMod(bar, N);

    if (! stripRead || cfg != stripConfig || first != stripFirstBar
        || (keys && (bar != stripKeysBar || ! (held == stripKeysChord))))
    {
        std::array<PhraseModel::StripCell, PhraseModel::kMaxPhraseBars> cells {};
        const int n = PhraseModel::phraseStrip(cfg, bar, keys ? &held : nullptr, cells);
        const bool hasForm = cfg.form != PhraseModel::Form::Loop;
        const bool whole = n != numBars || hasForm != stripHasForm;
        stripRead = true;
        stripConfig = cfg;
        stripFirstBar = first;
        stripKeysBar = bar;
        stripKeysChord = held;
        numBars = n;
        stripHasForm = hasForm;

        int lo = PhraseModel::kMaxPhraseBars, hi = -1;
        for (int j = 0; j < PhraseModel::kMaxPhraseBars; ++j)
        {
            StripText t;
            if (j < n)
            {
                const auto& c = cells[static_cast<size_t>(j)];
                if (c.form != 0) t.form = juce::String::charToString(static_cast<juce::juce_wchar>(c.form));
                t.chord = fromUtf8(c.chord);
            }
            StripText& old = strip[static_cast<size_t>(j)];
            if (! (t == old))
            {
                lo = juce::jmin(lo, j);
                hi = juce::jmax(hi, j);
                old = t;
            }
        }
        if (whole)         repaint(stripRow);
        else if (hi >= lo) repaint(stripCell(lo).getUnion(stripCell(hi)));
    }

    const int lit = numBars > 0 ? PhraseModel::floorMod(bar, numBars) : -1;
    if (lit != litBar)
    {
        if (litBar >= 0 && litBar < numBars) repaint(stripCell(litBar));
        if (lit >= 0) repaint(stripCell(lit));
        litBar = lit;
    }
}

// ─── Geometry ───────────────────────────────────────────────────────────────

juce::Rectangle<int> PhraseView::stripCell(int j) const
{
    const int n = juce::jmax(1, numBars);
    const int x0 = stripRow.getX() + stripRow.getWidth() * j / n;
    const int x1 = stripRow.getX() + stripRow.getWidth() * (j + 1) / n;
    return { x0, stripRow.getY(), x1 - x0, stripRow.getHeight() };
}

juce::Rectangle<int> PhraseView::stepColumn(int k) const
{
    const int n = juce::jmax(1, numSteps);
    const int x0 = gridRow.getX() + gridRow.getWidth() * k / n;
    const int x1 = gridRow.getX() + gridRow.getWidth() * (k + 1) / n;
    return { x0, gridRow.getY(), x1 - x0, gridRow.getHeight() };
}

juce::Rectangle<float> PhraseView::tieBounds(int k) const
{
    const int to = steps[static_cast<size_t>(k)].tieTo;
    const auto from = stepColumn(k).toFloat();
    const float x0 = from.getCentreX() + from.getWidth() * 0.25f;
    const float x1 = to < numSteps
        ? stepColumn(to).toFloat().getCentreX() - stepColumn(to).toFloat().getWidth() * 0.25f
        : stepColumn(numSteps - 1).toFloat().getRight() - 2.0f;   // on into the next bar
    const float y = static_cast<float>(noteRow.getBottom());
    return { x0, y - 2.0f, juce::jmax(4.0f, x1 - x0), 6.0f };
}

void PhraseView::repaintSteps(int lo, int hi)
{
    lo = juce::jmax(lo, 0);
    hi = juce::jmin(hi, numSteps - 1);
    if (hi < lo) return;
    repaint(stepColumn(lo).getUnion(stepColumn(hi)));
}

void PhraseView::resized()
{
    auto b = getLocalBounds();
    const int gap = b.getHeight() >= 45 ? 2 : 0;
    stripRow = b.removeFromTop((b.getHeight() - gap) / 3);
    b.removeFromTop(gap);
    gridRow = b;
    noteRow = b.removeFromTop(b.getHeight() / 2);
    glyphRow = b;

    auto sizeFor = [](int rowH) { return juce::jmin(14.0f, static_cast<float>(rowH) * 0.72f); };
    stripFont = uiFont(TextRole::Value, sizeFor(stripRow.getHeight()));
    stripBold = uiFont(TextRole::Value, sizeFor(stripRow.getHeight()), true);
    const int stepRowH = juce::jmin(noteRow.getHeight(), glyphRow.getHeight());
    stepFont = uiFont(TextRole::Value, sizeFor(stepRowH));
    stepBold = uiFont(TextRole::Value, sizeFor(stepRowH), true);

    // The form letters' column: the wider of A and B, measured here, not in paint().
    juce::GlyphArrangement ga;
    ga.addLineOfText(stripBold, "AB", 0.0f, 0.0f);
    formW = juce::roundToInt(std::ceil(ga.getBoundingBox(0, -1, true).getWidth() / 2.0f)) + 5;
}

// ─── Paint ──────────────────────────────────────────────────────────────────

void PhraseView::paint(juce::Graphics& g)
{
    // Strip: form letter and chord per bar, the bar the grid shows lit.
    for (int j = 0; j < numBars; ++j)
    {
        const auto cell = stripCell(j);
        if (! g.clipRegionIntersects(cell)) continue;
        const auto c = cell.reduced(1);
        g.setColour(j == litBar ? kSeqCol.withAlpha(0.35f) : kSurface);
        g.fillRect(c);

        const auto& s = strip[static_cast<size_t>(j)];
        auto t = c.reduced(3, 0);
        if (stripHasForm)
        {
            const auto f = t.removeFromLeft(juce::jmin(formW, t.getWidth()));
            if (s.form.isNotEmpty())
            {
                g.setFont(stripBold);
                g.setColour(kTextSecondary);
                g.drawText(s.form, f, juce::Justification::centredLeft, false);
            }
        }
        if (s.chord.isNotEmpty())
        {
            g.setFont(stripFont);
            g.setColour(kTextPrimary);
            g.drawFittedText(s.chord, t, juce::Justification::centredLeft, 1, 0.7f);
        }
    }

    // Grid: one column per step, the note over its glyph.
    for (int k = 0; k < numSteps; ++k)
    {
        const auto col = stepColumn(k);
        if (! g.clipRegionIntersects(col)) continue;
        const auto& s = steps[static_cast<size_t>(k)];
        const auto c = col.reduced(1);
        g.setColour(k == currentStep ? kSeqCol.withAlpha(0.35f) : s.rest ? kBg : kSurface);
        g.fillRect(c);
        if (k % stepsPerQuarter == 0)
        {
            g.setColour(kBorder);
            g.fillRect(c.getX(), c.getY(), 1, c.getHeight());
        }

        const juce::Rectangle<int> noteR(c.getX(), noteRow.getY(), c.getWidth(), noteRow.getHeight());
        const juce::Rectangle<int> glyphR(c.getX(), glyphRow.getY(), c.getWidth(), glyphRow.getHeight());
        g.setColour(s.rest ? kTextMuted : s.head ? kTextPrimary : kTextSecondary);
        g.setFont(s.head ? stepBold : stepFont);
        g.drawFittedText(s.note, noteR.reduced(2, 0), juce::Justification::centred, 1, 0.8f);
        g.drawFittedText(s.glyph, glyphR.reduced(2, 0), juce::Justification::centred, 1, 0.8f);

        if (s.accent)
        {
            // A wedge at the note's top right.
            const float ax = static_cast<float>(noteR.getRight()) - 3.0f;
            const float ay = static_cast<float>(noteR.getY()) + 2.0f;
            juce::Path w;
            w.startNewSubPath(ax - 5.0f, ay);
            w.lineTo(ax, ay + 2.5f);
            w.lineTo(ax - 5.0f, ay + 5.0f);
            g.setColour(kSeqCol);
            g.strokePath(w, juce::PathStrokeType(1.3f));
        }
    }

    // Ties after every column's fill: a tie crosses the columns it holds over.
    g.setColour(kSeqCol);
    for (int k = 0; k < numSteps; ++k)
    {
        if (steps[static_cast<size_t>(k)].tieTo < 0) continue;
        const auto r = tieBounds(k);
        if (! g.clipRegionIntersects(r.getSmallestIntegerContainer())) continue;
        const float y = r.getY() + 1.0f;
        juce::Path tie;
        tie.startNewSubPath(r.getX(), y);
        tie.quadraticTo(r.getCentreX(), y + 5.0f, r.getRight(), y);
        g.strokePath(tie, juce::PathStrokeType(1.3f));
    }
}
