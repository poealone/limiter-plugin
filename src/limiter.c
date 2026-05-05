/**
 * Limiter — PocketDAW lookahead brick-wall limiter.
 *
 * Envelope follower with attack/release smoothing drives a static gain
 * reduction; output is hard-clamped to the ceiling. A short lookahead
 * buffer lets the gain reduction begin BEFORE a transient hits the
 * output, eliminating audible squashing.
 */

#include <SDL2/SDL.h>
#include "pocketdaw.h"
#include "pd_text.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define NUM_PARAMS  4
#define NUM_PRESETS 5
#define LOOKAHEAD_MAX 480  /* ~10ms @ 48kHz */

typedef struct {
    float sampleRate;
    float params[NUM_PARAMS];

    float lookL[LOOKAHEAD_MAX];
    float lookR[LOOKAHEAD_MAX];
    int   lookPos;

    float gainSmooth;   /* current gain reduction multiplier (1.0 = no GR) */
    float gainReduction; /* expose for UI; 0.0 = none, 1.0 = full duck    */
    float peakHold;      /* envelope follower */

    int   currentPreset;
} Limiter;

enum {
    P_THRESHOLD = 0,  /* 0=-24dB, 1=0dB    — output threshold for limiting */
    P_CEILING,        /* 0=-6dB,  1=0dB    — final hard clip */
    P_RELEASE,        /* 0=10ms,  1=500ms  */
    P_LOOKAHEAD       /* 0=0ms,   1=10ms   */
};

static const char* param_names[NUM_PARAMS] = {
    "Thresh", "Ceiling", "Release", "Lookahd"
};

static const float presets[NUM_PRESETS][NUM_PARAMS] = {
    /* Master   */ { 0.85f, 0.95f, 0.40f, 0.6f },
    /* Mix Bus  */ { 0.70f, 0.95f, 0.30f, 0.5f },
    /* Drum Bus */ { 0.55f, 0.90f, 0.15f, 0.7f },
    /* Vocal    */ { 0.60f, 0.92f, 0.50f, 0.4f },
    /* Loud     */ { 0.40f, 0.97f, 0.20f, 0.8f }
};

static const char* preset_names[NUM_PRESETS] = {
    "Master", "Mix Bus", "Drum Bus", "Vocal", "Loud"
};

/* dB <-> linear */
static inline float dbToLin(float db) { return powf(10.0f, db * 0.05f); }

/* ── Required API ──────────────────────────────────────── */

int pdfx_api_version(void) { return PDFX_API_VERSION; }
const char* pdfx_name(void) { return "Limiter"; }
int pdfx_param_count(void) { return NUM_PARAMS; }

PdFxInstance pdfx_create(float sampleRate) {
    Limiter* lm = (Limiter*)calloc(1, sizeof(Limiter));
    if (!lm) return NULL;
    lm->sampleRate = sampleRate;
    lm->gainSmooth = 1.0f;
    lm->currentPreset = 0;
    for (int i = 0; i < NUM_PARAMS; i++) lm->params[i] = presets[0][i];
    return lm;
}

void pdfx_destroy(PdFxInstance inst) { free(inst); }

