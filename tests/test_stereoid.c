/* _GNU_SOURCE is the one feature macro that is additive on both glibc and
 * Darwin under -std=c11 (schwung-dr32 tests/test_split.c has the history). */
#define _GNU_SOURCE

/*
 * Stereoid's laws, measured on its output. Ported from schwung-dr32
 * tests/test_wide.c (the engines are DR32's), on plain buffers instead of a
 * kit, plus what the effect adds: a stereo input, HICUT, TRIM, COMP, the
 * approach to a moved knob, and the host contract.
 *   - WIDE 0 is a TRUE bypass, bit for bit, whatever the other knobs say
 *   - Comb: the MONO SUM is the input at any setting, for a stereo input too;
 *     full band the added side is exactly g x the mid, TIME late (Auto 8 ms);
 *     linear in g; no lean; a negative WIDE mirrors; a stereo input keeps the
 *     side it came with
 *   - Haas: one side 15 ms x (WIDE/100)^2 late, exactly, the other untouched;
 *     with TIME set, TIME is the delay and |WIDE| the delayed side's mix;
 *     LATE scales the delayed side
 *   - Disperse = Polyverse Wider's MEASURED model: the mid is the dry click,
 *     the side's level law (-12/-6/0/0 dB), delay law (0.03 ms per Wider-%)
 *     and group delay, and PER-CHANNEL processing
 *   - WFREQ and HICUT bound the widened band; Haas's three bands sum flat
 *   - M/S scales the side the input already has: the mid untouched, -100
 *     mono, a mono input left alone; WFREQ/HICUT bound which side moves
 *   - TRIM is the output level: the mono sum is the input x it
 *   - COMP is a trim that follows the width: Loud holds each ear's energy,
 *     Peak its worst-case level; Haas is trimmed only for what LATE adds
 *   - a moved knob is approached, not jumped to, and lands exactly
 *   - the module: knobs clamp, read back, survive a state round trip; an
 *     untouched sample comes back as the integer it was
 */
#include "../dsp/stereoid.h"
#include "../shared/audio_fx_api_v2.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host);

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define SR 44100
#define FR 128
#define LEN (FR * 172)          /* ~0.5 s, whole blocks */
#define PI 3.14159265358979

static float snare[2 * LEN];    /* a centred decaying noise burst */
static float wide_in[2 * LEN];  /* the same burst, different on each side */
static float click[2 * LEN];    /* a centred click at 0.1 s */
static float click_l[2 * LEN];  /* the click on the LEFT only */

static void make_signals(void) {
    uint32_t r = 12345u;
    for (int i = 0; i < LEN; i++) {
        r = r * 1664525u + 1013904223u;
        const float n = ((int32_t) r) / 2147483648.0f;
        r = r * 1664525u + 1013904223u;
        const float n2 = ((int32_t) r) / 2147483648.0f;
        const float env = 0.25f * expf(-(float) i / (0.05f * SR));
        snare[2 * i] = snare[2 * i + 1] = n * env;
        wide_in[2 * i] = n * env;
        wide_in[2 * i + 1] = (0.6f * n + 0.8f * n2) * env;
    }
    click[2 * (SR / 10)] = click[2 * (SR / 10) + 1] = 0.5f;
    click_l[2 * (SR / 10)] = 0.5f;
}

typedef struct { int mode; float wide, freq, time, trim, late, hicut; int comp; } cfg;
#define CFG(m, w) ((cfg){ (m), (w), 20.0f, 0.0f, 0.0f, 0.0f, STEREOID_HICUT_MAX, STEREOID_COMP_OFF })

static void apply(stereoid *s, cfg c) {
    s->p.mode = c.mode; s->p.wide = c.wide; s->p.freq = c.freq; s->p.time = c.time;
    s->p.trim = c.trim; s->p.late = c.late; s->p.hicut = c.hicut; s->p.comp = c.comp;
}

/* `in` through a fresh instance set as `c`, in host-sized blocks. */
static void run(cfg c, const float *in, float *out) {
    static stereoid s;
    stereoid_init(&s);
    apply(&s, c);
    memcpy(out, in, sizeof(float) * 2 * LEN);
    for (int at = 0; at < LEN; at += FR) stereoid_process(&s, out + 2 * at, FR);
}

static double energy(const float *x, int ch) {
    double e = 0;
    for (int i = 0; i < LEN; i++) e += (double) x[2 * i + ch] * x[2 * i + ch];
    return e;
}

/* A steady tone of `hz`, the same on both sides. */
static void tone_in(float *x, double hz, float amp) {
    for (int i = 0; i < LEN; i++) x[2 * i] = x[2 * i + 1] = amp * (float) sin(2 * PI * hz * i / SR);
}

/* RMS of one channel over the second half (past any filter's settling). */
static double rms_tail(const float *x, int ch) {
    double e = 0;
    for (int i = LEN / 2; i < LEN; i++) e += (double) x[2 * i + ch] * x[2 * i + ch];
    return sqrt(e / (LEN / 2));
}

/* The side's response to the centred click: level (dB) at `hz`, its phase,
 * and the frame it starts at. out must be a run() of `click`. */
static void side_at(const float *out, double hz, double *db, double *phase, int *onset) {
    const int t0 = SR / 10;
    const double m0 = 0.5 * (out[2 * t0] + out[2 * t0 + 1]);
    double re = 0, im = 0;
    int on = -1;
    for (int i = t0; i < LEN; i++) {
        const double sd = 0.5 * (out[2 * i] - out[2 * i + 1]) / m0;
        if (on < 0 && fabs(sd) > 1e-6) on = i - t0;
        re += sd * cos(2 * PI * hz * (i - t0) / SR);
        im -= sd * sin(2 * PI * hz * (i - t0) / SR);
    }
    if (db) *db = 10 * log10(re * re + im * im + 1e-30);
    if (phase) *phase = atan2(im, re);
    if (onset) *onset = on;
}

