/* stereoid.h — a four-engine stereo widener, three of them lifted from DR32's Stereo page.
 *
 * The DSP only: interleaved stereo float in place, 44.1 kHz, no host types.
 * The laws and where they came from are at each engine in stereoid.c.
 *
 * MIT licensed (see LICENSE).
 */
#ifndef STEREOID_H
#define STEREOID_H

#define STEREOID_SR        44100.0f
#define STEREOID_RING      2048       /* frames per channel; the longest delay, Haas's 15 ms, is 662 */
#define STEREOID_AP_STAGES 5

enum { STEREOID_COMB = 0, STEREOID_HAAS, STEREOID_DISPERSE, STEREOID_MS, STEREOID_MODES };
enum { STEREOID_COMP_OFF = 0, STEREOID_COMP_LOUD, STEREOID_COMP_PEAK, STEREOID_COMPS };

#define STEREOID_FREQ_MIN   20.0f     /* WFREQ at its floor = full band, no filter run */
#define STEREOID_FREQ_MAX   4000.0f
#define STEREOID_HICUT_MIN  1000.0f
#define STEREOID_HICUT_MAX  20000.0f  /* HICUT at its ceiling = off, no filter run */
#define STEREOID_TIME_MAX   12.0f
#define STEREOID_TRIM_MIN   (-12.0f)
#define STEREOID_LATE_MAX   12.0f

typedef struct {
    int   mode;     /* STEREOID_COMB | HAAS | DISPERSE | MS */
    float wide;     /* -100..100 %; 0 is a true bypass of the widener; the sign mirrors */
    float freq;     /* WFREQ, Hz: nothing below it is widened */
    float time;     /* ms; 0 = Auto (each engine's own delay) */
    float trim;     /* output level, dB, -12..0 */
    int   comp;     /* STEREOID_COMP_OFF | LOUD | PEAK: a trim that follows the width */
    float late;     /* Haas: the delayed side's level, dB */
    float hicut;    /* Hz: nothing above it is widened */
} stereoid_params;

typedef struct { float a1, a2, a3, k, hz; } stereoid_svf;

typedef struct {
    stereoid_params p;

    int   live;                 /* the widener ran last block: its state is current */
    int   mode;                 /* the engine that state belongs to */
    int   haas_side;            /* the channel Haas is delaying */
    int   w;                    /* ring write index */
    float ring[2][STEREOID_RING];

    stereoid_svf lo, hi;        /* the WFREQ and HICUT corners */
    float st[2][9][2];          /* per channel: the SVF states an engine uses */
    float ap[2][STEREOID_AP_STAGES];

    /* What a knob turn moves is approached, not jumped to: an effect sits on
     * sustained audio, where a stepped gain or delay is a click. */
    float g, mix, late_g, delay, out_g;
    int   out_set;
} stereoid;

void  stereoid_init(stereoid *s);
void  stereoid_defaults(stereoid_params *p);
/* Nothing to do: WIDE 0 and TRIM 0 dB. The caller may skip the block. */
int   stereoid_neutral(const stereoid *s);
/* Drop the widener's state (the caller skipped a block, so it is stale). */
void  stereoid_flush(stereoid *s);
void  stereoid_process(stereoid *s, float *x, int frames);

#endif
