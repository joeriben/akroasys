kdamp   = 0.00   ; damp [0.00..1.00]: the flat of the hand on the string, from left ringing to stopped
kpick   = 0.185  ; pick [0.10..0.50]: where along the string it is set going, as a fraction of its length
kstr    limit kfreq * koct1, 20, 12000
kfast   = exp(-knote * 20.00)                  ; the first instant's brilliance goes at once
kslow   = exp(-knote * 0.091)                  ; and the string keeps losing its top all the while
kbrth   poscil 0.120, 1.900                ; the hand behind it is never quite steady
kcut    limit 441.0 * (1 + 1.44 * kfast) * (1 + 1.38 * kslow) * (1 + kbrth) * (1 - 0.85 * kdamp), 60, 18000
aexd    dust 1.0, 3900.0                        ; the drive arrives as separate touches, not as
aexd    alpass aexd, 0.1100, 0.00710                ;   a wall of noise -- a wall is an electric
aexd    alpass aexd, 0.0820, 0.00470                ;   string driver, not a played string. Each
aexd    alpass aexd, 0.0640, 0.00310                ;   touch is then smeared out in time so none
aexd    alpass aexd, 0.0480, 0.00190                ;   of them arrives as a click. Allpasses
aexd    alpass aexd, 0.0340, 0.00113                ;   spread WITHOUT darkening; a lowpass here
aexd    alpass aexd, 0.0260, 0.00079                ;   buries the instrument's own colour
aexd    alpass aexd, 0.0200, 0.00053
aexd    alpass aexd, 0.0160, 0.00037
aexb    tone aexd, kcut
apik    vdelay aexb, (kpick / kstr) * 1000, 60
aexc    = aexb - apik
astr    streson aexc, kstr, 0.9884
aref    poscil 0.35, 400                          ; a fixed-loudness yardstick: how scattered the
abal    balance astr, aref                        ; drive is must not change how loud the tone is
asig    = abal * 0.85