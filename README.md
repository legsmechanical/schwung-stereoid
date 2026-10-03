# Stereoid

A stereo widener for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move: one audio
effect, four engines, one page of knobs.

| Engine | What it does | A stereo input | Mono sum |
|---|---|---|---|
| **Comb** | Adds a delayed copy of the mid to one side and subtracts it from the other (a Lauridsen complementary comb). No lean. | Keeps the width it came with; the mid is what widens | Exactly the input |
| **Haas** | Delays one side. The image leans toward the side that arrives first; that lean is the character. | One whole channel is delayed | Combs |
| **Disperse** | An all-pass and comb widener modelled from measurement of a commercial widener's output. | Each channel widens from itself; a hard-panned sound stays on its side | Exact for a mono input |
| **M/S** | Scales the side the input already has: the classic width control. Does nothing to a mono source; the most audible on a stereo mix. | Its whole job | Exactly the input |

Comb, Haas and Disperse create width, so they do the most to mono or centred material. M/S turns up
(or down, to mono at -100) the width that is already there.

## Knobs

| Knob | Range | |
|---|---|---|
| MODE | Comb, Haas, Disperse, M/S | The engine |
| WIDE | -100..+100 % | Width. 0 is a true bypass. The sign mirrors: which side is delayed (Haas), or which side gets which comb teeth. In M/S it is the side level: -100 mono, +100 double |
| WFREQ | 20..4000 Hz | Nothing below it is widened. 20 = full band |
| TIME | 0..12 ms | Not in M/S. The delay. 0 = Auto: each engine's own (Comb 8 ms; Haas and Disperse follow WIDE). In Haas, setting TIME makes WIDE the delayed side's mix |
| TRIM | -12..0 dB | Output level, by hand. A widener adds to each channel: at full width one can reach twice the input, and nothing here limits it |
| COMP | Off, Loud, Peak | A trim that follows the width. Loud holds the loudness as you widen (up to -3 dB); Peak holds the worst-case peak (up to -6 dB). Either lowers the mono sum by the same amount; Off leaves it exact |
| HICUT | 1000..20000 Hz | Nothing above it is widened. 20000 = off |
| LATE | -12..+12 dB | Haas only: the delayed side's level. Up counters the lean, down deepens it |

Requires Schwung 1.5.0.

## Build and test

```sh
tests/run.sh                              # the offline suite, no device
scripts/build.sh && scripts/install.sh    # cross-compile in Docker, deploy to move.local
```

MIT licensed.
