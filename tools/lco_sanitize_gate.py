"""Does `sanitize()` still take the ORCHESTRA out of the author's reply?

Since the author was asked to reason in plain language before writing the code
(`HOW TO ANSWER`), a reply is prose AND code, and `sanitize` has to tell them
apart. Getting that wrong is silent and expensive in both directions:

  - prose picked as the body -> the compiler is handed English, the attempt is
    burned, and a reply that contained a complete orchestra is reported as "the
    author could not write a compiling orchestra after N attempts",
  - code picked as the thinking -> the panel quotes Csound as the machine's
    reasoning, and the sketch it did NOT choose is what the user hears.

Neither shows up by ear and neither shows up in a green authoring run, because
the repair loop hides the first and the panel is only read afterwards. Hence a
gate, run on the shapes real replies actually take — including the malformed
ones, which are the whole reason this code is hard: the body is the LAST thing
written, so a generation that stops early loses its closing fence.

No model, no compiler, no IPC: this tests the parser, in-process, in a second.

Run:  .venv/bin/python tools/lco_sanitize_gate.py
"""
import os, re, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "backend"))
import lco_write as L  # noqa: E402

# The reasoning has to be the reasoning the author actually writes, and this is
# the whole difficulty of the corpus: HOW TO ANSWER asks three questions, and the
# answers come back as FRAGMENTS, which have no full stop to mark them as prose
# and routinely begin with a word starting a/k/i/f/S — the Csound type sigils.
# Generic filler ("The prompt asks for line 3 of reasoning.") scores zero and
# would let every defect this file exists to catch pass unnoticed, so the corpus
# asserts its own teeth below: it must outscore the body it is paired with.
REASONING = """What IS this sound: a cello, bowed, not plucked
a body of wood, air inside it, a long string across a bridge
and the bow is what keeps it going, so the excitation is continuous
it is not a struck thing, no impulse, nothing decays on its own
first the string, which is a comb filter on whatever excites it
k rate is enough for the bow pressure, it does not need audio rate
in the low register the body resonance sits under 300 Hz
as the bow speed rises the higher partials come up with it
kind of a nasal edge when the pressure is high
So: continuous noisy excitation into a string model, then a body filter
after that a gentle low shelf so it does not get thin
always moving, because a held bow is never perfectly steady"""
BODY = "\n".join([
    "aexc  dust 1.0, 3900",
    "kstr  limit kfreq * koct1, 20, 12000",
    "asig  mode aexc, kstr, 240",
    "asig  butlp asig, 2400",
])

# A library idiom quoted verbatim runs 8-18 lines; the body the author adapts
# from it runs 3-6. Whichever segment merely COUNTS higher is therefore the
# quotation, systematically — so a scoring picker ships the library's own
# example as the sound and captions the real orchestra as reasoning.
LIB_IDIOM = """aexq0   rand 0.06, 0.5, 1
k0qf0   limit (kfreq * koct1) * 1.0000, 20, 15000
k0qg0   = ((kfreq * koct1) * 1.0000 < 15000 ? 0.850 : 0)
a0q0   mode aexq0, k0qf0, 900
k0qf1   limit (kfreq * koct1) * 2.7600, 20, 15000
k0qg1   = ((kfreq * koct1) * 2.7600 < 15000 ? 0.700 : 0)
a0q1   mode aexq0, k0qf1, 1025
k0qf2   limit (kfreq * koct1) * 5.4000, 20, 15000
k0qg2   = ((kfreq * koct1) * 5.4000 < 15000 ? 0.500 : 0)
a0q2   mode aexq0, k0qf2, 1150
asig    = (a0q0 * k0qg0 + a0q1 * k0qg1 + a0q2 * k0qg2) * 0.150"""

# The library writes its modal banks as preprocessor macros, under a head of
# `;` lines that declare what the body does. Neither a `#define` line nor a
# `$NAME(...)` call has the shape of an opcode line, and the first two lines
# that DO stand inside the definition — so a reply without a fence was cut
# below the `#define`, and every call failed as an undefined macro.
MACRO_REASONING = ("What IS this sound: a metal bar, standing, not struck\n"
                   "five modes, rung by continuous noise while the key is held")