static double group_delay_ms(const float *out, double hz) {
    double p0, p1;
    side_at(out, hz - 5.0, NULL, &p0, NULL);
    side_at(out, hz + 5.0, NULL, &p1, NULL);
    double d = p1 - p0;
    while (d > PI) d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    return -d / (2 * PI * 10.0) * 1000.0;
}

static float a[2 * LEN], b[2 * LEN], t1[2 * LEN];

static void test_bypass(void) {
    for (int m = 0; m < STEREOID_MODES; m++) {
        cfg c = CFG(m, 0.0f);
        c.freq = 900; c.time = 5; c.late = 6; c.hicut = 3000;
        run(c, wide_in, b);
        CHECK(!memcmp(b, wide_in, sizeof b), "mode %d: WIDE 0 changed the signal", m);
    }
}

static void test_comb(void) {
    const int D = (int) (8.0f * 0.001f * SR + 0.5f);      /* 353 */

    /* The mono sum is the input, on a STEREO input, with both corners set. */
    {
        cfg c = CFG(STEREOID_COMB, 70.0f);
        c.freq = 400; c.hicut = 6000;
        run(c, wide_in, b);
        float worst = 0, pk = 0;
        for (int i = 0; i < LEN; i++) {
            const float e = fabsf((b[2 * i] + b[2 * i + 1]) - (wide_in[2 * i] + wide_in[2 * i + 1]));
            if (e > worst) worst = e;
            if (fabsf(wide_in[2 * i]) > pk) pk = fabsf(wide_in[2 * i]);
        }
        CHECK(worst <= 4e-7f * pk, "Comb changed the mono sum by %g (peak %g)", worst, pk);
    }

    /* Full band: the side IS g x the mid, 8 ms late; a negative WIDE mirrors. */
    for (int sgn = 1; sgn >= -1; sgn -= 2) {
        run(CFG(STEREOID_COMB, 50.0f * sgn), snare, b);
        float worst = 0;
        for (int i = 0; i < LEN; i++) {
            const float want = i >= D ? 0.5f * sgn * snare[2 * (i - D)] : 0.0f;
            const float e = fabsf(0.5f * (b[2 * i] - b[2 * i + 1]) - want);
            if (e > worst) worst = e;
        }
        CHECK(worst < 1e-6f, "Comb %+d%%: the side is not %+.1f x the mid 8 ms late (worst %g)", 50 * sgn, 0.5 * sgn, worst);
    }

    /* A stereo input keeps the side it came with: what is ADDED is g x its mid. */
    {
        run(CFG(STEREOID_COMB, 50.0f), wide_in, b);
        float worst = 0;
        for (int i = 0; i < LEN; i++) {
            const float mid = i >= D ? 0.5f * (wide_in[2 * (i - D)] + wide_in[2 * (i - D) + 1]) : 0.0f;
            const float added = 0.5f * (b[2 * i] - b[2 * i + 1]) - 0.5f * (wide_in[2 * i] - wide_in[2 * i + 1]);
            const float e = fabsf(added - 0.5f * mid);
            if (e > worst) worst = e;
        }
        CHECK(worst < 1e-6f, "Comb on a stereo input: the added side is not 0.5 x its mid (worst %g)", worst);
    }

    /* Linear amount: side energy goes as g squared. */
    {
        double side[2];
        for (int t = 0; t < 2; t++) {
            run(CFG(STEREOID_COMB, t ? 100.0f : 50.0f), snare, b);
            double e = 0;
            for (int i = 0; i < LEN; i++) { const double d = b[2 * i] - b[2 * i + 1]; e += d * d; }
            side[t] = e;
        }
        CHECK(fabs(side[1] / side[0] - 4.0) < 1e-3, "Comb 100%% vs 50%%: side energy x%.4f, want x4", side[1] / side[0]);
    }

    /* No lean: the two sides carry the same energy. */
    {
        cfg c = CFG(STEREOID_COMB, 100.0f);
        c.freq = 150;
        run(c, snare, b);
        const double el = energy(b, 0), er = energy(b, 1);
        CHECK(fabs(er / el - 1.0) < 0.1, "Comb leans: R/L energy %.3f on a centred burst", er / el);
    }

    /* TIME is the delay, exactly. */
    {
        cfg c = CFG(STEREOID_COMB, 50.0f);
        c.time = 3;
        run(c, snare, b);
        const int d3 = (int) (3.0f * 0.001f * SR + 0.5f);
        float worst = 0;
        for (int i = 0; i < LEN; i++) {
            const float want = i >= d3 ? 0.5f * snare[2 * (i - d3)] : 0.0f;
            const float e = fabsf(0.5f * (b[2 * i] - b[2 * i + 1]) - want);
            if (e > worst) worst = e;
        }
        CHECK(worst < 1e-6f, "Comb TIME 3 ms: the side is not the mid 3 ms late (worst %g)", worst);
    }

    /* The band: a 60 Hz tone under WFREQ 400 and a 12 kHz tone over HICUT
     * 3000 stay centred; a 1 kHz tone between them widens. */
    {
        const double hz[] = {60.0, 1000.0, 12000.0};
        double ratio[3];
        for (int t = 0; t < 3; t++) {
            cfg c = CFG(STEREOID_COMB, 100.0f);
            c.freq = 400; c.hicut = 3000;
            tone_in(a, hz[t], 0.2f);
            run(c, a, b);
            double sd = 0, m = 0;
            for (int i = LEN / 2; i < LEN; i++) {
                const double l = b[2 * i], r = b[2 * i + 1];
                sd += (l - r) * (l - r); m += (l + r) * (l + r);
            }
            ratio[t] = sd / m;
        }
        CHECK(ratio[1] > 0.5, "Comb barely widens a tone inside the band (side/mid %.3f)", ratio[1]);
        CHECK(ratio[0] < 0.01 * ratio[1], "WFREQ 400 did not keep 60 Hz centred (side/mid %.5f vs %.3f)", ratio[0], ratio[1]);
        CHECK(ratio[2] < 0.01 * ratio[1], "HICUT 3000 did not keep 12 kHz centred (side/mid %.5f vs %.3f)", ratio[2], ratio[1]);
        printf("  comb side/mid: 60 Hz %.5f, 1 kHz %.3f, 12 kHz %.5f\n", ratio[0], ratio[1], ratio[2]);
    }
}

