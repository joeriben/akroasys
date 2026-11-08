#pragma once
#include <JuceHeader.h>
#include "GuiHelpers.h"

// ── The LCO authoring trace ──────────────────────────────────────────────────
// What replaced the "HEARD AS" text box in the LCO panel (BJ 2026-07-24): the
// authoring is shown as the sequence of things that actually happened, one
// station per step, instead of a reading with the raw Csound body printed under
// a rule.
//
//   HEARD      the prompt that was authored
//   OPENED     the library entries the AUTHOR asked to have opened, having read
//              the shelf and decided what this sound needs
//   WROTE      which model wrote it, and its own one-line reading
//   REPAIRED   the Csound errors it had to be repaired past (absent when it
//              compiled first try — an absent station is a fact, not a gap)
//   RUNNING    the compile state of the orchestra now in the engine
//
// Every station is a RECORD, never a reconstruction. The author is asked for no
// account of itself (its system prompt is untouched — BJ: "nur was ohnehin
// passiert ist"), so nothing here is the machine explaining itself; it is the
// write-path's own bookkeeping, which used to be discarded inside Python.
//
// Two consequences that are deliberate and must not be "tidied away":
//  - the chips are the AUTHOR's choice, and the caption says so. Python no
//    longer decides what the author may see: the system prompt carries the whole
//    shelf and the author names what it wants. `named` (the chips) and `opened`
//    (the count line) are kept apart because they differ exactly when the author
//    named no instrument — the backend then opens everything, and the station
//    must not read that back as "the author chose nothing".
//  - an empty motions list is shown as such. "No movement asked for" is exactly
//    the condition movement-by-default has to answer for, and hiding it would
//    hide the interesting case.
//
// Idle-cheap: paints only when something is set. No animation, and the only
// timer runs between a mouse-down and the flip it may become — never at idle.
//
// TWO SIDES (BJ 2026-07-24, step 2). The front is the trace above. The back is
// the Csound the author actually wrote, and ONLY that — not the host scaffold
// wrapped around it, which nobody wrote and nobody can change. Click and hold
// anywhere on the card turns it over, and again to come back.
class LcoTraceView : public juce::Component,
                     public juce::SettableTooltipClient,
                     private juce::Timer
{
public:
    /** What is known about the orchestra now in the engine. `Unknown` is a real
     *  and necessary state, not a placeholder for Ok: a recalled preset, or a
     *  compile window abandoned because the engine left Csound mode, genuinely
     *  did not observe a compile — and saying "compiled" there would be the
     *  panel asserting something nobody checked. */
    enum class CompileState { Unknown, Compiling, Ok, Error, Ended };

    /** One authored orchestra's trace, as it came off the wire. */
    struct Trace
    {
        juce::String prompt;      // what was authored; empty = not known (preset without one)
        juce::String model;       // the model that actually wrote it
        juce::String reading;     // the author's own READING line
        juce::String thinking;    // its reasoning before the code, verbatim
        // The consultation. `consultationKnown` is what separates "the author
        // asked for nothing" (a finding, drawn as such) from "no consultation
        // was recorded for this orchestra" (a recall, where the station is left
        // OUT). Without the flag the two are the same empty list, and every
        // preset load would report a consultation that never happened.
        bool consultationKnown = false;
        juce::StringArray namedInstruments, namedAdjectives, namedMotions;
        juce::StringArray openedInstruments, openedAdjectives, openedMotions;
        int libraryEntryCount = 0;
        // The synth's own controls. `knobsKnown` says an authoring was RECORDED
        // for this trace at all, exactly as consultationKnown does above — a
        // recalled preset carries no answer and gets no station. `knobsPending`
        // is empty while `knobsSet` is what STANDS on the patch, and otherwise
        // says in one line why it does not stand yet: the author writes its
        // settings whatever the KNOBS switch says, and the switch decides only
        // when they land. Silence here was the whole reason the feature looked
        // broken for a day.
        bool knobsKnown = false;
        juce::String knobsPending;       // "" = live; else why these are waiting
        juce::StringArray knobsSet;      // "Filter Cutoff  400" — landed, or waiting
        juce::StringArray knobsRefused;  // "master_vol - no such control" — and why
        juce::StringArray repairs;      // compiler errors repaired past, first-seen order
        int attempts = 0;               // 1 = compiled on the first pass
        bool valid = false;
    };

    LcoTraceView()
    {
        addAndMakeVisible(viewport_);
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, false);
        viewport_.setScrollBarThickness(7);
        viewport_.getVerticalScrollBar().setColour(juce::ScrollBar::thumbColourId,
                                                   kBorder.brighter(0.25f));
        viewport_.getVerticalScrollBar().setColour(juce::ScrollBar::trackColourId,
                                                   juce::Colours::transparentBlack);
        content_.owner_ = this;
        pulse_.owner_   = this;
        blink_.owner_   = this;
        content_.addAndMakeVisible(pulse_);
        pulse_.setVisible(false);
        // Takes its own mouse, exactly as the text box it replaced did: the
        // scrollbar needs drags, and the card flip lands here.
        setInterceptsMouseClicks(true, true);
    }

    /** Rule 1 of the project's JUCE safety list: stop the timer BEFORE any
     *  member is destroyed. This one is only ever running mid-press, so the
     *  window is small — which is exactly the kind that gets missed. */
    ~LcoTraceView() override { stopTimer(); blink_.stopTimer(); }

    /** The panel's responsive base font — same unit every other LCO widget uses. */
    void setBaseFont(float f)
    {
        if (juce::approximatelyEqual(f, base_))
            return;
        base_ = f;
        relayout();
    }

    /** Show a trace. Clears any status line: a trace IS the state.
     *
     *  It also resets the compile state to Unknown. A NEW orchestra has not been
     *  compiled yet by definition, and carrying the previous one's result over
     *  would caption it with a verdict on a different sound — including the
     *  permanent case where the old verdict was an error and nothing reopens a
     *  compile window to correct it. */
    void setTrace(Trace t)
    {
        trace_  = std::move(t);
        trace_.valid = true;
        status_.clear();
        busy_ = false;
        blink_.stopTimer();
        // What streamed came from whichever attempt was running; the trace
        // carries the one that compiled. Keeping both would show the reasoning
        // twice, once out of date.
        live_.clear();
        liveAttempt_ = 0;
        liveBody_.clear();
        liveBodyAttempt_ = 0;
        followWriting_ = false;
        lastFollowY_ = 0;
        compile_ = CompileState::Unknown;
        compileDetail_.clear();
        turnToFront();
        relayout();
    }

    /** What became of the synth's own controls in this authoring. Separate from
     *  setTrace because the answer is not known when the trace is: the trace is
     *  shown as soon as the reply lands, and whether the knobs actually moved is
     *  decided a few lines later by the apply — which refuses lines the backend
     *  passed. Called again whenever the KNOBS switch moves, because the same
     *  settings are on the patch or waiting depending on where it stands.
     *
     *  @param set      one line per control, read back off it while they are on
     *                  the patch and taken from the request while they wait.
     *  @param refused  everything that will never land, each with its reason.
     *  @param pending  empty while `set` is live; otherwise the one line that
     *                  says why it is not — the switch is off, or this is not
     *                  the sounding oscillator. */
    void setKnobs(juce::StringArray set, juce::StringArray refused,
                  juce::String pending = {})
    {
        trace_.knobsKnown   = true;
        trace_.knobsPending = std::move(pending);
        trace_.knobsSet     = std::move(set);
        trace_.knobsRefused = std::move(refused);
        relayout();
    }

    /** Dim single-line state, shown INSTEAD of the trace — "authoring...",
     *  "prompt is empty", an authoring failure. Replacing the trace is the
     *  point: the previous trace describes a sound that is being superseded or
     *  an attempt that failed, and leaving it up would let it read as current.
     *  (This mirrors what the text box it replaced did.) */
    /** @param busy  something is RUNNING and the user is waiting for it. The
     *               line gets a pulsing dot, so "nothing has happened yet" and
     *               "nothing is going to happen" stop looking alike — which for
     *               an authoring that takes a 12B generation is most of the
     *               time the panel is on screen. Terminal states (an empty
     *               prompt, a failure) pass false and stay still. */
    void setStatus(const juce::String& text, bool busy = false)
    {
        busy_ = busy;
        if (busy) blink_.startTimerHz(12);
        else      blink_.stopTimer();
        status_ = text;
        trace_ = {};
        live_.clear();
        liveAttempt_ = 0;
        liveBody_.clear();
        liveBodyAttempt_ = 0;
        followWriting_ = false;
        lastFollowY_ = 0;
        compile_ = CompileState::Unknown;
        compileDetail_.clear();
        turnToFront();
        relayout();
    }

    /** Update ONLY the line's words while an authoring runs — "Reading the
     *  library", "Writing the instrument", "Repairing (attempt 2 of 6): ...".
     *  setStatus is a STATE CHANGE and clears the live stream with the trace;
     *  calling it per phase would blank the reasoning and code already on
     *  screen. This touches nothing but the text. */
    void setStatusLine(const juce::String& text)
    {
        if (trace_.valid || text == status_)
            return;
        status_ = text;
        if (! showBack_)
            relayout();
    }

    /** The author's reasoning WHILE it is being written, above the status line
     *  and before any other station exists — it is the first thing the machine
     *  produces, so it is the first thing shown.
     *
     *  `attempt` counts the author's tries: a repair sends it back to the start
     *  and it reasons again, so the text does not continue, it REPLACES. Without
     *  that number the second attempt's first line would append to the first
     *  attempt's last one and read as one train of thought.
     *
     *  Ignored once a trace has arrived: what streamed is provisional, and only
     *  the attempt that actually compiled is allowed to caption the sound. */
    void setLiveThinking(int attempt, const juce::String& text)
    {
        if (trace_.valid || (text == live_ && attempt == liveAttempt_))
            return;
        liveAttempt_ = attempt;
        live_ = text;
        // Held, not drawn, while the code is up. Laying out the back again would
        // re-shape every line of Csound through the text shaper and repaint
        // content that provably did not change — once per streamed line, for
        // the length of a 12B generation. It is on screen the moment the card
        // turns back.
        if (! showBack_)
            relayout();
    }

    /** The CODE while it is being written, under the reasoning — the long half
     *  of the generation, and the part that used to be dark (BJ 2026-07-24:
     *  minutes of "Writing the instrument" with no sight of the token output).
     *  Same replace-not-append contract as setLiveThinking, same reason; a
     *  repair's body replaces the failed one's.
     *
     *  Ignored once a trace has arrived: only the attempt that compiled may
     *  caption the sound, and its code is already the back of the card. */
    void setLiveBody(int attempt, const juce::String& code)
    {
        if (trace_.valid || (code == liveBody_ && attempt == liveBodyAttempt_))
            return;
        const bool firstCode = liveBody_.isEmpty();
        liveBodyAttempt_ = attempt;
        liveBody_ = code;
        if (showBack_)
            return;   // held, not drawn — setLiveThinking's own rule
        // Follow the pen. Engagement is a STATE, not a bottom test: with a
        // reasoning taller than the viewport the view sits at the top when the
        // first code arrives, "already at the bottom" can never become true on
        // its own, and a plain bottom test would simply never engage. So the
        // first code line engages the follow; scrolling AWAY from where the
        // follow last put the view disengages it (the reader went back up);
        // returning to the bottom engages it again.
        const int posBefore    = viewport_.getViewPositionY();
        const int bottomBefore = juce::jmax(0, content_.getHeight() - viewport_.getViewHeight());
        if (firstCode)
            followWriting_ = true;
        else if (followWriting_ && posBefore < lastFollowY_ - 4)
            followWriting_ = false;
        else if (! followWriting_ && posBefore >= bottomBefore - 4)
            followWriting_ = true;
        relayout();
        if (followWriting_)
        {
            lastFollowY_ = juce::jmax(0, content_.getHeight() - viewport_.getViewHeight());
            viewport_.setViewPosition(0, lastFollowY_);
        }
    }

    /** The RUNNING station: the compile window's own report. Kept separate from
     *  setStatus so a compile result never wipes the trace of the orchestra it
     *  is reporting on. `detail` carries the compiler's error text for Error. */
    void setCompileState(CompileState s, const juce::String& detail = {})
    {
        // Early-out on an unchanged state. pollCsoundCompile writes "compiling"
        // on EVERY 10 Hz tick for the whole compile window, and relayout() is not
        // cheap here: it builds a juce::TextLayout for the prompt, the entire
        // (deliberately uncapped) thinking, the reading and every repair, then
        // repaints, which shapes all of it a second time. The juce::Label this
        // replaced early-outed on unchanged text, so without this the panel would
        // pay two full text-shaping passes ten times a second for nothing.
        if (s == compile_ && detail == compileDetail_)
            return;
        compile_       = s;
        compileDetail_ = detail;
        relayout();
    }

    /** A trace is on the card (setTrace), not a status line or a stream. */
    bool isShowingTrace() const noexcept { return trace_.valid; }

    /** What the RUNNING station currently shows. The panel's runtime-ended
     *  poll keys its precedence on THIS, not on a shadow copy of its own last
     *  write: setTrace()/setStatus() reset compile_ to Unknown without going
     *  through the panel, and a shadow copy that misses those resets pins the
     *  poll's "already shown" guard on a state nobody is displaying — a
     *  latched engine could then never be reported again. */
    CompileState compileState() const { return compile_; }

    /** Stop the pulse if it is still going. Called from the panel's existing
     *  10 Hz tick with the real busy state, because a status can be armed by a
     *  path that then returns without ever writing a terminating one — pressing
     *  Generate in LCO mode while a neural generation is still running says
     *  "Still generating" and returns, and nothing in the neural completion
     *  path writes to this view at all. A dot left breathing at 12 Hz over an
     *  idle panel is precisely the regression this project keeps having. */
    void stopBusy()
    {
        if (! busy_)
            return;
        busy_ = false;
        blink_.stopTimer();
        layoutPulse();
        relayout();
    }

    /** Placeholder for the empty panel, before anything has been authored. */
    void setPlaceholder(juce::String text) { placeholder_ = std::move(text); relayout(); }

    /** The back of the card: the Csound the author wrote, verbatim.
     *
     *  The BODY, not the orchestra. The scaffold around it — sr, ksmps, the
     *  sixteen channel reads, the score — is the host's, identical in every
     *  patch, and nobody authored it; printing it would bury the six lines that
     *  are actually this sound under sixty that are not. */
    /** @param csound   the back of the card. Only real Csound belongs here — it
     *                  is set as code, unwrapped, and step 3 makes it editable.
     *  @param summary  a plain-language account of what went into the sound,
     *                  which is what a preset written before params_text meant
     *                  the BODY carries ("saw: the canonical bright analogue
     *                  waveform; organ: ..."). Drawn on the FRONT, as prose:
     *                  it is something to read, not something to edit. */
    void setBody(const juce::String& csound, const juce::String& summary = {})
    {
        if (csound == body_ && summary == summary_)
            return;
        body_    = csound;
        summary_ = summary;
        relayout();
    }

    /** Does this text contain a line of Csound? One line is enough: an opcode
     *  call or an assignment whose target carries a type sigil. Prose does not
     *  survive the anchor and the sigil together. */
    static bool looksLikeCsound(const juce::String& text)
    {
        juce::StringArray lines;
        lines.addLines(text);
        for (auto ln : lines)
        {
            ln = ln.upToFirstOccurrenceOf(";", false, false).trim();
            if (ln.isEmpty())
                continue;
            const auto first = ln.upToFirstOccurrenceOf(" ", false, false);
            if (first.length() < 2 || juce::String("akifgS").indexOfChar(first[0]) < 0)
                continue;
            const auto rest = ln.fromFirstOccurrenceOf(" ", false, false).trim();
            if (rest.startsWithChar('=') || (rest.isNotEmpty()
                    && juce::CharacterFunctions::isLowerCase(rest[0])))
                return true;
        }
        return false;
    }

    /** Which side is up. Set by the click-and-hold, and readable so the panel
     *  around it can keep its own caption in step. */
    bool isShowingBody() const noexcept { return showBack_; }

    /** The scrolled content sits ON TOP of this view, so it — not this view — is
     *  the component a tooltip lookup finds under the mouse (JUCE asks the
     *  deepest hit component and does not walk up to its parents). Mirror the
     *  tip down, or a tooltip set here would simply never appear. */
    void setTooltip(const juce::String& t) override
    {
        juce::SettableTooltipClient::setTooltip(t);
        content_.setTooltip(t);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(kSurface.brighter(0.08f));
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(kBorder);
        g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);
    }

    void resized() override
    {
        viewport_.setBounds(getLocalBounds().reduced(1));
        relayout();
    }

    // ── Click and hold turns the card over ───────────────────────────────────
    // A press, not a click: a click belongs to the scrollbar and to whatever
    // selection the back grows next, and taking it here would make the card
    // flip every time the user tried to use it. The timer exists only between
    // the press and the flip, and a drag of more than a few pixels cancels it —
    // that is someone scrolling, not someone turning the page.
    // Plain left press only. A right press is what a context menu wants — and
    // step 3 puts a text field on the back, which will want one — and on macOS
    // ctrl+left arrives as a right press, so both would otherwise turn the card
    // over instead. That context menu now exists (BJ 2026-07-29: the whole view
    // was painted, never selectable, so there was no way to get a prompt, an
    // error or the Csound itself out of it): a right press offers "Copy", which
    // puts whichever side is up on the clipboard as plain text.
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showCopyMenu(e.getScreenPosition());
            return;
        }
        if (e.mods.isLeftButtonDown() && ! e.mods.isAnyModifierKeyDown())
            startTimer(kHoldMs);
    }
    void mouseDrag(const juce::MouseEvent& e) override      { if (e.getDistanceFromDragStart() > 6) stopTimer(); }
    void mouseUp(const juce::MouseEvent&) override          { stopTimer(); }