MACRO_BODY = "\n".join([
    "; FOLLOWS kfreq: yes",
    "; five beam modes rung by continuous noise (Mathews & Smith 2003)",
    "#define BARM(R'Q'W) #",
    "kf      limit kfreq * koct1 * $R, 20, 15000",
    "am      mvmfilter aex, kf, $Q / (3.14159265 * kf)",
    "asum    += am * $W",
    "#",
    "aex     rand 0.09, 0.5, 1",
    "asum    = 0",
    "$BARM(1'260'0.85)",
    "$BARM(2.76'300'0.70)",
    "asig    = asum * kvol1",
])
# The other shape a macro takes: a whole definition on one line, an expression.
MACRO_ONE_LINE = "\n".join([
    "; a sine through a folding stage",
    "#define FOLDM(X) #(($X) - 2 * (abs(($X) - 0.6) - abs(($X) + 0.6)))#",
    "#define FOLDG(X) #(0.5 * ($X) * ($X))#",
    "kfold   = 3.5 ; fold[1..10]: how hard the sine is folded",
    "ain     poscil kfold, kfreq * koct1",
    "asig    = $FOLDM(ain) * $FOLDG(kfold) * kvol1",
])

# (name, reply, what the body must start with, predicate on the thinking)
CASES = [
    ("well-formed",
     "It is a bowed string: noisy continuous excitation, woody body.\n\n"
     "```csound\nkstr limit kfreq * koct1, 20, 12000\naexc dust 1.0, 3900\n"
     "asig mode aexc, kstr, 240\nREADING: bowed string\n```\n",
     "kstr limit", lambda t: t.startswith("It is a bowed string")),

    # The body is written last, so a stopped generation loses the closing fence.
    # With reasoning longer than the code — which HOW TO ANSWER invites — a
    # line-counting split inverts here and compiles the prose.
    ("unclosed final fence, reasoning longer than the body",
     REASONING + "\n\n```csound\n" + BODY,
     "aexc  dust", lambda t: t.startswith("What IS this sound")),

    # ... and the same reasoning under a PROPERLY CLOSED fence. Trivial to get
    # right — the fence says exactly where the body is — but a picker that
    # decides by content alone gets it wrong here too, so the damage is no
    # longer confined to malformed replies. This case is the canary.
    ("well-formed fence, reasoning longer than the body",
     REASONING + "\n\n```csound\n" + BODY + "\nREADING: bowed cello\n```\n",
     "aexc  dust", lambda t: t.startswith("What IS this sound")),

    # The author is oriented by the library, quotes the idiom it is working
    # from, and then writes something shorter of its own. The quotation always
    # comes first and is always the longer of the two.
    ("a library idiom quoted, then a shorter body of the author's own",
     "The struck_bar idiom is the closest starting point:\n\n```csound\n"
     + LIB_IDIOM + "\n```\n\nBut a cello is bowed, so the excitation is "
     "continuous and there is one string, not five modes:\n\n```csound\n"
     + BODY + "\nREADING: bowed cello\n```\n",
     "aexc  dust", lambda t: "k0qf0   limit" in t and "struck_bar idiom" in t),

    # HOW TO ANSWER says everything outside the fence is thinking, and the author
    # takes that literally in BOTH directions — it writes after the block as
    # readily as before it. A rule that simply takes the last code-carrying
    # segment ships the afterthought.
    ("a line of prose after the closing fence",
     "A bowed cello.\n\n```csound\n" + BODY + "\nREADING: bowed cello\n```\n\n"
     "asig carries the bowed tone into the host\n",
     "aexc  dust", lambda t: "asig carries the bowed tone" in t),

    # The same, at full length: the reasoning written AFTER the code. The
    # discarded orchestra compiles; the English does not, so this shape burns
    # the whole repair loop and is then reported as an author that could not
    # write a compiling orchestra.
    ("the whole reasoning written after the code",
     "Here is the orchestra for a bowed cello:\n\n```csound\n" + BODY
     + "\nREADING: bowed cello\n```\n\n"
     "What IS this sound: a cello, bowed, one string and a wooden body\n"
     "aexc is the bow, continuous noise, never an impulse\n"
     "kstr holds the played note, clamped below Nyquist\n"
     "asig is the string and the body together\n",
     "aexc  dust", lambda t: "What IS this sound" in t),

    # A plainer variant offered after the real one. Shorter, so a size rule gets
    # it right and a position rule gets it wrong — and it COMPILES, so the user
    # simply hears a bare sine where the cello should be, with no error anywhere.
    ("a simplified variant offered after the body",
     "A bowed cello.\n\n```csound\n" + BODY + "\nREADING: bowed cello\n```\n\n"
     "If you want it plainer, the core of it is just:\n\n"
     "```csound\nasig  oscili 0.4, kfreq * koct1\n```\n",
     "aexc  dust", lambda t: "If you want it plainer" in t),

    # An enumeration of the variables, in English. Reads as a multi-output
    # opcode line ("aL, aR reverbsc ...") to anything shape-based.
    ("an English enumeration of variable names after the fence",
     "A bowed cello.\n\n```csound\n" + BODY + "\nREADING: bowed cello\n```\n\n"
     "aexc, kstr and asig are the three stages\n",
     "aexc  dust", lambda t: "three stages" in t),

    # The library's idioms carry their reasoning in full-line comments, and the
    # author copies that habit. A body can therefore be mostly comment.
    ("a body that is mostly comment",
     "A bowed cello.\n\n```csound\n"
     "; bow: a continuous noise excitation, never an impulse\n"
     "aexc  rand 0.05\n"
     "; string: one mode at the played note\n"
     "asig  mode aexc, kfreq * koct1, 240\n"
     "; body: a gentle low-pass so it does not get glassy\n"
     "asig  butlp asig, 2400\n"
     "READING: bowed cello, continuous excitation\n```\n",
     "; bow: a continuous", lambda t: t.startswith("A bowed cello")),

    # A sketch quoted mid-thought, then the real body with no closer: taking the
    # last COMPLETE fence ships the sketch and the user hears the wrong sound.
    ("sketch fence, then the real body unclosed",
     "First thought.\n```csound\nasig vco2 0.4, kfreq\n```\nOn reflection, better:\n"
     "```csound\nasig vco2 0.5, kfreq*koct1, 2\nasig tone asig, 900\n",
     "asig vco2 0.5", lambda t: "On reflection" in t),

    # A lone closer above the real block: pairing binds it to the real block's
    # OPENER and captures the whitespace between them -> an empty body.
    ("stray closer above the real block",
     "Reasoning here.\n```\n\n```csound\nasig vco2 0.5, kfreq\nREADING: x\n```\n",
     "asig vco2", lambda t: t.startswith("Reasoning here")),

    ("no fence at all",
     "asig poscil 0.4, kfreq\nREADING: plain\n",
     "asig poscil", lambda t: t == ""),

    # No fence, and the body opens the way the library's bodies do: with its
    # declarations. They are Csound, written as part of the orchestra, and
    # belong above the cut; below it they were quoted as the reasoning.
    ("no fence, the body opens with its comment head",
     REASONING + "\n\n; FOLLOWS kfreq: yes\n; the bow: continuous noise\n"
     + BODY + "\nREADING: bowed cello\n",
     "; FOLLOWS kfreq: yes", lambda t: t.endswith("never perfectly steady")),

    ("no fence, a macro body under its comment head",
     MACRO_REASONING + "\n\n" + MACRO_BODY + "\nREADING: standing bar\n",
     "; FOLLOWS kfreq: yes", lambda t: t == MACRO_REASONING),

    ("no fence, one-line macro definitions",
     MACRO_REASONING + "\n\n" + MACRO_ONE_LINE + "\nREADING: folded sine\n",
     "; a sine through", lambda t: t == MACRO_REASONING),

    ("no fence and no reasoning, a macro body",
     MACRO_BODY + "\nREADING: standing bar\n",
     "; FOLLOWS kfreq: yes", lambda t: t == ""),

    # Four shapes an adversarial pass over the case-sensitive _STRIP and the
    # macro arms reproduced, each of which reached the compiler and failed there.
    ("header lines in capitals inside the fence",
     "A bell.\n\n```csound\nKSMPS = 64\nNCHNLS = 2\n0DBFS = 1\nInstr 1\n"
     "aexc    rand 0.05\nasig    mode aexc, kfreq * koct1, 900\n"
     "Outs asig, asig\nEndin\nREADING: bell\n```\n",
     "aexc    rand", lambda t: t == "A bell."),
    ("no fence, a formula between dollar signs above the code",
     "A struck bar: modes at\n$f_n = f_1 (2n+1)^2 / 9$\n\n"
     "aexc    rand 0.05\nasig    mode aexc, kfreq * koct1, 900\nREADING: bar\n",
     "aexc    rand", lambda t: "$f_n" in t),
    ("no fence, a lone hash in the reasoning",
     "Plan\n#\naexc    rand 0.05\nasig    mode aexc, kfreq * koct1, 900\n"
     "READING: x\n",
     "aexc    rand", lambda t: t.startswith("Plan")),
    ("no fence, a macro whose first line is a compound assignment",
     "A bar.\nnoise rings it\n\n#define M(R) #\nasum += $R\n#\n"
     "aex     rand 0.05\nasum    = 0\n$M(1)\nasig    = asum\nREADING: x\n",
     "#define M(R) #", lambda t: t == "A bar.\nnoise rings it"),

    ("nothing but a fence",
     "```csound\nasig poscil 0.4, kfreq\nREADING: bare\n```",
     "asig poscil", lambda t: t == ""),

    ("CRLF line endings",
     "Thinking.\r\n```csound\r\nasig poscil 0.4, kfreq\r\nREADING: crlf\r\n```\r\n",
     "asig poscil", lambda t: t == "Thinking."),

    # The thinking is a QUOTATION. A block the author quoted mid-sentence is part
    # of that sentence; deleting it leaves the sentence with its object missing
    # and still presents it as verbatim.
    ("a block quoted inside the reasoning survives",
     "The library saw idiom is:\n```\nasig vco2 0.5, kfreq\n```\n"
     "but a struck bar is closer.\n```csound\nares mode aexc, kfreq, 240\n"
     "READING: bar\n```",
     "ares mode",
     lambda t: "asig vco2 0.5, kfreq" in t and "but a struck bar is closer." in t),

    # Chat-template control tokens leak into the reply on the shipped GGUF. They
    # are the tokeniser's, not the author's, and must not be quoted as thinking.
    ("chat-template control tokens are not the author's words",
     "<|channel>thought\n<channel|>Reasoning.\n```csound\nasig poscil 0.4, kfreq\n```",
     "asig poscil", lambda t: t == "Reasoning."),
]