static void test_haas(void) {
    /* Auto: the Haas delay, exactly, on the sign's side. */
    const int pcts[] = {50, 100, -50, -100};
    for (int t = 0; t < 4; t++) {
        run(CFG(STEREOID_HAAS, (float) pcts[t]), wide_in, b);
        const float aa = pcts[t] * 0.01f;
        const int d = (int) (15.0f * aa * aa * 0.001f * SR + 0.5f);
        const int dch = pcts[t] > 0 ? 1 : 0;
        int ok_other = 1, ok_delayed = 1;
        for (int i = 0; i < LEN; i++) {
            if (b[2 * i + 1 - dch] != wide_in[2 * i + 1 - dch]) ok_other = 0;
            if (b[2 * i + dch] != (i >= d ? wide_in[2 * (i - d) + dch] : 0.0f)) ok_delayed = 0;
        }
        CHECK(ok_other, "Haas %d%% changed the %s side", pcts[t], dch ? "left" : "right");
        CHECK(ok_delayed, "Haas %d%% is not the %s side delayed by %d frames", pcts[t], dch ? "right" : "left", d);
    }

    /* TIME set: TIME the delay, |WIDE| the delayed side's mix, the sign the side. */
    {
        const int d5 = (int) (5.0f * 0.001f * SR + 0.5f);
        struct { float wide, m; int dch; } hs[] = {{100, 1.0f, 1}, {50, 0.5f, 1}, {-30, 0.3f, 0}};
        for (int t = 0; t < 3; t++) {
            cfg c = CFG(STEREOID_HAAS, hs[t].wide);
            c.time = 5;
            run(c, snare, b);
            float wo = 0, wd = 0;
            for (int i = 0; i < LEN; i++) {
                const int ch = hs[t].dch;
                const float dry = snare[2 * i + ch], late = i >= d5 ? snare[2 * (i - d5) + ch] : 0.0f;
                const float e = fabsf(b[2 * i + ch] - ((1.0f - hs[t].m) * dry + hs[t].m * late));
                if (e > wd) wd = e;
                const float o = fabsf(b[2 * i + 1 - ch] - snare[2 * i + 1 - ch]);
                if (o > wo) wo = o;
            }
            CHECK(wd < 1e-6f, "Haas TIME 5 WIDE %g: the delayed side is not the %.0f%% mix (worst %g)", hs[t].wide, hs[t].m * 100, wd);
            CHECK(wo == 0.0f, "Haas TIME 5 WIDE %g: the other side changed (worst %g)", hs[t].wide, wo);
        }
    }

    /* LATE +6.02 dB doubles the delayed side; the other does not move. */
    {
        const int d5 = (int) (5.0f * 0.001f * SR + 0.5f);
        cfg c = CFG(STEREOID_HAAS, 100.0f);
        c.time = 5; c.late = 6.0206f;
        run(c, snare, b);
        float wd = 0, wo = 0;
        for (int i = 0; i < LEN; i++) {
            const float late = i >= d5 ? snare[2 * (i - d5) + 1] : 0.0f;
            const float e = fabsf(b[2 * i + 1] - 2.0f * late);
            if (e > wd) wd = e;
            const float o = fabsf(b[2 * i] - snare[2 * i]);
            if (o > wo) wo = o;
        }
        CHECK(wd < 1e-5f, "LATE +6 dB did not double the delayed side (worst %g)", wd);
        CHECK(wo == 0.0f, "LATE moved the other side (worst %g)", wo);
    }

    /* The band. Over HICUT nothing is delayed: a 12 kHz tone comes out the
     * same on both sides. Inside it, a 1 kHz tone's delayed side differs. */
    {
        const double hz[] = {1000.0, 12000.0};
        double diff[2];
        for (int t = 0; t < 2; t++) {
            cfg c = CFG(STEREOID_HAAS, 100.0f);
            c.time = 0.7f; c.hicut = 3000;        /* 0.7 ms: not a whole period of either tone */
            tone_in(a, hz[t], 0.2f);
            run(c, a, b);
            double dd = 0, mm = 0;
            for (int i = LEN / 2; i < LEN; i++) {
                const double d = b[2 * i] - b[2 * i + 1];
                dd += d * d; mm += (double) b[2 * i] * b[2 * i];
            }
            diff[t] = dd / mm;
        }
        CHECK(diff[0] > 0.5, "Haas barely moves a tone inside the band (L-R / L energy %.3f)", diff[0]);
        CHECK(diff[1] < 0.01, "HICUT 3000 did not leave 12 kHz alone (L-R / L energy %.4f)", diff[1]);
    }

    /* Three bands sum FLAT: with the corners close (2 k and 3 k), a tone
     * between them leaves the undelayed side at the level it came in. Without
     * the low band's all-pass it does not. */
    {
        const double hz[] = {500.0, 2000.0, 2500.0, 3000.0, 8000.0};
        for (int t = 0; t < 5; t++) {
            cfg c = CFG(STEREOID_HAAS, 100.0f);
            c.freq = 2000; c.hicut = 3000;
            tone_in(a, hz[t], 0.2f);
            run(c, a, b);
            const double r = rms_tail(b, 0) / rms_tail(a, 0);
            CHECK(fabs(r - 1.0) < 0.01, "Haas's three bands are not flat at %g Hz: the undelayed side is x%.4f", hz[t], r);
        }
    }
}

