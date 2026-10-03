/* stereoid_module.c — Stereoid, a Schwung audio_fx module.
 *
 * This file is only the host contract: audio_fx v2, stereo interleaved int16
 * in place at 44100 Hz, stringly set_param/get_param, and a state blob. The
 * widener itself is dsp/stereoid.c.
 *
 * Every entry point here runs on the audio callback: no allocation past
 * create_instance, no file I/O, no logging.
 *
 * MIT licensed (see LICENSE).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shared/audio_fx_api_v2.h"
#include "../dsp/stereoid.h"

#define ST_MAX_BLOCK 512

typedef struct {
    stereoid s;
    float buf[ST_MAX_BLOCK * 2];
} st_t;

static const char *const kModes[STEREOID_MODES] = { "Comb", "Haas", "Disperse", "M/S" };
static const char *const kComps[STEREOID_COMPS] = { "Off", "Loud", "Peak" };

/* The ranges module.json declares; `mode` and `comp` are enums, and speak NAMES
 * both ways (the host learns an enum's wire format from what get_param says). */
typedef struct { const char *key; float lo, hi; } st_field;
static const st_field kFields[] = {
    { "mode",  0.0f,                (float)(STEREOID_MODES - 1) },
    { "wide",  -100.0f,             100.0f },
    { "freq",  STEREOID_FREQ_MIN,   STEREOID_FREQ_MAX },
    { "time",  0.0f,                STEREOID_TIME_MAX },
    { "trim",  STEREOID_TRIM_MIN,   0.0f },
    { "comp",  0.0f,                (float)(STEREOID_COMPS - 1) },
    { "hicut", STEREOID_HICUT_MIN,  STEREOID_HICUT_MAX },
    { "late",  -STEREOID_LATE_MAX,  STEREOID_LATE_MAX },
    { NULL, 0.0f, 0.0f }
};

static float *slot(stereoid_params *p, const char *key) {
    if (!strcmp(key, "wide"))  return &p->wide;
    if (!strcmp(key, "freq"))  return &p->freq;
    if (!strcmp(key, "time"))  return &p->time;
    if (!strcmp(key, "trim"))  return &p->trim;
    if (!strcmp(key, "hicut")) return &p->hicut;
    if (!strcmp(key, "late"))  return &p->late;
    return NULL;
}

static void st_set(st_t *I, const char *key, const char *val) {
    stereoid_params *p = &I->s.p;
    const char *const *names = NULL;
    int *e = NULL, count = 0;
    if (!strcmp(key, "mode")) { names = kModes; e = &p->mode; count = STEREOID_MODES; }
    else if (!strcmp(key, "comp")) { names = kComps; e = &p->comp; count = STEREOID_COMPS; }
    if (e) {
        for (int m = 0; m < count; m++)
            if (!strcmp(val, names[m])) { *e = m; return; }
        char *end;
        const long m = strtol(val, &end, 10);       /* an index, from a host that sends one */
        if (end != val && m >= 0 && m < count) *e = (int)m;
        return;
    }
    float *f = slot(p, key);
    if (!f) return;
    char *end;
    const float v = strtof(val, &end);
    if (end == val || v != v) return;               /* not a number: leave the knob where it is */
    for (const st_field *d = kFields; d->key; d++)
        if (!strcmp(d->key, key)) { *f = v < d->lo ? d->lo : (v > d->hi ? d->hi : v); break; }
}

static int st_get(st_t *I, const char *key, char *buf, int n) {
    stereoid_params *p = &I->s.p;
    int w;
    if (!strcmp(key, "mode")) {
        w = snprintf(buf, (size_t)n, "%s", kModes[p->mode]);
    } else if (!strcmp(key, "comp")) {
        w = snprintf(buf, (size_t)n, "%s", kComps[p->comp]);
    } else {
        const float *f = slot(p, key);
        if (!f) return -1;
        w = snprintf(buf, (size_t)n, "%g", (double)*f);
    }
    return (w > 0 && w < n) ? w : -1;
}

static void *st_create(const char *dir, const char *cfg) {
    (void)dir; (void)cfg;
    st_t *I = (st_t *)malloc(sizeof(st_t));
    if (!I) return NULL;
    stereoid_init(&I->s);
    return I;
}

static void st_destroy(void *inst) { free(inst); }

static void st_process(void *inst, int16_t *audio, int frames) {
    st_t *I = (st_t *)inst;
    if (!I || frames <= 0) return;
    /* Nothing to do: leave the int16 alone rather than round-trip it. */
    if (stereoid_neutral(&I->s)) { stereoid_flush(&I->s); return; }
    for (int off = 0; off < frames; off += ST_MAX_BLOCK) {
        const int n = (frames - off > ST_MAX_BLOCK) ? ST_MAX_BLOCK : (frames - off);
        int16_t *blk = audio + off * 2;
        for (int i = 0; i < n * 2; i++) I->buf[i] = blk[i] / 32768.0f;
        stereoid_process(&I->s, I->buf, n);
        /* The same scale both ways, so a sample the widener does not move
         * comes back as the integer it was. Past full scale it clips: TRIM. */
        for (int i = 0; i < n * 2; i++) {
            float v = I->buf[i] * 32768.0f;
            v = v < -32768.0f ? -32768.0f : (v > 32767.0f ? 32767.0f : v);
            blk[i] = (int16_t)lrintf(v);
        }
    }
}

static int st_write_state(st_t *I, char *buf, int n) {
    int off = 0;
    for (const st_field *f = kFields; f->key; f++) {
        char v[32];
        if (st_get(I, f->key, v, sizeof v) < 0) return -1;
        int w = snprintf(buf + off, (size_t)(n - off), "%s%s=%s", off ? ";" : "", f->key, v);
        if (w < 0 || w >= n - off) return -1;
        off += w;
    }
    return off;
}

static void st_read_state(st_t *I, const char *val) {
    const char *s = val;
    while (s && *s) {
        const char *eq = strchr(s, '='), *semi = strchr(s, ';');
        if (!eq || (semi && eq > semi)) { if (!semi) break; s = semi + 1; continue; }
        char key[16], v[32];
        const size_t klen = (size_t)(eq - s);
        const size_t vlen = semi ? (size_t)(semi - eq - 1) : strlen(eq + 1);
        if (klen < sizeof key && vlen < sizeof v) {
            memcpy(key, s, klen); key[klen] = 0;
            memcpy(v, eq + 1, vlen); v[vlen] = 0;
            st_set(I, key, v);
        }
        if (!semi) break;
        s = semi + 1;
    }
}

static void st_set_param(void *inst, const char *key, const char *val) {
    st_t *I = (st_t *)inst;
    if (!I || !key || !val) return;
    if (!strcmp(key, "state")) { st_read_state(I, val); return; }
    st_set(I, key, val);
}

static int st_get_param(void *inst, const char *key, char *buf, int n) {
    st_t *I = (st_t *)inst;
    if (!I || !key || !buf || n <= 0) return -1;
    if (!strcmp(key, "state")) return st_write_state(I, buf, n);
    return st_get(I, key, buf, n);
}

static audio_fx_api_v2_t g_api;

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    (void)host;
    memset(&g_api, 0, sizeof g_api);
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = st_create;
    g_api.destroy_instance = st_destroy;
    g_api.process_block    = st_process;
    g_api.set_param        = st_set_param;
    g_api.get_param        = st_get_param;
    g_api.on_midi          = NULL;
    return &g_api;
}