failures = []

# The corpus must be able to FAIL. Both halves of that: reasoning that a content
# score mistakes for code, and a quotation longer than the body. A corpus whose
# prose looks nothing like code passes every case above no matter how the picker
# is written, and reads as proof while proving nothing.
#
# Measured BOTH ways, because the two disagree and each catches a different way
# of getting this wrong. The line matcher alone is what a shape-based rule sees;
# _codeishness is what the picker's own fallback consults, and a corpus that has
# teeth against one yardstick can be toothless against the other — which is
# exactly how the "prose after the fence" family stayed invisible.
def _looks_like_code(chunk):
    return sum(1 for ln in chunk.splitlines()
               if ln.strip() and not L._SENTENCE_END.search(ln)
               and L._CODE_LINE.match(ln))


# The reasoning is measured by SHAPE only. _codeishness scores it low on
# purpose — that guard is part of the fix — so demanding it score high there
# would be demanding the fix not work. What has to stay true is that a rule
# reading only the shape of the lines WOULD be fooled by this reasoning.
_prose, _body = _looks_like_code(REASONING), _looks_like_code(BODY)
if _prose <= _body:
    failures.append(f"corpus has no teeth: {_prose} lines of the reasoning read as code "
                    f"against {_body} in the body — a shape-based picker would pass by luck")