static void test_disperse(void) {
    /* Fitted to impulse renders of Wider itself:
     *   L += F(L), R -= F(R); F = g * AP5(delay(HP x)); g = min(W/100, 1);
     *   delay = 0.03 ms * W; W = 2 * |WIDE|. */
    struct { float wide; double want_db; int want_lag; } law[] = {
        {12.5f, -12.04, 33}, {25, -6.02, 66}, {50, 0.0, 132}, {100, 0.0, 265},
    };
    const int t0 = SR / 10;
    for (int t = 0; t < 4; t++) {
        run(CFG(STEREOID_DISPERSE, law[t].wide), click, b);
        const double m0 = 0.5 * (b[2 * t0] + b[2 * t0 + 1]);
        double worst = 0;
        for (int i = 0; i < LEN; i++) if (i != t0) {
            const double mm = 0.5 * (b[2 * i] + b[2 * i + 1]);
            if (fabs(mm) > worst) worst = fabs(mm);
        }
        CHECK(fabs(m0 - 0.5) < 1e-6 && worst < 1e-6 * fabs(m0), "Disperse %g: the mid is not the dry click (stray %g)", law[t].wide, worst);
        double db;
        int onset;
        side_at(b, 1000.0, &db, NULL, &onset);       /* the all-passes are flat: this is the gain law */
        CHECK(fabs(db - law[t].want_db) < 0.1, "Disperse WIDE %g: side at 1 kHz %.2f dB, want %.2f", law[t].wide, db, law[t].want_db);
        /* the ALL-PASSES, which a level check cannot see: the side's group
         * delay at 200 Hz is the width delay plus Wider's measured 1.41 ms */
        const double gd = group_delay_ms(b, 200.0), want = law[t].want_lag * 1000.0 / SR + 1.41;
        CHECK(fabs(gd - want) < 0.12, "Disperse WIDE %g: side group delay at 200 Hz %.2f ms, want %.2f", law[t].wide, gd, want);
        /* the delay: 0.03 ms per Wider-% (the cubic's first tap sits a frame ahead) */
        CHECK(abs(onset - law[t].want_lag) <= 2, "Disperse WIDE %g: side starts at %d frames, want ~%d", law[t].wide, onset, law[t].want_lag);
    }

    /* Per CHANNEL, as Wider is: a LEFT-only input leaves the right silent,
     * where a mid/side widener would not. */
    {
        run(CFG(STEREOID_DISPERSE, 50.0f), click_l, b);
        double rmax = 0, lsum = 0;
        for (int i = 0; i < LEN; i++) { if (fabsf(b[2 * i + 1]) > rmax) rmax = fabsf(b[2 * i + 1]); lsum += fabs(b[2 * i]); }
        CHECK(lsum > 0.5 && rmax == 0.0, "Disperse is not per-channel: a left-only click put %g on the right", rmax);
        run(CFG(STEREOID_COMB, 50.0f), click_l, b);
        rmax = 0;
        for (int i = 0; i < LEN; i++) if (fabsf(b[2 * i + 1]) > rmax) rmax = fabsf(b[2 * i + 1]);
        CHECK(rmax > 0.1, "Comb does not widen from the mid: a left-only click left the right at %g", rmax);
    }

    /* A mono input's sum is exact. */
    {
        cfg c = CFG(STEREOID_DISPERSE, 80.0f);
        c.freq = 200;
        run(c, snare, b);
        float worst = 0;
        for (int i = 0; i < LEN; i++) {
            const float e = fabsf((b[2 * i] + b[2 * i + 1]) - 2.0f * snare[2 * i]);
            if (e > worst) worst = e;
        }
        CHECK(worst < 1e-6f, "Disperse changed a mono input's sum by %g", worst);
    }

    /* TIME fixes the delay whatever WIDE is. */
    {
        int on[2];
        for (int t = 0; t < 2; t++) {
            cfg c = CFG(STEREOID_DISPERSE, t ? 100.0f : 25.0f);
            c.time = 5;
            run(c, click, b);
            side_at(b, 1000.0, NULL, NULL, &on[t]);
        }
        CHECK(on[0] == on[1] && abs(on[0] - 220) <= 3, "Disperse TIME 5 ms: side starts at %d / %d frames, want both ~220", on[0], on[1]);
    }

    /* WFREQ and HICUT are LR4 on F's input: -6 dB at the corner, gone a
     * decade past it, and the mid is still the dry click. */
    {
        cfg c = CFG(STEREOID_DISPERSE, 50.0f);
        c.freq = 200; c.hicut = 2000;
        run(c, click, b);
        double at_lo, at_hi, low, high, mid;
        side_at(b, 200.0, &at_lo, NULL, NULL);
        side_at(b, 2000.0, &at_hi, NULL, NULL);
        side_at(b, 30.0, &low, NULL, NULL);
        side_at(b, 15000.0, &high, NULL, NULL);
        side_at(b, 632.0, &mid, NULL, NULL);
        CHECK(fabs(at_lo + 6.02) < 0.3, "WFREQ 200: the side at 200 Hz is %.2f dB, want -6", at_lo);
        CHECK(fabs(at_hi + 6.02) < 0.3, "HICUT 2000: the side at 2 kHz is %.2f dB, want -6", at_hi);
        CHECK(low < -40 && high < -40, "the band's skirts: 30 Hz %.1f dB, 15 kHz %.1f dB, want both under -40", low, high);
        CHECK(mid > -1.0, "the band's middle (632 Hz) is %.2f dB, want within 1 dB of full", mid);
        double worst = 0;
        for (int i = 0; i < LEN; i++) if (i != t0) {
            const double mm = 0.5 * (b[2 * i] + b[2 * i + 1]);
            if (fabs(mm) > worst) worst = fabs(mm);
        }
        CHECK(worst < 1e-6, "with both corners set the mid is not the dry click (stray %g)", worst);
    }
}

