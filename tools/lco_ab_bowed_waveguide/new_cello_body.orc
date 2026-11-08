; A BOWED STRING -- a bow on a string, the arm that draws it, and the box it is mounted on.
;
; STRING  the head's `t5bow`: a waveguide string in velocity waves with the bow's friction
;         curve at the contact point (Smith, ICMC 1986; McIntyre, Schumacher & Woodhouse,
;         JASA 1983). The hair sticks to the string and slips off it, and that cycle IS the
;         tone: nothing in it is noise. The string loses its top at the bridge through a
;         one-pole whose cutoff is in Hz, so it is the same string at 44100 and at the 4x
;         the plugin runs -- which is what `wgbow` is not.
; ARM     the stroke. At the start of a note the hair bites -- more weight, less speed --
;         and lets go over the first tenth of a second; afterwards weight and speed keep
;         drifting, because a drawn bow never stands still. All of it in the arm, none of
;         it on the pitch. The weight itself is not a knob: measured over slope 1.0..2.5
;         it moves the colour by a tenth and nothing else a player would reach for, and
;         heavier than slope 1.25 (lower numbers are more weight) it lets the string fall
;         into double-period motion away from the bridge. It stands at 2.0, where the bow
;         holds the fundamental at every pitch.
; BOX     what radiates is the string at the bridge through the body: the violin's four
;         signature modes as `mode` filters -- A0 272 Hz, CBR 407, B1- 462, B1+ 551 -- and
;         the bridge hill around 2.3 kHz (J. Woodhouse, Euphonics 5.3). `box` scales them;
;         at 0.37 they sit where a cello's do. The Q values are not in the source and are
;         not pretended to be: they are this entry's, by ear. Without the box a waveguide
;         string is a buzz. Around 880-1100 Hz the hill lifts the second and third
;         partial above the fundamental, as a violin's body does at A5.
; FOLLOWS kfreq: yes, to about kplace * 14700 Hz at 44100 and kplace * 19600 Hz at 4x
;         (1.9 and 2.5 kHz at the default place); above that the bow walks toward the
;         middle by itself, and above about 6 kHz the pitch is wrong. The bowed note sits a few
;         cents SHARP, growing with pitch and with darkness -- the hair catches the
;         smeared corner as soon as it enters the stick window, before it has fully
;         arrived. Measured at the defaults, both rates: +0.5 c at 55 Hz, +1 at 110, +2 at
;         220, +4 at 440, +8 at 880, +13 at 1320. The free string (bow removed) is on
;         kfreq to 0.01 cents at both rates; the rest is the bow's.
; DECAY   none: the bow is always drawn and the host's envelope owns the note.
kplace  = 0.130  ; place [0.10..0.20]: where along the string the bow is drawn, near the bridge to a fifth along
kgut    = 0.60   ; gut [0.00..1.00]: the string itself, from gut (dark and warm) to steel (bright and wiry)
kbox    = 0.37   ; box [0.35..1.20]: the size of the body, from a cello up past a violin
kf      limit kfreq * koct1, 20, 12000
kcatch  = exp(-knote * 14.0)                             ; the hair bites, then lets go
kdraw   poscil 0.12, 0.190                               ; the weight the arm carries drifts
kdraw2  poscil 0.07, 0.310
kbreath poscil 0.15, 0.130                               ; and so does how fast it is drawn
kslope  limit 2.0 * (1 - 0.30 * kcatch + kdraw + kdraw2), 0.8, 2.5   ; the weight: STK's slope, 1 = leaning in
kspeed  = 0.10 * (1 - 0.60 * exp(-knote * 30.0)) * (1 + kbreath)
kcut    = 1400 * exp(1.7918 * kgut)                      ; 1400 Hz (gut) .. 8400 Hz (steel), 4100 at the default
kcut    limit kcut, min(3 * kf, 20000), 20000            ; a string cannot be darker than its own third partial
astr    t5bow kf, kspeed, kslope, kplace, kcut
aa0     mode astr, 272 * kbox, 18                        ; the air through the f-holes
acbr    mode astr, 407 * kbox, 20                        ; and the three wood modes
ab1m    mode astr, 462 * kbox, 25
ab1p    mode astr, 551 * kbox, 25
ahill   mode astr, 2300 * kbox, 4                        ; the bridge hill
abody   = aa0 * 0.050 + acbr * 0.035 + ab1m * 0.040 + ab1p * 0.040 + ahill * 0.25
abodyb  balance abody, astr, 1                           ; the box is a FILTER, not a fader:
amix    = astr * 0.30 + abodyb * 0.85                    ;   held against what went into it
aref    poscil 0.35, 400                                 ; a fixed-loudness yardstick, as `string`
abal    balance amix, aref                               ;   has it: place and gut move the
asig    = abal * 0.85                                    ;   colour, not the level