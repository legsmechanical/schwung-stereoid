/* stereoid.c — Comb, Haas and Disperse, one stage, three engines.
 *
 * Lifted from DR32's per-pad widener (schwung-dr32 dsp/dr32_kit.c: wide_run,
 * comb_run, haas_run, disperse_run), where every law below was tuned by ear
 * and pinned by test. What a standalone effect adds to it:
 *   - HICUT, the top of the widened band (DR32 has only WFREQ, the bottom)
 *   - TRIM, a plain output level, and COMP grown a third setting, Peak
 *   - gains and the delay are approached rather than jumped to
 *
 * Each engine keeps its own answer to a STEREO input (Josh: "have each engine
 * keep its stereo behavior"): Comb widens the mid, Disperse widens each
 * channel from itself, Haas delays one whole channel.
 *
 * MIT licensed (see LICENSE).
 */
#include "stereoid.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f
#define RING_MASK (STEREOID_RING - 1)

#define COMB_MS             8.0f      /* Comb's Auto delay */
#define HAAS_MS_MAX         15.0f     /* Haas's Auto delay at |WIDE| 100: past it a transient flams */
#define DISPERSE_MS_PER_PCT 0.0300f   /* Wider's delay law, per Wider-% */
#define SLEW                0.25f     /* frames of delay per frame: 12 ms is crossed in ~50 ms */

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- filters: Andy Simper's trapezoidal SVF, Q = 1/sqrt(2) ---------------- */

static void svf_set(stereoid_svf *f, float hz) {
    if (f->hz == hz) return;
    const float t = tanf(PI_F * hz / STEREOID_SR);
    f->k = 1.41421356f;
    f->a1 = 1.0f / (1.0f + t * (t + f->k));
    f->a2 = t * f->a1;
    f->a3 = t * f->a2;
    f->hz = hz;
}

static inline void svf_tick(const stereoid_svf *f, float st[2], float v0, float *lp, float *hp) {
    float v3 = v0 - st[1];
    float v1 = f->a1 * st[0] + f->a2 * v3;
    float v2 = st[1] + f->a2 * st[0] + f->a3 * v3;
    st[0] = 2.0f * v1 - st[0];
    st[1] = 2.0f * v2 - st[1];
    *lp = v2;
    *hp = v0 - f->k * v1 - v2;
}

/* 24 dB Linkwitz-Riley: two Butterworth sections, -6 dB at the corner. */
static inline float lr4_hp(const stereoid_svf *f, float (*st)[2], float v) {
    float lp, hp;
    svf_tick(f, st[0], v, &lp, &hp);
    svf_tick(f, st[1], hp, &lp, &hp);
    return hp;
}

static inline float lr4_lp(const stereoid_svf *f, float (*st)[2], float v) {
    float lp, hp;
    svf_tick(f, st[0], v, &lp, &hp);
    svf_tick(f, st[1], lp, &lp, &hp);
    return lp;
}

/* Both halves of the crossover; lo + hi is the input through an all-pass. */
static inline void lr4_split(const stereoid_svf *f, float (*st)[2], float v, float *lo, float *hi) {
    float l1, h1, dump;
    svf_tick(f, st[0], v, &l1, &h1);
    svf_tick(f, st[1], l1, lo, &dump);
    svf_tick(f, st[2], h1, &dump, hi);
}

/* ---- the delay: a ring read `delay` frames back -------------------------- *
 * A whole number of frames reads the frame itself, so a settled Comb or Haas
 * is exact. Between frames, a 4-point cubic (Catmull-Rom): straight-line
 * interpolation darkened Disperse's top 1-3 dB more than Wider does. */
static inline float ring_read(const float *r, int w, float delay) {
    const int   Di = (int)delay;
    const float Df = delay - (float)Di;
    const float y0 = r[(w - Di) & RING_MASK];
    if (Df == 0.0f) return y0;
    const float ym = Di > 0 ? r[(w - Di + 1) & RING_MASK] : y0;
    const float y1 = r[(w - Di - 1) & RING_MASK];
    const float y2 = r[(w - Di - 2) & RING_MASK];
    const float c1 = 0.5f * (y1 - ym);
    const float c2 = ym - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym) + 1.5f * (y0 - y1);
    return ((c3 * Df + c2) * Df + c1) * Df + y0;
}

static inline float slew(float cur, float to) {
    const float d = to - cur;
    if (d > SLEW) return cur + SLEW;
    if (d < -SLEW) return cur - SLEW;
    return to;
}