static void test_ms(void) {
    /* Full band: the mid is untouched and the side is k x the input's,
     * k = 1 + WIDE/100. */
    const float wides[] = {100, 50, -50, -100};
    for (int t = 0; t < 4; t++) {
        run(CFG(STEREOID_MS, wides[t]), wide_in, b);
        const float k = 1.0f + wides[t] * 0.01f;
        float wm = 0, ws = 0;
        for (int i = 0; i < LEN; i++) {
            const float mi = 0.5f * (wide_in[2 * i] + wide_in[2 * i + 1]), si = 0.5f * (wide_in[2 * i] - wide_in[2 * i + 1]);
            const float mo = 0.5f * (b[2 * i] + b[2 * i + 1]), so = 0.5f * (b[2 * i] - b[2 * i + 1]);
            if (fabsf(mo - mi) > wm) wm = fabsf(mo - mi);
            if (fabsf(so - k * si) > ws) ws = fabsf(so - k * si);
        }
        CHECK(wm < 1e-7f, "M/S %g: the mid moved by %g", wides[t], wm);
        CHECK(ws < 1e-6f, "M/S %g: the side is not x%.2f (worst %g)", wides[t], k, ws);
    }
    /* -100 folds to mono, exactly; a mono input is left alone. */
    run(CFG(STEREOID_MS, -100), wide_in, b);
    int mono = 1;
    for (int i = 0; i < LEN; i++) if (b[2 * i] != b[2 * i + 1]) { mono = 0; break; }
    CHECK(mono, "M/S -100 did not fold to mono");
    run(CFG(STEREOID_MS, 100), snare, b);
    CHECK(!memcmp(b, snare, sizeof b), "M/S changed a mono input");

    /* The band: with WFREQ 400 and HICUT 3000 a stereo 60 Hz or 12 kHz keeps
     * its side; a 1 kHz one's doubles. The mono sum stays exact. */
    {
        const double hz[] = {60.0, 1000.0, 12000.0};
        double ratio[3];
        for (int t = 0; t < 3; t++) {
            for (int i = 0; i < LEN; i++) {
                const float v = 0.2f * (float) sin(2 * PI * hz[t] * i / SR);
                a[2 * i] = v; a[2 * i + 1] = 0.5f * v;          /* left-heavy: side = v/4 */
            }
            cfg c = CFG(STEREOID_MS, 100);
            c.freq = 400; c.hicut = 3000;
            run(c, a, b);
            double si = 0, so = 0;
            float wsum = 0;
            for (int i = LEN / 2; i < LEN; i++) {
                const double di = a[2 * i] - a[2 * i + 1], d = b[2 * i] - b[2 * i + 1];
                si += di * di; so += d * d;
                const float e = fabsf((b[2 * i] + b[2 * i + 1]) - (a[2 * i] + a[2 * i + 1]));
                if (e > wsum) wsum = e;
            }
            ratio[t] = sqrt(so / si);
            CHECK(wsum < 1e-6f, "M/S with both corners changed the mono sum at %g Hz (worst %g)", hz[t], wsum);
        }
        CHECK(fabs(ratio[1] - 2.0) < 0.1, "M/S +100 inside the band: side x%.3f, want x2", ratio[1]);
        CHECK(fabs(ratio[0] - 1.0) < 0.02 && fabs(ratio[2] - 1.0) < 0.02,
              "M/S moved the side outside the band: 60 Hz x%.3f, 12 kHz x%.3f, want x1", ratio[0], ratio[2]);
    }

    /* COMP follows k as Haas's follows LATE: Peak 1/k, Loud sqrt(2/(1+k^2)),
     * never a boost. A hard-left tone at +100 with Peak stays under its peak. */
    {
        cfg c = CFG(STEREOID_MS, 100);
        c.comp = STEREOID_COMP_PEAK;
        for (int i = 0; i < LEN; i++) { a[2 * i] = 0.25f * (float) sin(2 * PI * 1000.0 * i / SR); a[2 * i + 1] = 0; }
        run(c, a, b);
        float pk = 0;
        for (int i = 0; i < 2 * LEN; i++) if (fabsf(b[i]) > pk) pk = fabsf(b[i]);
        CHECK(pk <= 0.2501f, "M/S +100 with COMP Peak let a hard-left tone reach %.4f (in 0.25)", pk);
        float worst = 0;
        run(c, wide_in, b);
        for (int i = 0; i < LEN; i++) {
            const float e = fabsf((b[2 * i] + b[2 * i + 1]) - 0.5f * (wide_in[2 * i] + wide_in[2 * i + 1]));
            if (e > worst) worst = e;
        }
        CHECK(worst < 1e-6f, "M/S +100 COMP Peak: the mono sum is not x0.5 (worst %g)", worst);
        c.comp = STEREOID_COMP_LOUD;
        run(c, wide_in, b);
        worst = 0;
        for (int i = 0; i < LEN; i++) {
            const float e = fabsf((b[2 * i] + b[2 * i + 1]) - 0.63245553f * (wide_in[2 * i] + wide_in[2 * i + 1]));
            if (e > worst) worst = e;
        }
        CHECK(worst < 1e-6f, "M/S +100 COMP Loud: the mono sum is not x0.632 (worst %g)", worst);
        c = CFG(STEREOID_MS, -60);
        run(c, wide_in, a);
        c.comp = STEREOID_COMP_PEAK;
        run(c, wide_in, b);
        CHECK(!memcmp(a, b, sizeof a), "COMP boosted a narrowed M/S");
    }
}

static void test_trim(void) {
    cfg c = CFG(STEREOID_COMB, 100.0f);
    c.trim = -6.0206f;
    run(c, wide_in, b);
    float worst = 0;
    for (int i = 0; i < LEN; i++) {
        const float e = fabsf((b[2 * i] + b[2 * i + 1]) - 0.5f * (wide_in[2 * i] + wide_in[2 * i + 1]));
        if (e > worst) worst = e;
    }
    CHECK(worst < 1e-6f, "TRIM -6 dB: the mono sum is not half the input (worst %g)", worst);

    /* With the widener off it is still the output level. */
    c = CFG(STEREOID_HAAS, 0.0f);
    c.trim = -6.0206f;
    run(c, wide_in, b);
    worst = 0;
    for (int i = 0; i < 2 * LEN; i++) {
        const float e = fabsf(b[i] - 0.5f * wide_in[i]);
        if (e > worst) worst = e;
    }
    CHECK(worst < 1e-6f, "TRIM -6 dB at WIDE 0 is not half the input (worst %g)", worst);
}