# The quotation is measured BOTH ways, because that one is about length and not
# about English: it must outweigh the body under whichever metric arbitrates.
for _label, _measure in (("shape", _looks_like_code), ("picker", L._codeishness)):
    _b, _lib = _measure(BODY), _measure(LIB_IDIOM)
    if _lib <= _b:
        failures.append(f"corpus has no teeth by {_label}: quoted idiom scores {_lib}, "
                        f"body {_b} — the quotation must be the LONGER of the two")

for name, reply, body_starts, thinking_ok in CASES:
    body, reading, thinking = L.sanitize(reply)
    first = body.splitlines()[0] if body.strip() else "<EMPTY BODY>"
    if not first.startswith(body_starts):
        failures.append(f"{name}: body starts {first!r}, expected {body_starts!r}")
    elif not thinking_ok(thinking):
        failures.append(f"{name}: thinking is {thinking!r}")
    else:
        print(f"  ok   {name}", flush=True)

# The two must never be the same text: if they are, the split put one segment on
# both sides and the panel would caption the orchestra as its own reasoning.
for name, reply, _, _ in CASES:
    body, _, thinking = L.sanitize(reply)
    if thinking.strip() and thinking.strip() == body.strip():
        failures.append(f"{name}: thinking IS the body")

# ── What is still IN the body ───────────────────────────────────────────────
# The cases above ask where the body STARTS. That cannot see a line taken out
# of the middle, and both ways of getting this wrong are silent until the
# compiler speaks: a `#define` left behind in the reasoning ("Undefined macro"),
# and a variable the header rule mistook for the header. Csound is
# case-sensitive: `kr` is the control rate, `kR` is a variable — the library's
# drum_head has 55 lines of it — and a rule that reads any case took all of
# them out of a fenced, well-formed reply.
# (name, reply, lines that must be in the body, lines that must not)
KEEPS = [
    ("a variable named kR is not the header's kr",
     "A rubbed skin.\n\n```csound\nkrad    = 0.375 * koct1\nkR      = krad * krad\n"
     "Sr      = \"skin\"\nasig    poscil kR, kfreq\nREADING: skin\n```\n",
     ["krad    = 0.375 * koct1", "kR      = krad * krad", 'Sr      = "skin"',
      "asig    poscil kR, kfreq"], []),
    ("a header, an instr wrapper and an output are the host's",
     "```csound\nsr = 44100\nkr = 4410\nksmps = 64\nnchnls = 2\n0dbfs = 1\n"
     "instr 1\nasig poscil 0.4, kfreq\nouts asig, asig\nendin\nREADING: x\n```\n",
     ["asig poscil 0.4, kfreq"],
     ["sr = 44100", "kr = 4410", "ksmps = 64", "nchnls = 2", "0dbfs = 1",
      "instr 1", "outs asig, asig", "endin"]),
    # The two arms that are not Csound still read any case.
    ("CSD tags and the SET line in any case",
     "```csound\n<csinstruments>\nasig poscil 0.4, kfreq\nSet: osc1_level = 0.8\n"
     "</CsInstruments>\nREADING: x\n```\n",
     ["asig poscil 0.4, kfreq"],
     ["<csinstruments>", "Set: osc1_level = 0.8", "</CsInstruments>"]),
    ("no fence, a macro body: every line of it",
     MACRO_REASONING + "\n\n" + MACRO_BODY + "\nREADING: standing bar\n",
     MACRO_BODY.splitlines(), MACRO_REASONING.splitlines()),
    ("no fence, one-line macro definitions: every line of them",
     MACRO_REASONING + "\n\n" + MACRO_ONE_LINE + "\nREADING: folded sine\n",
     MACRO_ONE_LINE.splitlines(), MACRO_REASONING.splitlines()),
    ("a fenced macro body: every line of it",
     MACRO_REASONING + "\n\n```csound\n" + MACRO_BODY + "\nREADING: standing bar\n```\n",
     MACRO_BODY.splitlines(), MACRO_REASONING.splitlines()),
]