private:
    static constexpr int kHoldMs = 400;

    /** Bring the trace back up, without a relayout — the caller is about to.
     *
     *  Called wherever a state arrives that the back cannot show. This view is
     *  the ONLY visible status channel in LCO mode: dcoStatusLabel is never laid
     *  out and dcoFlagsLabel is given an empty rectangle there. So a status that
     *  landed while the code was up would not merely be late, it would never
     *  appear — an authoring that failed while the card was turned over would be
     *  invisible for good. */
    void turnToFront()
    {
        if (! showBack_)
            return;
        showBack_ = false;
        viewport_.setScrollBarsShown(true, false);
        viewport_.setViewPosition(0, 0);
    }

    void timerCallback() override
    {
        stopTimer();
        showBack_ = ! showBack_;
        // The two sides scroll independently in one viewport, so an arrival at
        // the bottom of a long trace would otherwise open the code halfway down.
        viewport_.setViewPosition(0, 0);
        // …and that reset is not the reader scrolling up. Without this, coming
        // back to the front mid-generation would look to setLiveBody like
        // someone leaving the pen behind, and the follow would stay off for the
        // rest of the authoring.
        lastFollowY_ = 0;
        // Code does not wrap — a wrapped Csound line is a different line. The
        // back scrolls sideways instead; the front never needs to.
        viewport_.setScrollBarsShown(true, showBack_);
        relayout();
        if (onFlip)
            onFlip(showBack_);
    }

    /** Right-click: "Copy" the side currently up, as plain text. One item, not
     *  a menu of stations — the view has no text selection (it is painted, not
     *  laid out in real components), so there is no "copy this part" to offer,
     *  only "copy what's on screen". Anchored at the cursor, not the view's
     *  bounds, matching every other context menu in this codebase. */
    void showCopyMenu(juce::Point<int> screenPos)
    {
        const juce::String text = showBack_ ? backText() : frontText();
        juce::PopupMenu m;
        m.addItem(1, showBack_ ? "Copy Csound" : "Copy trace", text.isNotEmpty());
        const juce::Rectangle<int> targetArea(screenPos.x, screenPos.y, 1, 1);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(targetArea),
            [text](int result)
            {
                if (result == 1)
                    juce::SystemClipboard::copyTextToClipboard(text);
            });
    }

    /** The back of the card, as plain text: the authored Csound, verbatim —
     *  preceded by the compiler's complaint when renderBody() is showing one
     *  above it. Without this, "Copy Csound" right after a failed compile
     *  would copy the code and silently drop the error the user is looking
     *  at, which is exactly the case this menu exists for. */
    juce::String backText() const
    {
        juce::String s;
        if (compile_ == CompileState::Error && compileDetail_.isNotEmpty())
            s << compileDetail_ << "\n\n";
        s << body_;
        return s;
    }

    /** The front of the card, as plain text — one paragraph per station, in
     *  the same order render() draws them, so the copy reads exactly like the
     *  screen it came from. Mirrors render()'s own station content rather than
     *  calling it, because render() draws (dots, hairlines, chip boxes); this
     *  only needs the words inside each station. */
    juce::String frontText() const
    {
        if (! trace_.valid)
        {
            juce::String s = status_.isNotEmpty() ? status_ : placeholder_;
            if (live_.isNotEmpty())
                s << "\n\nTHINKING\n" << live_;
            if (liveBody_.isNotEmpty())
                s << "\n\nWRITING\n" << liveBody_;
            return s;
        }

        juce::StringArray out;

        if (trace_.prompt.isNotEmpty())
            out.add("HEARD\n" + trace_.prompt);

        if (trace_.consultationKnown)
        {
            juce::StringArray named;
            named.addArray(trace_.namedInstruments);
            named.addArray(trace_.namedAdjectives);
            named.addArray(trace_.namedMotions);
            const int openedCount = trace_.openedInstruments.size()
                                  + trace_.openedAdjectives.size()
                                  + trace_.openedMotions.size();
            const bool wholeLibrary = trace_.libraryEntryCount > 0
                                   && openedCount >= trace_.libraryEntryCount;

            juce::String opened("OPENED\n");
            opened << (named.isEmpty() ? juce::String("the author named no entry")
                                       : named.joinIntoString(", "));
            juce::String note;
            if (wholeLibrary)
                note << "the whole library was opened";
            else if (trace_.libraryEntryCount > 0)
                note << openedCount << " of " << trace_.libraryEntryCount << " entries opened";
            if (trace_.namedMotions.isEmpty())
                note << (note.isEmpty() ? juce::String()
                                        : juce::String::fromUTF8(" \xc2\xb7 "))
                     << "no movement asked for";
            if (note.isNotEmpty())
                opened << "\n" << note;
            out.add(opened);
        }

        if (trace_.thinking.isNotEmpty())
            out.add("THOUGHT\n" + trace_.thinking);

        {
            juce::StringArray wrote;
            if (trace_.model.isNotEmpty())   wrote.add(trace_.model);
            if (trace_.reading.isNotEmpty()) wrote.add(trace_.reading);
            if (summary_.isNotEmpty())       wrote.add(summary_);
            out.add("WROTE\n" + wrote.joinIntoString("\n"));
        }

        if (trace_.knobsKnown)
        {
            juce::String rubric("KNOBS");
            if (! trace_.knobsSet.isEmpty())
                rubric << "  " << trace_.knobsSet.size();
            juce::StringArray lines;
            if (trace_.knobsPending.isNotEmpty())
                lines.add(trace_.knobsPending);
            if (! trace_.knobsSet.isEmpty())
                lines.addArray(trace_.knobsSet);
            else if (trace_.knobsPending.isEmpty())
                lines.add("offered, and the author took nothing");
            lines.addArray(trace_.knobsRefused);
            out.add(rubric + "\n" + lines.joinIntoString("\n"));
        }

        if (! trace_.repairs.isEmpty())
        {
            juce::String rubric("REPAIRED");
            if (trace_.attempts > 1)
                rubric << "  " << trace_.attempts << " attempts";
            out.add(rubric + "\n" + trace_.repairs.joinIntoString("\n"));
        }

        {
            juce::String text("compile not observed");
            switch (compile_)
            {
                case CompileState::Compiling: text = "compiling...";  break;
                case CompileState::Ok:        text = "compiled";      break;
                case CompileState::Error:
                    text = compileDetail_.isNotEmpty() ? compileDetail_
                                                       : juce::String("the orchestra did not compile");
                    break;
                case CompileState::Ended:
                    text = "the orchestra stopped its own performance - regenerate to restart the engine";
                    break;
                case CompileState::Unknown:   break;
            }
            out.add("RUNNING\n" + text);
        }

        return out.joinIntoString("\n\n");
    }