/* ---- COMB ----------------------------------------------------------------- *
 * A complementary-comb widener (Lauridsen's):
 *
 *     side = g * BP(mid)(t - TIME)      mid = (L + R) / 2,  g = WIDE / 100
 *     L += side        R -= side
 *
 * Both sides are treated alike and opposite, so the image does not lean, and
 * L + R is untouched — the added copy cancels — so the mono sum is exactly
 * the input, whatever the input is. A stereo input keeps the width it had.
 * Width is linear in g: the side is 20*log10(g) dB under the mid. A negative
 * WIDE flips the side's sign: the same width, the comb teeth mirrored.
 * BP is WFREQ's high-pass and HICUT's low-pass: only that band is added, so
 * nothing outside it widens and nothing needs splitting.
 */
static void comb_run(stereoid *s, float pct, float *x, int frames, int first) {
    const stereoid_params *p = &s->p;
    const float g_to = pct * 0.01f;
    const float ms = p->time > 0.0f ? p->time : COMB_MS;
    const float d_to = (float)(int)(ms * 0.001f * STEREOID_SR + 0.5f);
    const int hp = p->freq > STEREOID_FREQ_MIN + 0.5f;
    const int lp = p->hicut < STEREOID_HICUT_MAX - 0.5f;
    if (hp) svf_set(&s->lo, p->freq);
    if (lp) svf_set(&s->hi, p->hicut);
    if (first) { s->g = g_to; s->delay = d_to; }
    float g = s->g;
    const float g_inc = (g_to - g) / (float)frames;
    for (int i = 0; i < frames; i++) {
        float m = 0.5f * (x[2 * i] + x[2 * i + 1]);
        if (hp) m = lr4_hp(&s->lo, s->st[0] + 0, m);
        if (lp) m = lr4_lp(&s->hi, s->st[0] + 2, m);
        s->ring[0][s->w] = m;
        s->delay = slew(s->delay, d_to);
        const float side = g * ring_read(s->ring[0], s->w, s->delay);
        s->w = (s->w + 1) & RING_MASK;
        x[2 * i]     += side;
        x[2 * i + 1] -= side;
        g += g_inc;
    }
    s->g = g_to;
}

/* ---- HAAS ----------------------------------------------------------------- *
 * One channel's band is delayed — + the RIGHT, - the LEFT — so the image
 * LEANS toward the side that arrives first (the precedence effect): that lean
 * is this engine's character. Auto: the delay is 15 ms x (WIDE/100)^2, fully
 * delayed (width comes on between 0 and ~3 ms; the cap is where a transient
 * stops fusing and reads as a flam). With TIME set, a Haas plugin's layout:
 * TIME the delay, |WIDE| the delayed side's MIX (dry at 0, fully delayed at
 * 100). LATE is the delayed band's level: up counters the lean — precedence
 * traded against intensity — down deepens it.
 *
 * Both channels are split the same way (LR4, so the bands stay in phase):
 * below WFREQ and above HICUT nothing is delayed. With both corners set, the
 * low band passes through the HICUT crossover's all-pass too, or the three
 * bands would not sum flat.
 */
static void haas_run(stereoid *s, float pct, float *x, int frames, int first) {
    const stereoid_params *p = &s->p;
    const float a = pct * 0.01f;
    const int manual = p->time > 0.0f;
    const float ms = manual ? p->time : HAAS_MS_MAX * a * a;
    const float mix_to = manual ? fabsf(a) : 1.0f;
    const float late_to = powf(10.0f, clampf(p->late, -STEREOID_LATE_MAX, STEREOID_LATE_MAX) * 0.05f);
    const float d_to = (float)(int)(ms * 0.001f * STEREOID_SR + 0.5f);
    const int dch = a < 0.0f ? 0 : 1;
    if (!first && dch != s->haas_side) memset(s->ring, 0, sizeof(s->ring));   /* the other side now: start clean */
    s->haas_side = dch;
    const int split = p->freq > STEREOID_FREQ_MIN + 0.5f;
    const int cut = p->hicut < STEREOID_HICUT_MAX - 0.5f;
    if (split) svf_set(&s->lo, p->freq);
    if (cut) svf_set(&s->hi, p->hicut);
    if (first) { s->mix = mix_to; s->late_g = late_to; s->delay = d_to; }
    float mix = s->mix, late_g = s->late_g;
    const float mix_inc = (mix_to - mix) / (float)frames;
    const float late_inc = (late_to - late_g) / (float)frames;
    for (int i = 0; i < frames; i++) {
        s->delay = slew(s->delay, d_to);
        for (int c = 0; c < 2; c++) {
            const float v = x[2 * i + c];
            float lo = 0.0f, band = v, top = 0.0f;
            if (split) lr4_split(&s->lo, s->st[c] + 0, v, &lo, &band);
            if (cut) {
                const float in = band;
                lr4_split(&s->hi, s->st[c] + 3, in, &band, &top);
                if (split) {
                    float l2, h2;
                    lr4_split(&s->hi, s->st[c] + 6, lo, &l2, &h2);
                    lo = l2 + h2;
                }
            }
            if (c == dch) {
                s->ring[c][s->w] = band;
                const float late = ring_read(s->ring[c], s->w, s->delay);
                band = (mix >= 1.0f ? late : band + mix * (late - band)) * late_g;
            }
            x[2 * i + c] = lo + band + top;
        }
        s->w = (s->w + 1) & RING_MASK;
        mix += mix_inc;
        late_g += late_inc;
    }
    s->mix = mix_to;
    s->late_g = late_to;
}