for name, reply, must_keep, must_drop in KEEPS:
    body, _, _ = L.sanitize(reply)
    have = [ln.strip() for ln in body.splitlines()]
    lost = [ln for ln in must_keep if ln.strip() not in have]
    kept = [ln for ln in must_drop if ln.strip() in have]
    if lost:
        failures.append(f"keeps/{name}: the body lost {lost!r}")
    elif kept:
        failures.append(f"keeps/{name}: the body still carries {kept!r}")
    else:
        print(f"  ok   keeps: {name}", flush=True)

# ── The LIVE reasoning has to end up saying the same thing ──────────────────
# _live_watch runs on a buffer truncated at an arbitrary character, so it can
# fail in ways sanitize() cannot: a fence pattern ending in `$` matches the end
# of the BUFFER as readily as the end of a line, and a reply whose fence is not
# at a line start (or absent) gives it nothing to stop at. Both are invisible in
# a finished authoring — the answer is right, only the panel was wrong.
#
# Fed one CHARACTER at a time, which is worse than any real token boundary.
LIVE_CASES = [
    ("well-formed",
     "What IS this sound: a cello, bowed\nthe bow keeps it going\n"
     "```csound\naexc dust 1.0, 3900\nasig mode aexc, kfreq, 240\nREADING: cello\n```\n"),
    # ```mode``` mid-sentence: half-arrived, it looks like a finished fence line.
    ("an inline code span inside the reasoning",
     "What IS this: a struck bell.\n```mode``` is the right opcode, not vco2.\n"
     "it rings for seconds\n```csound\nasig mode aexc, 440, 900\nREADING: bell\n```\n"),
    ("no fence anywhere",
     "A struck bell, metal, long decay\nit needs an impulse and modes\n"
     "aexc mpulse 1, 0\nasig mode aexc, 440, 900\nkdet linseg 1, 3, 0.98\nREADING: bell\n"),
    # The same without a marker, for the two shapes whose first code line the
    # line matcher used to miss: the comment head and the macro definition.
    ("no fence, a comment head above the code",
     "A struck bell, metal, long decay\nit needs an impulse and modes\n"
     "; FOLLOWS kfreq: yes\n; the strike: one sample of energy\n"
     "aexc mpulse 1, 0\nasig mode aexc, 440, 900\nREADING: bell\n"),
    ("no fence, a macro body under its comment head",
     MACRO_REASONING + "\n\n" + MACRO_BODY + "\nREADING: standing bar\n"),
    ("no fence, one-line macro definitions",
     MACRO_REASONING + "\n\n" + MACRO_ONE_LINE + "\nREADING: folded sine\n"),
    ("the fence opens at the end of a sentence",
     "A bell. It moves as it decays. ```csound\n"
     "asig mode aexc, 440, 900\nREADING: bell\n```\n"),
    # A leaked control token completes into something SHORTER; a growth test
    # leaves the fragment on screen and drops the correction.
    ("a revision that shortens the text",
     "aaaaaaaaaaaaaaaaaaaa <|xx\nyy|>\nnext\n"
     "```csound\nasig poscil 0.4, kfreq\nREADING: x\n```\n"),
    # "aexc is the bow, continuous noise" is a sentence ABOUT the code and reads
    # as code to any line matcher. Stopping at the first such line truncates the
    # reasoning of almost every real reply.
    ("sentences that read as code must not cut the reasoning short",
     "What IS this sound: a cello\naexc is the bow, continuous noise\n"
     "kstr holds the played note\nand the body is wood\n"
     "```csound\naexc dust 1.0, 3900\nasig mode aexc, kfreq, 240\nREADING: cello\n```\n"),
    # ONE code-ish line inside the reasoning ("attack fast, decay slow" carries
    # no English function word, no sentence end, and matches the code shape),
    # and a fence whose block opens with comments. That combination printed the
    # ENTIRE paragraph under WRITING in monospace — duplicating the THINKING
    # station right above it — for as long as the comments took to arrive.
    ("a code-ish line in the reasoning, then a fence that opens with comments",
     "What IS this sound: a struck bell, metal\nattack fast, decay slow\n"
     "no bow, no breath, one impulse and it is over\n"
     "```csound\n; the strike: one sample of energy\n; then the modes ring\n"
     "aexc mpulse 1, 0\nasig mode aexc, kfreq, 900\nREADING: bell\n```\n"),
]