/* COMP: Off | Loud | Peak. */
static void test_comp(void) {
    /* The mono sum is the input x the trim, exactly: Loud 1/sqrt(1+g^2),
     * Peak 1/(1+g), with g Comb's |WIDE|/100 and Disperse's min(2|WIDE|/100, 1). */
    struct { int mode; float wide; int comp; float want; } law[] = {
        {STEREOID_COMB, 100, STEREOID_COMP_LOUD, 0.70710678f}, {STEREOID_COMB, 100, STEREOID_COMP_PEAK, 0.5f},
        {STEREOID_COMB, -50, STEREOID_COMP_LOUD, 0.89442719f}, {STEREOID_COMB, -50, STEREOID_COMP_PEAK, 0.66666667f},
        {STEREOID_DISPERSE, 50, STEREOID_COMP_LOUD, 0.70710678f}, {STEREOID_DISPERSE, 100, STEREOID_COMP_PEAK, 0.5f},
        {STEREOID_DISPERSE, 25, STEREOID_COMP_PEAK, 0.66666667f},
    };
    for (int t = 0; t < 7; t++) {
        cfg c = CFG(law[t].mode, law[t].wide);
        c.comp = law[t].comp;
        run(c, snare, b);
        float worst = 0;
        for (int i = 0; i < LEN; i++) {
            const float e = fabsf((b[2 * i] + b[2 * i + 1]) - 2.0f * law[t].want * snare[2 * i]);
            if (e > worst) worst = e;
        }
        CHECK(worst < 1e-6f, "COMP %d, mode %d WIDE %g: the mono sum is not the input x %.4f (worst %g)",
              law[t].comp, law[t].mode, law[t].wide, law[t].want, worst);
    }

    /* What each one is FOR, on a full-width Comb. Loud: an ear's energy is the
     * dry one's. Peak: no ear exceeds the dry peak, where Off reaches twice it
     * (a 1 kHz tone, TIME 1 ms: the copy lands in phase on the left). */
    {
        cfg c = CFG(STEREOID_COMB, 100);
        c.comp = STEREOID_COMP_LOUD;
        run(c, snare, b);
        const double r = energy(b, 0) / energy(snare, 0);
        CHECK(fabs(r - 1.0) < 0.05, "COMP Loud: a full-width burst's left ear is %.3f of the dry, want ~1", r);
        float pk[3];
        for (int k = 0; k < 3; k++) {
            c = CFG(STEREOID_COMB, 100);
            c.time = 1; c.comp = k;
            tone_in(a, 1000.0, 0.25f);
            run(c, a, b);
            pk[k] = 0;
            for (int i = LEN / 2; i < LEN; i++) if (fabsf(b[2 * i]) > pk[k]) pk[k] = fabsf(b[2 * i]);
        }
        CHECK(pk[0] > 0.49f, "the worst case is not the worst case: Off peaks at %.3f, want ~0.5", pk[0]);
        CHECK(pk[2] <= 0.2501f, "COMP Peak let an ear reach %.4f (the dry peak is 0.25)", pk[2]);
        CHECK(pk[1] > pk[2] && pk[1] < pk[0], "COMP Loud's peak %.3f is not between Peak's %.3f and Off's %.3f", pk[1], pk[2], pk[0]);
        printf("  comb at full width, worst-case tone: peak Off %.3f, Loud %.3f, Peak %.3f (dry 0.250)\n", pk[0], pk[1], pk[2]);
    }

    /* Haas adds nothing: COMP leaves it alone... */
    for (int k = 1; k < 3; k++) {
        cfg c = CFG(STEREOID_HAAS, 60);
        run(c, snare, a);
        c.comp = k;
        run(c, snare, b);
        CHECK(!memcmp(a, b, sizeof a), "COMP %d changed a Haas with LATE at 0", k);
        c.late = -6;
        c.comp = 0; run(c, snare, a);
        c.comp = k; run(c, snare, b);
        CHECK(!memcmp(a, b, sizeof a), "COMP %d boosted a Haas with LATE turned down", k);
    }
    /* ...until LATE raises the delayed side. +6.02 dB, Peak: everything is
     * halved, so the delayed side is back at the dry level. */
    {
        const int d5 = (int) (5.0f * 0.001f * SR + 0.5f);
        cfg c = CFG(STEREOID_HAAS, 100);
        c.time = 5; c.late = 6.0206f; c.comp = STEREOID_COMP_PEAK;
        run(c, snare, b);
        float wd = 0, wo = 0;
        for (int i = 0; i < LEN; i++) {
            const float late = i >= d5 ? snare[2 * (i - d5) + 1] : 0.0f;
            if (fabsf(b[2 * i + 1] - late) > wd) wd = fabsf(b[2 * i + 1] - late);
            if (fabsf(b[2 * i] - 0.5f * snare[2 * i]) > wo) wo = fabsf(b[2 * i] - 0.5f * snare[2 * i]);
        }
        CHECK(wd < 1e-5f && wo < 1e-5f, "Haas LATE +6 with COMP Peak: delayed side off by %g, the other by %g", wd, wo);
        c.comp = STEREOID_COMP_LOUD;        /* sqrt(2 / (1 + 4)) */
        run(c, snare, b);
        wo = 0;
        for (int i = 0; i < LEN; i++)
            if (fabsf(b[2 * i] - 0.63245553f * snare[2 * i]) > wo) wo = fabsf(b[2 * i] - 0.63245553f * snare[2 * i]);
        CHECK(wo < 1e-5f, "Haas LATE +6 with COMP Loud: the undelayed side is not x0.632 (worst %g)", wo);
    }

    /* At WIDE 0 there is nothing to compensate — Haas with LATE up is the
     * case that would otherwise still be trimmed. */
    {
        cfg c = CFG(STEREOID_HAAS, 0);
        c.late = 6; c.comp = STEREOID_COMP_PEAK;
        run(c, wide_in, b);
        CHECK(!memcmp(b, wide_in, sizeof b), "COMP changed the signal at WIDE 0");
    }
}