public:
    /** Called after a flip, with the side now up. */
    std::function<void(bool)> onFlip;

private:
    // ── Geometry, in one place so measuring and drawing cannot drift ──────────
    // The house roles, not ratios of my own: Caption is what every control label
    // in this build is set in, Hint what every piece of secondary text is. The
    // rubrics take Hint too — they are already set apart by small caps, letter
    // spacing and colour, and a fourth size here would be a size this build does
    // not otherwise have.
    float labelFont() const { return uiFontSize(TextRole::Hint,    base_); }
    float bodyFont()  const { return uiFontSize(TextRole::Caption, base_); }
    float hintFont()  const { return uiFontSize(TextRole::Hint,    base_); }
    int   dotR()      const { return 3; }
    int   railX()     const { return 4; }            // centre of the dot column
    int   textX()     const { return railX() + 12; }
    int   stationGap() const { return juce::roundToInt(bodyFont() * 0.85f); }

    // Small-caps station label with manual tracking — JUCE has no letter-spacing,
    // and the spacing is what makes these read as rubrics rather than as text.
    // The rubric never wraps and never drives the layout height (the station
    // reserves a fixed labelH), so there is no width-measuring counterpart.
    static void drawTrackedText(juce::Graphics& g, const juce::Font& f,
                                const juce::String& s, float x, float y, float h, float track)
    {
        for (int i = 0; i < s.length(); ++i)
        {
            const auto ch = s.substring(i, i + 1);
            const float w = juce::GlyphArrangement::getStringWidth(f, ch);
            g.drawText(ch, juce::Rectangle<float>(x, y, w + track, h),
                       juce::Justification::centredLeft, false);
            x += w + track;
        }
    }

    /** Wrapped paragraph: measures and (when g != nullptr) draws in ONE code
     *  path, so the height the layout reserves is always the height it needs. */
    static float paragraph(juce::Graphics* g, const juce::String& text, const juce::Font& f,
                           juce::Colour c, float x, float y, float w)
    {
        if (text.isEmpty() || w <= 1.0f)
            return 0.0f;
        juce::AttributedString as;
        as.append(text, f, c);
        as.setLineSpacing(1.0f);
        juce::TextLayout tl;
        tl.createLayout(as, w);
        if (g != nullptr)
            tl.draw(*g, { x, y, w, tl.getHeight() });
        return tl.getHeight();
    }

    /** A run of key chips, wrapped. `dimmed` draws the orientation entries the
     *  prompt did NOT reach — same shape, visibly weaker, never mixed in. */
    float chips(juce::Graphics* g, const juce::StringArray& keys,
                float x, float y, float w, bool dimmed) const
    {
        if (keys.isEmpty())
            return 0.0f;
        // Braces, not parens: `Font f(FontOptions(x))` is a function declaration.
        const juce::Font f { juce::FontOptions(hintFont()) };
        const float h    = std::round(hintFont() * 1.75f);
        const float padX = 5.0f, gap = 4.0f;
        float cx = x, cy = y;
        for (const auto& k : keys)
        {
            const float tw = juce::GlyphArrangement::getStringWidth(f, k);
            const float cw = tw + padX * 2.0f;
            if (cx > x && cx + cw > x + w)      // wrap
            {
                cx = x;
                cy += h + gap;
            }
            if (g != nullptr)
            {
                const juce::Rectangle<float> r(cx, cy, cw, h);
                g->setColour(dimmed ? kBorder.withAlpha(0.55f) : kBorder);
                g->drawRoundedRectangle(r.reduced(0.5f), 2.5f, 1.0f);
                g->setColour(dimmed ? kTextDisabled : kImpulseAText);
                g->setFont(f);
                g->drawText(k, r, juce::Justification::centred, false);
            }
            cx += cw + gap;
        }
        return (cy - y) + h;
    }

    /** The BACK: the authored body, monospaced and unwrapped.
     *
     *  Csound is line-structured — one opcode, one line — so wrapping would
     *  print lines the author never wrote. In a column this narrow that means
     *  the card scrolls sideways, which is why `width` is in/out: it comes in as
     *  the viewport's width and goes out as the longest line, so there is
     *  something to scroll to.
     *
     *  Trailing `; ...` is drawn apart from the code. The library's idioms carry
     *  their reasoning in exactly those comments ("mode 1 @ x1.00, Q 900"), and
     *  they are the author talking ABOUT the code rather than the code — the
     *  same distinction the front of the card is built on. */
    int renderBody(juce::Graphics* g, int& width) const
    {
        const float x = 8.0f;
        // Wraps to the VIEWPORT, never to the widened content: the draw pass is
        // handed the content width, and laying the error out to that would run
        // it off the side of the very page it was moved here to be read on, and
        // reserve a different height than the measure pass did.
        const float wrapW = static_cast<float>(juce::jmax(40, wrapW_)) - x - 11.0f;
        const juce::Font fLabel { juce::FontOptions(labelFont()) };
        const juce::Font fHint  { juce::FontOptions(hintFont())  };
        const float labelH = std::round(labelFont() * 1.5f);
        float top = 7.0f;

        // Which side is up, and how to leave it. Nothing else here says so, and
        // the way back is a gesture rather than a control.
        if (g != nullptr)
        {
            g->setColour(kTextDisabled);
            drawTrackedText(*g, fLabel, juce::String::fromUTF8("CSOUND \xc2\xb7 HOLD TO TURN BACK"),
                            x, top, labelH, juce::jmax(0.8f, labelFont() * 0.16f));
        }
        top += labelH + 3.0f;

        // The compiler's complaint belongs on the page being edited. It quotes
        // the offending line of the BODY, which is what is on screen here — on
        // the front it would be a verdict about code the reader cannot see.
        if (compile_ == CompileState::Error && compileDetail_.isNotEmpty())
            top += paragraph(g, compileDetail_, fHint, kErrorText, x, top, wrapW) + 5.0f;

        if (body_.trim().isEmpty())
        {
            const juce::Font fb { juce::FontOptions(bodyFont()) };
            return juce::roundToInt(top + paragraph(g, "nothing has been written yet",
                                                    fb, kDim, x, top, wrapW) + 7.0f);
        }

        const juce::Font f { juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                               juce::jmax(9.5f, base_ * 0.86f),
                                               juce::Font::plain) };
        const float lineH = std::round(f.getHeight() * 1.30f);
        float y = top, widest = 0.0f;
        juce::StringArray lines;
        lines.addLines(body_);
        if (g != nullptr)
            g->setFont(f);
        for (const auto& ln : lines)
        {
            widest = juce::jmax(widest, juce::GlyphArrangement::getStringWidth(f, ln));
            if (g != nullptr && ln.isNotEmpty())
            {
                const int c = commentStart(ln);
                const auto code = c < 0 ? ln : ln.substring(0, c);
                if (code.isNotEmpty())
                {
                    g->setColour(kTextSecondary);
                    g->drawText(code, juce::Rectangle<float>(x, y, widest + 400.0f, lineH),
                                juce::Justification::centredLeft, false);
                }
                if (c >= 0)
                {
                    g->setColour(kTextDisabled);
                    g->drawText(ln.substring(c),
                                juce::Rectangle<float>(x + juce::GlyphArrangement::getStringWidth(f, code),
                                                       y, widest + 400.0f, lineH),
                                juce::Justification::centredLeft, false);
                }
            }
            y += lineH;
        }
        width = juce::jmax(width, juce::roundToInt(x + widest + 11.0f));
        return juce::roundToInt(y + 7.0f);
    }

    /** The code while it is being written, in the back page's own dress —
     *  monospace, the trailing `;` comment set apart — but only its TAIL.
     *
     *  A tail on purpose: this is laid out once per streamed line for the
     *  length of a 12B generation, and shaping the whole growing body each
     *  time is the idle-CPU regression class this project keeps having. The
     *  full text is on the back of the card the moment the trace lands.
     *
     *  Ellipsized, never wrapped: a wrapped Csound line is a different line
     *  (renderBody's own rule), and the front has no horizontal scroll. */
    float liveCode(juce::Graphics* g, float y0, float ww) const
    {
        constexpr int kTail = 14;
        juce::StringArray lines;
        lines.addLines(liveBody_);
        const int from = juce::jmax(0, lines.size() - kTail);
        const float x = static_cast<float>(textX());
        // Braces, not parens: `Font f(FontOptions(x))` is a function declaration.
        const juce::Font f { juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                               juce::jmax(9.5f, base_ * 0.86f),
                                               juce::Font::plain) };
        const float lineH = std::round(f.getHeight() * 1.30f);
        float y = y0;
        if (from > 0)
        {
            // The hidden head, named — a tail that does not say it is one
            // reads as the whole body.
            const juce::Font fHead { juce::FontOptions(hintFont()) };
            const float headH = std::round(hintFont() * 1.4f);
            if (g != nullptr)
            {
                g->setColour(kDimmer);
                g->setFont(fHead);
                g->drawText("... " + juce::String(from) + " lines",
                            juce::Rectangle<float>(x, y, ww, headH),
                            juce::Justification::centredLeft, false);
            }
            y += headH + 2.0f;
        }
        for (int i = from; i < lines.size(); ++i)
        {
            const auto& ln = lines.getReference(i);
            if (g != nullptr && ln.isNotEmpty())
            {
                g->setFont(f);
                const int c = commentStart(ln);
                const auto code = c < 0 ? ln : ln.substring(0, c);
                if (code.isNotEmpty())
                {
                    g->setColour(kTextSecondary);
                    g->drawText(code, juce::Rectangle<float>(x, y, ww, lineH),
                                juce::Justification::centredLeft, true);
                }
                if (c >= 0)
                {
                    const float cw = juce::GlyphArrangement::getStringWidth(f, code);
                    // Only where the code left room — overprinting the very
                    // text it annotates helps nobody.
                    if (cw < ww - 12.0f)
                    {
                        g->setColour(kTextDisabled);
                        g->drawText(ln.substring(c),
                                    juce::Rectangle<float>(x + cw, y, ww - cw, lineH),
                                    juce::Justification::centredLeft, true);
                    }
                }
            }
            y += lineH;
        }
        return y - y0;
    }

    /** Index of the `;` that starts a trailing comment, or -1. A semicolon
     *  inside a string literal (`sprintf "gate%d; ..."`) is not one, so the
     *  quotes before it have to balance. */
    static int commentStart(const juce::String& line)
    {
        bool inString = false;
        for (int i = 0; i < line.length(); ++i)
        {
            const auto ch = line[i];
            if (ch == '"')            inString = ! inString;
            else if (ch == ';' && ! inString) return i;
        }
        return -1;
    }

    /** Walks every station. g == nullptr measures, g != nullptr draws — one
     *  traversal for both so the scroll height can never disagree with the
     *  content. Returns the total height. */
    int render(juce::Graphics* g, int width) const
    {
        // Right inset clears the viewport's own scrollbar (7px) plus a margin:
        // the content width is asked for BEFORE the scrollbar's visibility is
        // settled, so a tighter margin lets a long line slide under the thumb.
        const float w = static_cast<float>(width) - static_cast<float>(textX()) - 11.0f;
        // Braces, not parens: `Font f(FontOptions(x))` is a function declaration.
        const juce::Font fLabel { juce::FontOptions(labelFont()) };
        const juce::Font fBody  { juce::FontOptions(bodyFont())  };
        const juce::Font fHint  { juce::FontOptions(hintFont())  };
        const float labelH = std::round(labelFont() * 1.5f);
        float y = 7.0f;

        // One station: dot, rubric, body. `hasNext` draws the hairline down to
        // where the next dot will sit; the last station ends the rail instead of
        // trailing a line into empty space.
        auto station = [&](juce::Colour dot, const juce::String& rubric,
                           juce::Colour rubricCol, bool hasNext,
                           const std::function<float(float)>& body)
        {
            const float dotY = y + labelH * 0.5f;
            if (g != nullptr)
            {
                g->setColour(rubricCol);
                drawTrackedText(*g, fLabel, rubric, static_cast<float>(textX()), y, labelH,
                                juce::jmax(0.8f, labelFont() * 0.16f));
            }
            y += labelH + 1.0f;
            y += body(w);
            if (g != nullptr)
            {
                // Hairline first, dot over it — the dot must not be cut into.
                if (hasNext)
                {
                    g->setColour(kBorder);
                    g->fillRect(juce::Rectangle<float>(static_cast<float>(railX()) - 0.5f,
                                                       dotY, 1.0f,
                                                       juce::jmax(0.0f, y + stationGap() - dotY)));
                }
                g->setColour(dot);
                g->fillEllipse(static_cast<float>(railX() - dotR()), dotY - static_cast<float>(dotR()),
                               static_cast<float>(dotR() * 2), static_cast<float>(dotR() * 2));
            }
            y += static_cast<float>(stationGap());
        };

        // No trace yet: the status line, and under it the authoring as it
        // arrives — the reasoning first, then the code while it is written. The
        // panel is not empty for the length of a 12B generation, and what fills
        // it is the machine's own output rather than a spinner.
        if (! trace_.valid)
        {
            const auto text = status_.isNotEmpty() ? status_ : placeholder_;
            // The dot that breathes while something is running is drawn by
            // pulse_, a child of its own, so its 12 Hz repaint cannot drag this
            // whole view through the text shaper with it.
            y += paragraph(g, text, fBody, busy_ ? kTextPrimary : kDim,
                           static_cast<float>(textX()), y, w);
            const bool haveCode = liveBody_.isNotEmpty();
            if (live_.isNotEmpty() || haveCode)
            {
                y += static_cast<float>(stationGap());
                if (live_.isNotEmpty())
                    station(kWarning, "THINKING", kTextDisabled, haveCode, [&](float ww)
                    {
                        return paragraph(g, live_, fHint, kDim,
                                         static_cast<float>(textX()), y, ww);
                    });
                // WRITING while it happens is provisional by nature — the trace
                // that lands replaces it with the attempt that compiled.
                if (haveCode)
                    station(kWarning, "WRITING", kTextDisabled, false, [&](float ww)
                    {
                        return liveCode(g, y, ww);
                    });
                return juce::roundToInt(y - static_cast<float>(stationGap()) + 7.0f);
            }
            return juce::roundToInt(y + 7.0f);
        }

        // ── HEARD ────────────────────────────────────────────────────────────
        // Omitted when the prompt is not known. A Csound-only preset stores no
        // prompt, and captioning the restored orchestra with whatever happens to
        // be in the editor would name a prompt that never authored it.
        if (trace_.prompt.isNotEmpty())
            station(kImpulseA, "HEARD", kTextDisabled, true, [&](float ww)
            {
                return paragraph(g, trace_.prompt, fBody, kImpulseAText,
                                 static_cast<float>(textX()), y, ww);
            });

        // ── OPENED ───────────────────────────────────────────────────────────
        // What the author asked for after reading the shelf. Every chip here is
        // an entry it named itself.
        //
        // Drawn ONLY when a consultation was actually recorded. A recalled
        // orchestra has none (no preset stores one), and an empty list there
        // would print "the author asked for nothing" over an orchestra whose
        // consultation simply was never kept — the precise false claim this
        // whole surface exists to avoid.
        if (trace_.consultationKnown)
        station(kImpulseA, "OPENED", kTextDisabled, true, [&](float ww)
        {
            const float y0 = y;
            juce::StringArray named;
            named.addArray(trace_.namedInstruments);
            named.addArray(trace_.namedAdjectives);
            named.addArray(trace_.namedMotions);
            const int openedCount = trace_.openedInstruments.size()
                                  + trace_.openedAdjectives.size()
                                  + trace_.openedMotions.size();
            // The chips are what the author ASKED FOR; the count line says what
            // was actually put in front of it. The two part company when the
            // reply named words or movements but no instrument — the backend
            // then opens everything, and reading "the author named nothing" off
            // a full library would print that over a reply that named three
            // things. `named` is carried on the wire precisely so this station
            // never has to guess one from the other.
            const bool wholeLibrary = trace_.libraryEntryCount > 0
                                   && openedCount >= trace_.libraryEntryCount;

            float yy = y;
            if (named.isEmpty())
                yy += paragraph(g, "the author named no entry",
                                fHint, kTextDisabled, static_cast<float>(textX()), yy, ww) + 2.0f;
            else
                yy += chips(g, named, static_cast<float>(textX()), yy, ww, false) + 4.0f;

            juce::String note;
            if (wholeLibrary)
                note << "the whole library was opened";
            else if (trace_.libraryEntryCount > 0)
                note << openedCount << " of " << trace_.libraryEntryCount << " entries opened";
            if (trace_.namedMotions.isEmpty())
                note << (note.isEmpty() ? juce::String()
                                        : juce::String::fromUTF8(" \xc2\xb7 "))
                     << "no movement asked for";
            if (note.isNotEmpty())
                yy += paragraph(g, note, fHint, kDimmer, static_cast<float>(textX()), yy, ww) + 2.0f;
            return yy - y0;
        });

        // ── THOUGHT ──────────────────────────────────────────────────────────
        // The author's reasoning in its own words, verbatim and in full: what it
        // decided the sound IS, what excites and resonates in it, what moves.
        // Quoted, never summarised — a summary would be this panel's account of
        // the machine, and the whole point is the machine's own.
        //
        // Absent when the reply carried no prose outside the fence, and on every
        // recall (no preset stores it). Never fabricated from the code.
        if (trace_.thinking.isNotEmpty())
            station(kImpulseA, "THOUGHT", kTextDisabled, true, [&](float ww)
            {
                return paragraph(g, trace_.thinking, fHint, kDim,
                                 static_cast<float>(textX()), y, ww);
            });

        // ── WROTE ────────────────────────────────────────────────────────────
        station(kImpulseA, "WROTE", kTextDisabled, true, [&](float ww)
        {
            const float y0 = y;
            float yy = y;
            if (trace_.model.isNotEmpty())
                yy += paragraph(g, trace_.model, fHint, kDim,
                                static_cast<float>(textX()), yy, ww) + 2.0f;
            if (trace_.reading.isNotEmpty())
                yy += paragraph(g, trace_.reading, fBody, kImpulseAText,
                                static_cast<float>(textX()), yy, ww);
            // What a pre-body preset stored instead of the code: an account of
            // what went into the sound. Prose, in the panel's own text — it is
            // something to read, not something to edit, which is why it is here
            // and not on the back.
            if (summary_.isNotEmpty())
                yy += paragraph(g, summary_, fHint, kDim,
                                static_cast<float>(textX()), yy + 3.0f, ww) + 3.0f;
            return yy - y0;
        });

        // ── KNOBS ────────────────────────────────────────────────────────────
        // What the author asked of the synth's own controls, and whether it is
        // on the patch. All three outcomes are drawn, because two of them are
        // silent from outside and look identical: an authoring that took nothing
        // and one whose settings are waiting for the switch both leave the patch
        // untouched. Left out only for a trace that has no answer (a recalled
        // preset).
        if (trace_.knobsKnown)
        {
            const bool took = ! trace_.knobsSet.isEmpty();
            const bool live = took && trace_.knobsPending.isEmpty();
            juce::String rubric("KNOBS");
            if (took) rubric << "  " << trace_.knobsSet.size();
            // hasNext is TRUE unconditionally: RUNNING always follows, and this
            // argument draws the rail hairline down to it, not the station's own
            // emphasis. (It was `took`, which broke the rail for exactly the two
            // silent outcomes this station exists to show.)
            station(live ? kImpulseB : kTextDisabled, rubric, kTextDisabled, true, [&](float ww)
            {
                const float y0 = y;
                float yy = y;
                // The reason first, whenever there is one — a caller that hands
                // one in gets it drawn, with or without lines under it. Waiting
                // lines are dim: they are real settings this instrument asks
                // for, and the only thing missing is the player's permission.
                if (trace_.knobsPending.isNotEmpty())
                    yy += paragraph(g, trace_.knobsPending, fHint, kDim,
                                    static_cast<float>(textX()), yy, ww) + 2.0f;
                if (took)
                    for (const auto& s : trace_.knobsSet)
                        yy += paragraph(g, s, fHint, live ? kImpulseB : kDim,
                                        static_cast<float>(textX()), yy, ww) + 2.0f;
                else if (trace_.knobsPending.isEmpty())
                    yy += paragraph(g, "offered, and the author took nothing",
                                    fHint, kDim, static_cast<float>(textX()), yy, ww);
                // A refused line is the author asking for something the synth
                // does not have, or a value it could not read. Shown in the
                // warning ink next to what did land, so a half-applied authoring
                // cannot read as a whole one.
                for (const auto& s : trace_.knobsRefused)
                    yy += paragraph(g, s, fHint, kWarning,
                                    static_cast<float>(textX()), yy, ww) + 2.0f;
                return yy - y0;
            });
        }

        // ── REPAIRED ─────────────────────────────────────────────────────────
        // Drawn ONLY when the author actually had to be sent back. A first-try
        // orchestra shows no station here, and that absence is the report.
        if (! trace_.repairs.isEmpty())
        {
            juce::String rubric("REPAIRED");
            if (trace_.attempts > 1)
                rubric << "  " << trace_.attempts << " attempts";
            station(kWarning, rubric, kWarning, true, [&](float ww)
            {
                const float y0 = y;
                float yy = y;
                for (const auto& e : trace_.repairs)
                    yy += paragraph(g, e, fHint, kDim, static_cast<float>(textX()), yy, ww) + 2.0f;
                return yy - y0;
            });
        }

        // ── RUNNING ──────────────────────────────────────────────────────────
        // Five states, and Unknown is not a synonym for Ok: an orchestra whose
        // compile nobody watched (a recalled preset; a window abandoned because
        // the engine left Csound mode mid-compile) must say so rather than
        // report a success it never saw.
        {
            juce::Colour dot = kTextDisabled, ink = kDimmer;
            juce::String text = "compile not observed";
            switch (compile_)
            {
                case CompileState::Compiling: dot = kWarning; ink = kWarning;   text = "compiling..."; break;
                case CompileState::Ok:        dot = kSuccess; ink = kSuccess;   text = "compiled";     break;
                case CompileState::Error:     dot = kError;   ink = kErrorText;
                    text = compileDetail_.isNotEmpty() ? compileDetail_
                                                       : juce::String("the orchestra did not compile");
                    break;
                case CompileState::Ended:     dot = kWarning; ink = kWarning;
                    text = "the orchestra stopped its own performance - regenerate to restart the engine";
                    break;
                case CompileState::Unknown:   break;
            }
            station(dot, "RUNNING", kTextDisabled, false, [&](float ww)
            {
                return paragraph(g, text, fHint, ink, static_cast<float>(textX()), y, ww);
            });
        }

        return juce::roundToInt(y - static_cast<float>(stationGap()) + 7.0f);
    }

    void relayout()
    {
        int w = juce::jmax(1, viewport_.getMaximumVisibleWidth());
        // The width text WRAPS to, kept apart from the content width, which the
        // back widens to its longest code line. Measuring and drawing must wrap
        // identically, and the draw pass only sees the widened one — so the
        // compile error on the back would be laid out to the scrolled width and
        // run off the side of the viewport it is meant to be read in.
        wrapW_ = w;
        const int h = showBack_ ? renderBody(nullptr, w)   // widens w to the longest line
                                : render(nullptr, w);
        // Subtract the bar thickness OURSELVES rather than asking the viewport:
        // getMaximumVisibleHeight() still reports the previous content's bar
        // state at this point, so on the flip itself it answers the full height,
        // content exactly that tall ends up 7px past the visible area, and the
        // viewport replies with a vertical scrollbar whose entire range is the
        // horizontal bar's thickness.
        const int floorH = viewport_.getHeight()
                         - (showBack_ ? viewport_.getScrollBarThickness() : 0);
        content_.setSize(w, juce::jmax(floorH, h));
        layoutPulse();
        content_.repaint();
    }

    /** The pulsing dot is its OWN component, and that is the whole point: a
     *  repaint of a rectangle inside content_ still runs Content::paint, which
     *  ignores the clip region and re-shapes every paragraph in the view —
     *  including the author's uncapped streamed reasoning, twelve times a
     *  second, for the length of a 12B generation. A separate child repaints
     *  alone. */
    void layoutPulse()
    {
        const bool wanted = busy_ && ! trace_.valid && ! showBack_;
        pulse_.setVisible(wanted);
        if (wanted)
            pulse_.setBounds(0, 0, textX(), juce::roundToInt(bodyFont() * 1.6f));
    }

    struct Pulse : public juce::Component
    {
        Pulse() { setInterceptsMouseClicks(false, false); }   // the flip lands behind it
        void paint(juce::Graphics& g) override
        {
            if (owner_ == nullptr)
                return;
            const float phase = std::sin(static_cast<float>(juce::Time::getMillisecondCounter() % 1200)
                                         / 1200.0f * juce::MathConstants<float>::twoPi);
            const float cy = 7.0f + owner_->bodyFont() * 0.62f;
            const int   r  = owner_->dotR();
            g.setColour(kWarning.withAlpha(0.45f + 0.55f * (0.5f + 0.5f * phase)));
            g.fillEllipse(static_cast<float>(owner_->railX() - r), cy - static_cast<float>(r),
                          static_cast<float>(r * 2), static_cast<float>(r * 2));
        }
        LcoTraceView* owner_ = nullptr;
    };

    struct Content : public juce::Component,
                     public juce::SettableTooltipClient
    {
        void paint(juce::Graphics& g) override
        {
            if (owner_ == nullptr)
                return;
            int w = getWidth();
            if (owner_->showBack_) owner_->renderBody(&g, w);
            else                   owner_->render(&g, w);
        }
        // The scrolled content sits on top, so the press lands HERE. JUCE does
        // not pass mouse events up to a parent, so without this the card could
        // only be flipped by hitting the 1px margin around the viewport.
        void mouseDown(const juce::MouseEvent& e) override { if (owner_) owner_->mouseDown(e); }
        void mouseDrag(const juce::MouseEvent& e) override { if (owner_) owner_->mouseDrag(e); }
        void mouseUp  (const juce::MouseEvent& e) override { if (owner_) owner_->mouseUp(e); }
        LcoTraceView* owner_ = nullptr;
    };

    // content_ AFTER viewport_ is deliberate and safe: Viewport holds its viewed
    // component through a WeakReference with deleteContent == false, so content_
    // being destroyed first simply nulls that reference and ~Viewport no-ops.
    juce::Viewport viewport_;
    Content content_;
    Trace   trace_;
    juce::String status_, placeholder_, compileDetail_;
    juce::String live_;             // the reasoning as it streams; empty once traced
    int          liveAttempt_ = 0;
    juce::String liveBody_;         // the code as it streams; empty once traced
    int          liveBodyAttempt_ = 0;
    bool         followWriting_ = false;  // the live-code follow is engaged
    int          lastFollowY_ = 0;        // where the follow last put the view
    juce::String body_;             // the back of the card: the authored Csound
    juce::String summary_;          // a legacy preset's plain-language account, front side
    bool         showBack_ = false;
    bool         busy_ = false;

    /** Repaints the status dot, and nothing else in the view: pulse_ is its own
     *  component, so this cannot reach Content::paint. Runs strictly between a
     *  busy status and the state that ends it — and PromptPanel's existing 10 Hz
     *  tick calls stopBusy() whenever nothing is actually running, so a path
     *  that arms it and returns without a terminating status cannot leave it
     *  spinning at idle. */
    struct Blink : public juce::Timer
    {
        ~Blink() override { stopTimer(); }
        void timerCallback() override { if (owner_ != nullptr) owner_->pulse_.repaint(); }
        LcoTraceView* owner_ = nullptr;
    };
    Pulse pulse_;
    Blink blink_;
    int   wrapW_ = 0;
    CompileState compile_ = CompileState::Unknown;
    float   base_ = 13.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LcoTraceView)
};