def stream_live(reply):
    """The whole reply through _live_watch one CHARACTER at a time — worse than
    any real token boundary — plus the end-of-generation flush, exactly as
    build_csound_response drives it."""
    frames, code_frames = [], []
    on_delta = L._live_watch(lambda a, t: frames.append(t),
                             lambda a, t: code_frames.append(t), 1)
    for ch in reply:
        on_delta(ch)
    on_delta.flush()
    return frames, code_frames


def check_live_body(name, reply):
    """The BODY stream must end where sanitize ends, line for line: what the
    panel watched being written has to be the code the card then carries.
    Compared on non-empty lines (sanitize drops blanks); the one line sanitize
    ADDS — the trailing `asig = <var>` recovery for a body routed through
    `out` under another name — never streamed, so it alone may be missing.

    And no frame ON THE WAY there may show the REASONING as code. That is the
    second assertion and it is not redundant: a splitter can converge perfectly
    on the last frame and still print the whole paragraph in monospace for
    seconds in the middle (adversarial-review finding — one code-ish line like
    "attack fast, decay slow" in the prose was enough to arm it the moment a
    fence opened, duplicating the THINKING station right below itself).
    Checking only the final frame cannot see that, and the middle frames ARE
    the feature."""
    _, code_frames = stream_live(reply)
    body, _, thinking = L.sanitize(reply)
    live_code = code_frames[-1] if code_frames else ""
    want = [ln for ln in body.splitlines() if ln.strip()]
    got = [ln for ln in live_code.splitlines() if ln.strip()]
    if want and re.fullmatch(r"\s*asig\s*=\s*\w+\s*", want[-1]) and got == want[:-1]:
        want = want[:-1]
    if "```" in live_code:
        failures.append(f"live/{name}: a fence marker reached the live code")
    elif got != want:
        failures.append(f"live/{name}: the live code ends on {got!r}, "
                        f"the card carries {want!r}")
    else:
        print(f"  ok   live code: {name}", flush=True)

    # An ENGLISH line the finished answer calls reasoning must never have stood
    # in a body frame. Codeish lines are exempt on purpose: the author quotes
    # the library idiom it is adapting in a fence of its own, and while that
    # block is the only one that has arrived it is legitimately what the panel
    # shows being written — real Csound, replaced by the real body when it
    # comes. Prose is the thing that must never appear there. (Prose sanitize
    # ends up treating as the body — the segment-0 fallback — is not in
    # `thinking`, so this cannot fire on it either.)
    prose = {ln.strip() for ln in thinking.splitlines()
             if ln.strip() and not L._codeishness(ln)}
    for n, frame in enumerate(code_frames):
        leaked = [ln.strip() for ln in frame.splitlines() if ln.strip() in prose]
        if leaked:
            failures.append(f"live/{name}: body frame {n} of {len(code_frames)} showed "
                            f"the reasoning as code: {leaked[:2]!r}")
            break