/* A knob moved under a steady tone: no step in the output, and once the
 * approach is over the law holds exactly. */
static void test_moves(void) {
    static stereoid s;
    const float amp = 0.25f;
    const double hz = 1000.0;
    /* the largest frame-to-frame step the tone itself can make, widened */
    const float natural = (float) (2 * PI * hz / SR) * amp * 2.0f;

    struct { const char *what; cfg from, to; } mv[6];
    mv[0].what = "Comb TIME 2 -> 10";  mv[0].from = CFG(STEREOID_COMB, 100); mv[0].from.time = 2;  mv[0].to = mv[0].from; mv[0].to.time = 10;
    mv[1].what = "Comb WIDE 100 -> 10"; mv[1].from = CFG(STEREOID_COMB, 100); mv[1].to = CFG(STEREOID_COMB, 10);
    mv[2].what = "Haas WIDE 20 -> 100"; mv[2].from = CFG(STEREOID_HAAS, 20);  mv[2].to = CFG(STEREOID_HAAS, 100);
    mv[3].what = "TRIM 0 -> -12";       mv[3].from = CFG(STEREOID_COMB, 50);  mv[3].to = mv[3].from; mv[3].to.trim = -12;
    mv[4].what = "COMP Off -> Peak";    mv[4].from = CFG(STEREOID_COMB, 100); mv[4].to = mv[4].from; mv[4].to.comp = STEREOID_COMP_PEAK;

    mv[5].what = "M/S WIDE -100 -> 100"; mv[5].from = CFG(STEREOID_MS, -100); mv[5].to = CFG(STEREOID_MS, 100);

    for (int t = 0; t < 6; t++) {
        tone_in(a, hz, amp);
        if (t == 5) for (int i = 0; i < LEN; i++) a[2 * i + 1] *= 0.3f;     /* M/S needs a side to move */
        stereoid_init(&s);
        apply(&s, mv[t].from);
        memcpy(b, a, sizeof b);
        const int half = (LEN / 2 / FR) * FR;
        for (int at = 0; at < LEN; at += FR) {
            if (at == half) apply(&s, mv[t].to);
            stereoid_process(&s, b + 2 * at, FR);
        }
        float step = 0;
        for (int i = 1; i < LEN; i++)
            for (int ch = 0; ch < 2; ch++) {
                const float d = fabsf(b[2 * i + ch] - b[2 * (i - 1) + ch]);
                if (d > step) step = d;
            }
        CHECK(step < 1.5f * natural, "%s: the output steps by %.4f (the tone's own largest step is %.4f)", mv[t].what, step, natural);
        /* ...and it LANDS: the tail equals a fresh instance set to `to`. */
        run(mv[t].to, a, t1);
        float worst = 0;
        for (int i = LEN - 2 * FR; i < LEN; i++)
            for (int ch = 0; ch < 2; ch++) {
                const float e = fabsf(b[2 * i + ch] - t1[2 * i + ch]);
                if (e > worst) worst = e;
            }
        CHECK(worst < 1e-6f, "%s: after the approach the output is not the settled one (worst %g)", mv[t].what, worst);
    }

    /* An engine change starts clean: no stale ring is replayed. */
    stereoid_init(&s);
    apply(&s, CFG(STEREOID_COMB, 100));
    memcpy(b, snare, sizeof b);
    for (int at = 0; at < LEN; at += FR) stereoid_process(&s, b + 2 * at, FR);
    apply(&s, CFG(STEREOID_DISPERSE, 100));
    memset(b, 0, sizeof b);
    for (int at = 0; at < LEN; at += FR) stereoid_process(&s, b + 2 * at, FR);
    float pk = 0;
    for (int i = 0; i < 2 * LEN; i++) if (fabsf(b[i]) > pk) pk = fabsf(b[i]);
    CHECK(pk == 0.0f, "an engine change replayed old audio into silence (peak %g)", pk);
}

/* ---- the host contract ---------------------------------------------------- */

static audio_fx_api_v2_t *api;
static void *inst;
static void set(const char *k, const char *v) { api->set_param(inst, k, v); }
static const char *get(const char *k) {
    static char buf[512];
    buf[0] = 0;
    if (api->get_param(inst, k, buf, sizeof buf) < 0) return "<none>";
    return buf;
}