void pdfx_process(PdFxInstance inst, PdFxAudio* audio) {
    Limiter* lm = (Limiter*)inst;
    if (!lm) return;

    if (audio->sampleRate > 0.0f && audio->sampleRate != lm->sampleRate)
        lm->sampleRate = audio->sampleRate;

    float thresholdDb = -24.0f + lm->params[P_THRESHOLD] * 24.0f;   /* -24..0 */
    float ceilingDb   = -6.0f  + lm->params[P_CEILING]   *  6.0f;   /*  -6..0 */
    float releaseMs   = 10.0f  + lm->params[P_RELEASE]   * 490.0f;  /* 10..500 */
    int   lookSamples = (int)(lm->params[P_LOOKAHEAD] * 0.010f * lm->sampleRate);
    if (lookSamples >= LOOKAHEAD_MAX) lookSamples = LOOKAHEAD_MAX - 1;
    if (lookSamples < 0) lookSamples = 0;

    float threshLin = dbToLin(thresholdDb);
    float ceilLin   = dbToLin(ceilingDb);
    float relCoef = expf(-1.0f / (releaseMs * 0.001f * lm->sampleRate));

    float maxGr = 0.0f;

    for (int i = 0; i < audio->bufferSize; i++) {
        float inL = audio->inputL[i];
        float inR = audio->inputR[i];

        /* Push current sample into lookahead buffer, read delayed sample. */
        lm->lookL[lm->lookPos] = inL;
        lm->lookR[lm->lookPos] = inR;
        int readPos = lm->lookPos - lookSamples;
        if (readPos < 0) readPos += LOOKAHEAD_MAX;
        float dL = lm->lookL[readPos];
        float dR = lm->lookR[readPos];
        lm->lookPos = (lm->lookPos + 1) % LOOKAHEAD_MAX;

        /* Detector: peak of current (un-delayed) input — this is what's */
        /* about to come out of the lookahead buffer in `lookSamples` steps. */
        float peak = fabsf(inL) > fabsf(inR) ? fabsf(inL) : fabsf(inR);

        /* Target gain: scale down so that peak * gain == threshLin */
        float targetGain = (peak > threshLin) ? threshLin / peak : 1.0f;

        /* Instant attack (worst case = clip), exponential release */
        if (targetGain < lm->gainSmooth) {
            lm->gainSmooth = targetGain;
        } else {
            lm->gainSmooth = targetGain + (lm->gainSmooth - targetGain) * relCoef;
        }
        if (lm->gainSmooth > 1.0f) lm->gainSmooth = 1.0f;
        if (lm->gainSmooth < 0.0f) lm->gainSmooth = 0.0f;

        float gr = 1.0f - lm->gainSmooth;
        if (gr > maxGr) maxGr = gr;

        /* Apply to lookahead-delayed signal, then hard-clip to ceiling */
        float yL = dL * lm->gainSmooth;
        float yR = dR * lm->gainSmooth;
        if (yL >  ceilLin) yL =  ceilLin; else if (yL < -ceilLin) yL = -ceilLin;
        if (yR >  ceilLin) yR =  ceilLin; else if (yR < -ceilLin) yR = -ceilLin;

        audio->outputL[i] = yL;
        audio->outputR[i] = yR;
    }

    lm->gainReduction = maxGr;
}

float pdfx_get_param(PdFxInstance inst, int index) {
    Limiter* lm = (Limiter*)inst;
    if (!lm || index < 0 || index >= NUM_PARAMS) return 0.0f;
    return lm->params[index];
}

void pdfx_set_param(PdFxInstance inst, int index, float value) {
    Limiter* lm = (Limiter*)inst;
    if (!lm || index < 0 || index >= NUM_PARAMS) return;
    lm->params[index] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    lm->currentPreset = -1;
}

const char* pdfx_param_name(int index) {
    if (index < 0 || index >= NUM_PARAMS) return NULL;
    return param_names[index];
}

void pdfx_reset(PdFxInstance inst) {
    Limiter* lm = (Limiter*)inst;
    if (!lm) return;
    memset(lm->lookL, 0, sizeof(lm->lookL));
    memset(lm->lookR, 0, sizeof(lm->lookR));
    lm->lookPos = 0;
    lm->gainSmooth = 1.0f;
    lm->gainReduction = 0.0f;
    lm->peakHold = 0.0f;
}

int pdfx_preset_count(void) { return NUM_PRESETS; }
const char* pdfx_preset_name(int index) {
    if (index < 0 || index >= NUM_PRESETS) return NULL;
    return preset_names[index];
}
void pdfx_load_preset(PdFxInstance inst, int index) {
    Limiter* lm = (Limiter*)inst;
    if (!lm || index < 0 || index >= NUM_PRESETS) return;
    for (int i = 0; i < NUM_PARAMS; i++) lm->params[i] = presets[index][i];
    lm->currentPreset = index;
}
int pdfx_get_preset(PdFxInstance inst) {
    Limiter* lm = (Limiter*)inst;
    return lm ? lm->currentPreset : -1;
}