for name, reply in LIVE_CASES:
    frames, _ = stream_live(reply)
    live = frames[-1] if frames else ""
    _, _, final = L.sanitize(reply)
    if "```" in live or L._codeishness(live) >= 2:
        failures.append(f"live/{name}: Csound reached the panel as reasoning: {live!r}")
    elif live != final:
        failures.append(f"live/{name}: the panel ends on {live!r}, "
                        f"the answer carries {final!r}")
    else:
        print(f"  ok   live: {name}", flush=True)
    check_live_body(name, reply)

# The live body over the FULL malformed-reply corpus, not only the streaming
# cases above: the stray closer, the unclosed final fence, the quoted idiom
# before the real block — the shapes _fence_segments exists for are exactly the
# shapes a live splitter gets wrong first (adversarial-review finding: a parity
# toggle passed all six easy cases and went dark on four of these sixteen).
# The live THINKING is asserted only on LIVE_CASES: sanitize counts prose
# written AFTER the body as thinking, which no live view can know in time.
for name, reply, _, _ in CASES:
    check_live_body("corpus/" + name, reply)

print()
if failures:
    print("FAIL")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print(f"PASS — {len(CASES)} reply shapes, the orchestra came out of every one; "
      f"{len(KEEPS)} bodies kept every line that is theirs and none of the host's; "
      f"{len(LIVE_CASES)} streamed live without losing or leaking a line")