static void test_module(void) {
    api = move_audio_fx_init_v2(NULL);
    CHECK(api && api->api_version == AUDIO_FX_API_VERSION_2, "no v2 api");
    inst = api->create_instance("", NULL);
    CHECK(inst != NULL, "create_instance failed");
    if (!inst) return;

    /* Defaults are module.json's. */
    const char *want[][2] = {{"mode", "Comb"}, {"wide", "50"}, {"freq", "150"}, {"time", "0"}, {"trim", "0"},
                             {"hicut", "20000"}, {"late", "0"}, {"comp", "Off"}};
    for (int i = 0; i < 8; i++)
        CHECK(!strcmp(get(want[i][0]), want[i][1]), "default %s reads %s, want %s", want[i][0], get(want[i][0]), want[i][1]);

    /* Clamp and read back. */
    set("wide", "150");   CHECK(!strcmp(get("wide"), "100"), "wide clamps to 100: %s", get("wide"));
    set("wide", "-150");  CHECK(!strcmp(get("wide"), "-100"), "wide clamps to -100: %s", get("wide"));
    set("freq", "5");     CHECK(!strcmp(get("freq"), "20"), "freq clamps to 20: %s", get("freq"));
    set("freq", "9000");  CHECK(!strcmp(get("freq"), "4000"), "freq clamps to 4000: %s", get("freq"));
    set("hicut", "10");   CHECK(!strcmp(get("hicut"), "1000"), "hicut clamps to 1000: %s", get("hicut"));
    set("time", "40");    CHECK(!strcmp(get("time"), "12"), "time clamps to 12: %s", get("time"));
    set("trim", "6");     CHECK(!strcmp(get("trim"), "0"), "trim clamps to 0: %s", get("trim"));
    set("trim", "-40");   CHECK(!strcmp(get("trim"), "-12"), "trim clamps to -12: %s", get("trim"));
    set("late", "99");    CHECK(!strcmp(get("late"), "12"), "late clamps to 12: %s", get("late"));
    CHECK(!strcmp(get("tone"), "<none>"), "the removed TONE knob still answers %s", get("tone"));
    set("wide", "junk");  CHECK(!strcmp(get("wide"), "-100"), "a non-number moved wide: %s", get("wide"));

    /* The enum: names both ways, an index accepted, anything else ignored. */
    set("mode", "Disperse"); CHECK(!strcmp(get("mode"), "Disperse"), "mode reads %s", get("mode"));
    set("mode", "M/S");      CHECK(!strcmp(get("mode"), "M/S"), "mode reads %s", get("mode"));
    set("mode", "3");        CHECK(!strcmp(get("mode"), "M/S"), "mode index 3 reads %s", get("mode"));
    set("mode", "1");        CHECK(!strcmp(get("mode"), "Haas"), "mode by index reads %s", get("mode"));
    set("mode", "Wider");    CHECK(!strcmp(get("mode"), "Haas"), "an unknown name moved mode: %s", get("mode"));
    set("comp", "Peak");     CHECK(!strcmp(get("comp"), "Peak"), "comp reads %s", get("comp"));
    set("comp", "On");       CHECK(!strcmp(get("comp"), "Peak"), "an unknown name moved comp: %s", get("comp"));
    set("mode", "7");        CHECK(!strcmp(get("mode"), "Haas"), "an out-of-range index moved mode: %s", get("mode"));
    CHECK(!strcmp(get("nonesuch"), "<none>"), "an unknown key answered %s", get("nonesuch"));

    /* State round trip. */
    set("wide", "-37"); set("freq", "220"); set("time", "4.5"); set("trim", "-3.5");
    set("hicut", "8000"); set("late", "-4");
    static char blob[512];
    snprintf(blob, sizeof blob, "%s", get("state"));
    void *other = api->create_instance("", NULL);
    void *mine = inst;
    inst = other;
    set("state", blob);
    const char *rt[][2] = {{"mode", "Haas"}, {"wide", "-37"}, {"freq", "220"}, {"time", "4.5"}, {"trim", "-3.5"},
                           {"hicut", "8000"}, {"late", "-4"}, {"comp", "Peak"}};
    for (int i = 0; i < 8; i++)
        CHECK(!strcmp(get(rt[i][0]), rt[i][1]), "state round trip: %s reads %s, want %s (blob %s)", rt[i][0], get(rt[i][0]), rt[i][1], blob);
    api->destroy_instance(other);
    inst = mine;

    /* Audio. Neutral leaves the int16 alone. */
    static int16_t in[2 * FR], out[2 * FR];
    uint32_t r = 99u;
    for (int i = 0; i < 2 * FR; i++) { r = r * 1664525u + 1013904223u; in[i] = (int16_t) (r >> 16); }
    in[0] = 32767; in[1] = -32768;
    set("wide", "0"); set("trim", "0");      /* COMP is still Peak: at WIDE 0 it is nothing */
    memcpy(out, in, sizeof in);
    api->process_block(inst, out, FR);
    CHECK(!memcmp(in, out, sizeof in), "a neutral instance changed the audio");

    /* A sample the widener does not move comes back as the integer it was:
     * Haas delays the right, so the left is untouched, full scale included. */
    set("mode", "Haas"); set("wide", "100"); set("freq", "20"); set("hicut", "20000"); set("late", "0"); set("time", "0");
    memcpy(out, in, sizeof in);
    api->process_block(inst, out, FR);
    int same = 1, moved = 0;
    for (int i = 0; i < FR; i++) {
        if (out[2 * i] != in[2 * i]) same = 0;
        if (out[2 * i + 1] != in[2 * i + 1]) moved = 1;
    }
    CHECK(same, "the int16 round trip changed a sample the widener did not touch");
    CHECK(moved, "Haas at WIDE 100 left the right side where it was");

    /* TRIM alone halves it, to the bit. */
    set("wide", "0"); set("trim", "-6.0206");
    api->process_block(inst, out, FR);      /* one block for the level to arrive */
    memcpy(out, in, sizeof in);
    api->process_block(inst, out, FR);
    int worst = 0;
    for (int i = 0; i < 2 * FR; i++) {
        const int e = abs(out[i] - (int) lrint(in[i] * 0.5));
        if (e > worst) worst = e;
    }
    CHECK(worst <= 1, "TRIM -6 dB is off by %d LSB", worst);

    /* Past full scale it clips rather than wraps. */
    set("mode", "Comb"); set("wide", "100"); set("trim", "0"); set("time", "1"); set("comp", "Off");
    for (int i = 0; i < 2 * FR; i++) in[i] = 30000;
    int wrapped = 0;
    for (int blk = 0; blk < 4; blk++) {
        memcpy(out, in, sizeof in);
        api->process_block(inst, out, FR);
        if (blk == 0) continue;                 /* TRIM is still arriving from -6 dB */
        for (int i = 0; i < FR; i++) if (out[2 * i] < 30000) wrapped = 1;
    }
    CHECK(!wrapped && out[2 * (FR - 1)] == 32767, "a sample past full scale wrapped (last left %d)", out[2 * (FR - 1)]);

    api->destroy_instance(inst);
}

int main(void) {
    printf("stereoid\n");
    make_signals();
    test_bypass();
    test_comb();
    test_haas();
    test_disperse();
    test_ms();
    test_trim();
    test_comp();
    test_moves();
    test_module();
    printf("%s (%d checks, %d failures)\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