/* ---- DISPERSE ------------------------------------------------------------- *
 * Polyverse's Wider, MEASURED. Not from its code: from its OUTPUT. Josh
 * rendered a single-sample click through Wider in Ableton at Width 0-200%,
 * with a left-only input and with Low Bypass at 200 Hz (32-bit float,
 * 44.1 kHz), and the responses were fitted, every part to within a fraction
 * of a percent:
 *
 *     L_out = L + F(L)        R_out = R - F(R)      (each channel on its own)
 *     F     = g * AP5( delay_D( BP(x) ) )
 *     g     = min(W / 100, 1)          -12 dB at 25%, -6 at 50, 0 dB from 100 on
 *     D     = 0.0300 ms * W            3 ms at 100%, 6 ms at 200%: the second
 *                                      half of the knob only LENGTHENS the delay
 *     AP5   = five first-order all-passes, the same whatever W is
 *     BP    = Low Bypass, a 24 dB Linkwitz-Riley high-pass (WFREQ)
 *
 * For a mono input that is the M/S shape Comb has (side = F(mid), mono sum
 * exact); for a stereo one each side widens from itself, as Wider does — a
 * hard-panned sound stays on its side, and the mono sum gains F(L - R).
 * The delay makes the comb and the all-passes smear it.
 *
 * WIDE (-100..+100) spans Wider's 0-200%: |WIDE| x 2. The sign mirrors (which
 * side gets +F). TIME overrides the delay. Not Wider's: HICUT, a matching
 * low-pass on F's input.
 *
 * The all-passes are FIXED, and a knob to move them is not worth a slot: the
 * corners are log-spaced, so the group delay is ~0.27/f and moving them all
 * together reproduces the same curve (measured: an octave moved it by under
 * 0.05 ms at every frequency).
 */
static const float DISPERSE_A[STEREOID_AP_STAGES] = {
    /* (a + z^-1) / (1 + a z^-1); corners 4.4, 41.5, 232, 1281 Hz and one near
     * Nyquist (a > 0). */
    -0.999374f, -0.994100f, -0.967481f, -0.832310f, 0.816475f,
};

static void disperse_run(stereoid *s, float pct, float *x, int frames, int first) {
    const stereoid_params *p = &s->p;
    const float W = 2.0f * fabsf(pct);                    /* Wider's 0..200 % */
    const float g_to = (W < 100.0f ? W * 0.01f : 1.0f) * (pct < 0.0f ? -1.0f : 1.0f);
    const float ms = p->time > 0.0f ? p->time : DISPERSE_MS_PER_PCT * W;
    const float d_to = ms * 0.001f * STEREOID_SR;         /* frames, fractional */
    const int hp = p->freq > STEREOID_FREQ_MIN + 0.5f;
    const int lp = p->hicut < STEREOID_HICUT_MAX - 0.5f;
    if (hp) svf_set(&s->lo, p->freq);
    if (lp) svf_set(&s->hi, p->hicut);
    if (first) { s->g = g_to; s->delay = d_to; }
    float g = s->g;
    const float g_inc = (g_to - g) / (float)frames;
    for (int i = 0; i < frames; i++) {
        s->delay = slew(s->delay, d_to);
        for (int c = 0; c < 2; c++) {
            float v = x[2 * i + c];
            if (hp) v = lr4_hp(&s->lo, s->st[c] + 0, v);
            if (lp) v = lr4_lp(&s->hi, s->st[c] + 2, v);
            s->ring[c][s->w] = v;
            float y = ring_read(s->ring[c], s->w, s->delay);
            for (int j = 0; j < STEREOID_AP_STAGES; j++) {   /* first-order all-passes */
                const float a = DISPERSE_A[j];
                const float o = a * y + s->ap[c][j];
                s->ap[c][j] = y - a * o;
                y = o;
            }
            x[2 * i + c] += c ? -g * y : g * y;
        }
        s->w = (s->w + 1) & RING_MASK;
        g += g_inc;
    }
    s->g = g_to;
}

/* ---- the stage ------------------------------------------------------------ */