/* ── Page 0 draw ──────────────────────────────────────── */

static void lm_fillRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderFillRect(r, &rc);
}
static void lm_drawRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderDrawRect(r, &rc);
}

int pdfx_draw(PdFxInstance inst, void* renderer, PdDrawContext* ctx) {
    Limiter* lm = (Limiter*)inst;
    SDL_Renderer* r = (SDL_Renderer*)renderer;
    if (!lm || !r || !ctx) return 0;

    int X = ctx->x, Y = ctx->y, W = ctx->w, H = ctx->h;
    int sel = ctx->selectedParam;
    int editing = ctx->editMode;

    /* Limiter accent — warning amber */
    const uint8_t AR = 255, AG = 180, AB = 60;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 14, 10, 4, 255);
    lm_fillRect(r, X, Y, W, H);

    /* Title band */
    int bandH = 14;
    SDL_SetRenderDrawColor(r, 22, 16, 6, 255);
    lm_fillRect(r, X, Y, W, bandH);
    pdt_drawStrC(r, X + 6, Y + 4, 255, 220, 160, 255, "LIMITER");
    SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
    SDL_RenderDrawLine(r, X, Y + bandH, X + W - 1, Y + bandH);

    /* Threshold + ceiling visualization — vertical scale with markers */
    {
        int fx = X + 8, fy = Y + 18, fw = W - 16, fh = 86;
        SDL_SetRenderDrawColor(r, 8, 6, 3, 255);
        lm_fillRect(r, fx, fy, fw, fh);
        SDL_SetRenderDrawColor(r, AR, AG, AB, ctx->peak > 0.01f ? 200 : 100);
        lm_drawRect(r, fx, fy, fw, fh);

        /* Vertical scale: top = 0dB, bottom = -24dB */
        float threshold01 = lm->params[P_THRESHOLD];
        float ceiling01   = lm->params[P_CEILING];

        /* Threshold line — yellow horizontal */
        int threshY = fy + (int)((1.0f - threshold01) * (fh - 4)) + 2;
        SDL_SetRenderDrawColor(r, AR, AG, AB, 200);
        SDL_RenderDrawLine(r, fx + 2, threshY, fx + fw - 2, threshY);
        for (int x = fx + 4; x < fx + fw - 2; x += 6) {
            SDL_RenderDrawPoint(r, x, threshY - 1);
        }

        /* Ceiling line — red */
        int ceilY = fy + (int)((1.0f - ceiling01 * 0.25f) * (fh - 4)) + 2;
        SDL_SetRenderDrawColor(r, 255, 80, 60, 220);
        SDL_RenderDrawLine(r, fx + 2, ceilY, fx + fw - 2, ceilY);

        /* Gain reduction bar — bright red bar growing down from top */
        float gr = lm->gainReduction; /* 0..1 */
        int grH = (int)(gr * (fh - 8));
        if (grH > 0) {
            SDL_SetRenderDrawColor(r, 255, 60, 40, 200);
            lm_fillRect(r, fx + fw / 2 - 8, fy + 4, 16, grH);
        }

        /* Input + output peak meters as vertical bars on left/right of GR */
        int pkH = (int)(ctx->peakL * (fh - 8));
        SDL_SetRenderDrawColor(r, 80, 220, 80, 220);
        lm_fillRect(r, fx + 6, fy + fh - 4 - pkH, 8, pkH);
        int pkR = (int)(ctx->peakR * (fh - 8));
        SDL_SetRenderDrawColor(r, 80, 220, 80, 220);
        lm_fillRect(r, fx + fw - 14, fy + fh - 4 - pkR, 8, pkR);

        /* Lookahead indicator — small bar at top */
        int laW = (int)(lm->params[P_LOOKAHEAD] * (fw - 8));
        SDL_SetRenderDrawColor(r, AR, AG / 2, AB / 2, 180);
        lm_fillRect(r, fx + 4, fy + 4, laW, 2);
    }

    /* Live oscilloscope */
    {
        int ox = X + 8, oy = Y + 108, ow = W - 16, oh = 26;
        SDL_SetRenderDrawColor(r, 8, 6, 3, 255);
        lm_fillRect(r, ox, oy, ow, oh);
        SDL_SetRenderDrawColor(r, AR, AG, AB, ctx->peak > 0.01f ? 200 : 80);
        lm_drawRect(r, ox, oy, ow, oh);
        int mid = oy + oh / 2;
        if (ctx->scopeLen > 0 && ctx->scopeBufL) {
            SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
            int px = ox + 2, py = mid;
            for (int i = 1; i < ow - 4; i++) {
                int si = i * ctx->scopeLen / (ow - 4);
                if (si >= ctx->scopeLen) break;
                int nx = ox + 2 + i;
                int ny = mid - (int)(ctx->scopeBufL[si] * (oh / 2 - 4));
                if (ny < oy + 2) ny = oy + 2;
                if (ny > oy + oh - 2) ny = oy + oh - 2;
                SDL_RenderDrawLine(r, px, py, nx, ny);
                px = nx; py = ny;
            }
        }
    }

    /* Peak meters L/R */
    {
        int mx = X + 8, my = Y + 138, mh = 12;
        int mw = (W - 20) / 2;
        for (int ch = 0; ch < 2; ch++) {
            float pk = (ch == 0) ? ctx->peakL : ctx->peakR;
            if (pk < 0.0f) pk = 0.0f; if (pk > 1.0f) pk = 1.0f;
            int x0 = mx + ch * (mw + 4);
            SDL_SetRenderDrawColor(r, 14, 10, 6, 255);
            lm_fillRect(r, x0, my, mw, mh);
            int fill = (int)(pk * (mw - 2));
            uint8_t mcR = pk > 0.7f ? 255 : 180;
            uint8_t mcG = pk < 0.7f ? 220 : (uint8_t)((1.0f - pk) * 290);
            uint8_t mcB = pk < 0.7f ? 100 : 60;
            SDL_SetRenderDrawColor(r, mcR, mcG, mcB, 255);
            lm_fillRect(r, x0 + 1, my + 1, fill, mh - 2);
            SDL_SetRenderDrawColor(r, AR, AG, AB, 120);
            lm_drawRect(r, x0, my, mw, mh);
        }
    }

    /* Param strip */
    {
        int stripY = Y + 154;
        int stripH = H - 154 - 2;
        if (stripH < 30) stripH = 30;
        int pCount = NUM_PARAMS;
        int stripPadX = 6;
        int cellW = (W - stripPadX * 2) / pCount;
        float vals[NUM_PARAMS];
        for (int i = 0; i < NUM_PARAMS; i++) vals[i] = lm->params[i];
        static const char* labels[NUM_PARAMS] = { "THR", "CEL", "REL", "LA " };

        SDL_SetRenderDrawColor(r, AR / 8, AG / 8, AB / 8, 200);
        SDL_RenderDrawLine(r, X + 4, stripY, X + W - 4, stripY);

        for (int p = 0; p < pCount; p++) {
            int cx = X + stripPadX + p * cellW;
            int cy = stripY + 4;
            int cw = cellW - 1;
            int ch2 = stripH - 6;
            int isSel = (sel == p);
            int isEdit = isSel && editing;
            pdt_drawParamCell(r, ctx, p, cx, cy, cw, ch2, labels[p], vals[p],
                              isSel, isEdit, AR, AG, AB);
        }
    }

    SDL_SetRenderDrawColor(r, AR, AG, AB, 80);
    lm_drawRect(r, X, Y, W, H);
    return 1;
}
