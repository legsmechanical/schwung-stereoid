# Stereoid — a four-engine stereo widener (audio_fx)

Lifted from DR32's per-pad Stereo page (`schwung-dr32` `dsp/dr32_kit.c`: `wide_run`, `comb_run`,
`haas_run`, `disperse_run`), where the laws were tuned by ear. DR32 keeps its own copy; the two are
not shared code, and a change here does not go back there.

## Shape

- `dsp/stereoid.{h,c}` — the widener. Interleaved stereo float in place, no host types. The laws and
  where each came from are in the comment above each engine.
- `src/stereoid_module.c` — the host contract only (audio_fx v2, int16, stringly params, state).
- `src/module.json` — ONE page of eight. `late` (Haas only) and `time` (not M/S) are gated on `mode`
  with `visible_if`, which is a LEVEL field: in `chain_params` it hides nothing.
- `tests/test_stereoid.c` — every law, plus the host contract. `tools/pages_check.mjs` runs
  upstream's validator and planner once per engine.

## Decisions (Josh, 2026-10-01)

- **Each engine keeps its own answer to a stereo input**: Comb widens the mid, Disperse each channel
  from itself, Haas delays one whole channel. No source switch.
- **Everything on one page**, and only the knobs that apply to the engine show. A hidden knob closes
  up, so the one engine-only knob is LAST: MODE WIDE WFREQ TIME / TRIM COMP HICUT, then LATE in
  Haas. Comb and Disperse show seven, Haas eight, M/S six (no TIME).
- **Headroom is TRIM and COMP together** (Josh: *"why not add the multi-mode compensation we
  discussed earlier and keep trim"*). TRIM is a plain output level. COMP is Off | Loud | Peak, a trim
  that follows the width: Loud `1/sqrt(1+g^2)` (DR32's COMP), Peak `1/(1+g)`. Haas is trimmed only
  for what LATE adds above 0 dB. Nothing LIMITS the output; past full scale the int16 clips. Peak's
  bound is for the unfiltered worst case — a filter or all-pass can overshoot it slightly.
- **M/S, the fourth engine** (2026-10-03). Josh, on stereo content: *"is it normal to not be able to
  tell much difference?"* — yes: the other three CREATE width, so an already-wide source has little
  to gain. M/S scales the side the input already has (`k = 1 + WIDE/100`, -100 exactly mono), inside
  the WFREQ..HICUT band. A mono input is left alone. COMP treats k as Haas treats LATE.
- The measured model is called **Disperse**, never by the name of the product it was measured from.

## Rules that bite

- **WIDE 0 is a true bypass** of the widener, and WIDE 0 with TRIM 0 skips the int16 round trip
  altogether. The int16 scale is 32768 both ways so that a sample the widener does not move comes
  back as the integer it was.
- **Settled is exact; moving is approached.** Gains ramp across a block and the delay slews at 0.25
  frames a frame, because an effect sits on sustained audio where a stepped delay is a click. A
  whole-frame delay reads the frame itself, so the exact-law tests hold once it lands. A fresh
  stage (first block, engine change) starts AT its targets.
- **`-O3`, not `-Ofast`**: the tests pin exact arithmetic, and fast-math may reorder it.
- **Haas's three bands must sum flat.** With WFREQ and HICUT both set, the low band goes through the
  HICUT crossover's all-pass too. The test uses close corners (2 k / 3 k) because far-apart ones
  hide the omission.
- **There is no TONE knob** (Josh: *"just get rid of tone"*). Moving Disperse's all-pass corners
  together was built, measured and removed: they are log-spaced, group delay ~0.27/f, so the moved
  curve is the same curve (under 0.05 ms change per octave of knob). An audible handle would be the
  AMOUNT of dispersion (passes through the chain); offered and declined.
- **Clean-room.** Disperse was fitted to a commercial plugin's OUTPUT (impulse renders). Those
  renders, and the product's name as a mode name, stay out of this repo.
- The compiler is pinned (gcc 12.2.0) and `build.sh` reads the pin out of the built `.so`.
- Install to the STOCK tree only (`schwung/modules/audio_fx/stereoid`); dbx-host's `audio_fx` is a
  symlink to it.

## Not done yet

No release workflow, `release.json`, `help.json` or catalog entry. Not yet heard on the device.