void stereoid_defaults(stereoid_params *p) {
    p->mode = STEREOID_COMB;
    p->wide = 50.0f;
    p->freq = 150.0f;
    p->time = 0.0f;
    p->trim = 0.0f;
    p->comp = STEREOID_COMP_OFF;
    p->late = 0.0f;
    p->hicut = STEREOID_HICUT_MAX;
}

void stereoid_init(stereoid *s) {
    memset(s, 0, sizeof(*s));
    stereoid_defaults(&s->p);
}

/* COMP: the trim that follows the width, so a sweep of WIDE holds its level.
 * Off, the mono sum is exactly the input (as Wider's is). On, it drops by
 * the trim. With g the side's level (Comb |WIDE|/100; Disperse min(W/100, 1)):
 *
 *   Loud   1 / sqrt(1 + g^2)   each ear's ENERGY holds: a mid/side widener
 *                              adds g^2 of it, +3 dB at full width
 *   Peak   1 / (1 + g)         each ear's worst-case LEVEL holds: the side
 *                              can land in phase with the dry, +6 dB at full
 *
 * Haas adds nothing — one ear is only delayed — until LATE raises that ear
 * above the other. Then, with l = LATE's gain: Loud sqrt(2 / (1 + l^2)),
 * Peak 1 / l. Never a boost.
 */
static float comp_gain(const stereoid_params *p, float pct) {
    if (p->comp == STEREOID_COMP_OFF || pct == 0.0f) return 1.0f;
    const int peak = p->comp == STEREOID_COMP_PEAK;
    if (p->mode == STEREOID_HAAS) {
        const float l = powf(10.0f, clampf(p->late, -STEREOID_LATE_MAX, STEREOID_LATE_MAX) * 0.05f);
        if (l <= 1.0f) return 1.0f;
        return peak ? 1.0f / l : sqrtf(2.0f / (1.0f + l * l));
    }
    float g = fabsf(pct) * 0.01f;
    if (p->mode == STEREOID_DISPERSE) { g *= 2.0f; if (g > 1.0f) g = 1.0f; }
    return peak ? 1.0f / (1.0f + g) : 1.0f / sqrtf(1.0f + g * g);
}

int stereoid_neutral(const stereoid *s) {
    return s->p.wide == 0.0f && s->p.trim >= 0.0f;
}

void stereoid_flush(stereoid *s) {
    if (!s->live) return;
    memset(s->ring, 0, sizeof(s->ring));
    memset(s->st, 0, sizeof(s->st));
    memset(s->ap, 0, sizeof(s->ap));
    s->w = 0;
    s->live = 0;
}

void stereoid_process(stereoid *s, float *x, int frames) {
    if (frames <= 0) return;
    const stereoid_params *p = &s->p;
    const float pct = clampf(p->wide, -100.0f, 100.0f);

    /* WIDE 0 is a TRUE bypass of the widener, whatever the other knobs say. */
    if (pct == 0.0f) {
        stereoid_flush(s);
    } else {
        /* An engine change starts the stage clean — they share the state. */
        if (s->live && s->mode != p->mode) stereoid_flush(s);
        const int first = !s->live;
        s->mode = p->mode;
        if (p->mode == STEREOID_HAAS) haas_run(s, pct, x, frames, first);
        else if (p->mode == STEREOID_DISPERSE) disperse_run(s, pct, x, frames, first);
        else comb_run(s, pct, x, frames, first);
        s->live = 1;
        /* Through silence the filter states decay into denormals and stay
         * there: the lowest all-pass corner is 4 Hz. */
        for (int c = 0; c < 2; c++) {
            for (int j = 0; j < 9; j++)
                for (int n = 0; n < 2; n++)
                    if (fabsf(s->st[c][j][n]) < 1e-20f) s->st[c][j][n] = 0.0f;
            for (int j = 0; j < STEREOID_AP_STAGES; j++)
                if (fabsf(s->ap[c][j]) < 1e-20f) s->ap[c][j] = 0.0f;
        }
    }

    /* TRIM and COMP: the output level, by hand and by rule. A widener ADDS to
     * each channel — at full width one can reach twice the input, and Haas's
     * LATE adds up to 12 dB more — and nothing here limits it. The mono sum
     * is the input x this, exactly. */
    const float to = powf(10.0f, clampf(p->trim, STEREOID_TRIM_MIN, 0.0f) * 0.05f) * comp_gain(p, pct);
    if (!s->out_set) { s->out_g = to; s->out_set = 1; }
    if (s->out_g == to) {
        if (to != 1.0f)
            for (int i = 0; i < 2 * frames; i++) x[i] *= to;
    } else {
        float t = s->out_g;
        const float inc = (to - t) / (float)frames;
        for (int i = 0; i < frames; i++) {
            x[2 * i] *= t;
            x[2 * i + 1] *= t;
            t += inc;
        }
        s->out_g = to;
    }
}
