/* Xbox DirectSound (MCPX APU, XDK 4134) -> host: a software mixer played through XAudio2.
 *
 * Built from ReWhacked's mixer (XDK 4831). JSRF uses DirectSound buffers only (no streams or
 * file media objects): the engine's sound object (0x168F80..0x173xxx) plays its effects, and
 * CRI ADX (0x141E50) decodes music and voices itself into looping 16-bit PCM ring buffers that
 * it refills through Lock/Unlock while reading GetCurrentPosition. Every DSOUND function the
 * engine or ADX calls is patched (config/dsound_abi.csv gives each one's argument bytes).
 *
 * Every buffer is a voice of one mixer thread that renders 48 kHz stereo in 10 ms chunks into
 * a single XAudio2 voice; play cursors follow the mixer, so the game's view of time matches
 * what is heard. Formats: PCM (8/16-bit) and Xbox ADPCM (tag 0x69: IMA ADPCM, 36-byte blocks
 * per channel, 64 samples per block, matching vgmstream). Volume is in millibels less the
 * voice's headroom (default 6 dB).
 *
 * XDK 4134 differs from 4831 here: speaker routing is a mix-bin *mask* (DSBUFFERDESC field 4
 * and SetMixBins(DWORD)), not 4831's DSMIXBINS pointer, and buffers have SetLoopRegion. 3D
 * positioning, the effects DSP (reverb, I3DL2), envelopes, filters and LFOs are accepted and
 * not modelled yet. Master volume and mute-in-background come from the settings;
 * REJET_VOLUME=<percent> overrides the volume, REJET_NOAUDIO=1 mixes without output,
 * REJET_AUDIODUMP=1 records the final mix to mix.wav. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xaudio2.h>

#include "hle.h"
#include "kernel.h"
#include "settings.h"
#include "xboxkrnl_impl.h"
#include "xdk_addrs.gen.h"

#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_LOOPING 0x00000004
#define DSBPLAY_LOOPING   0x00000001
#define WAVE_FORMAT_XBOX_ADPCM 0x0069

#define OUT_RATE 48000
#define CHUNK 480 /* frames per mix (10 ms) */
#define QUEUED 3  /* chunks queued in XAudio2 */

typedef struct XWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} XWAVEFORMATEX;

/* XDK 4134 DSBUFFERDESC (dwSize 0x18; ADX fills it at 0x141E50). */
typedef struct XDSBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    XWAVEFORMATEX *lpwfxFormat;
    DWORD dwMixBinMask; /* 0 = default routing */
    DWORD dwInputMixBin;
} XDSBUFFERDESC;

/* Mix-bin mask bits (speaker bins; higher bits feed effects and are not modelled). */
#define MIXBIN_FRONT_LEFT  0x00000001
#define MIXBIN_FRONT_RIGHT 0x00000002
#define MIXBIN_CENTER      0x00000004
#define MIXBIN_LFE         0x00000008
#define MIXBIN_BACK_LEFT   0x00000010
#define MIXBIN_BACK_RIGHT  0x00000020

/* ------------------------------------------------------------------ sample formats */

typedef struct Format {
    WORD tag, channels, align, bits;
    DWORD rate;
} Format;

static void format_from(Format *f, const XWAVEFORMATEX *w)
{
    if (!w) { /* no format: 48 kHz stereo 16-bit */
        f->tag = 1;
        f->channels = 2;
        f->rate = OUT_RATE;
        f->bits = 16;
        f->align = 4;
        return;
    }
    f->tag = w->wFormatTag;
    f->channels = w->nChannels ? w->nChannels : 1;
    f->rate = w->nSamplesPerSec ? w->nSamplesPerSec : OUT_RATE;
    f->bits = w->wBitsPerSample ? w->wBitsPerSample : 16;
    f->align = w->nBlockAlign ? w->nBlockAlign
                              : (WORD)(f->tag == WAVE_FORMAT_XBOX_ADPCM ? 36 * f->channels
                                                                        : f->channels * f->bits / 8);
}

static int is_adpcm(const Format *f)
{
    return f->tag == WAVE_FORMAT_XBOX_ADPCM;
}

/* Whole frames in `bytes` of data. */
static DWORD frames_in(const Format *f, DWORD bytes)
{
    return is_adpcm(f) ? bytes / f->align * 64 : bytes / f->align;
}

static DWORD frames_to_bytes(const Format *f, DWORD frames)
{
    return is_adpcm(f) ? frames / 64 * f->align : frames * f->align;
}

static const int IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

/* Decode one Xbox ADPCM block (all channels) into out[64 * channels], interleaved. */
static void adpcm_block(const BYTE *blk, unsigned channels, short *out)
{
    for (unsigned c = 0; c < channels; c++) {
        int pred = (short)(blk[c * 4] | (blk[c * 4 + 1] << 8));
        int index = blk[c * 4 + 2];
        if (index > 88)
            index = 88;
        /* As vgmstream decodes it: the header sample is output first, then the first 63 of
         * the block's 64 nibbles (the last nibble only feeds the next block's header). */
        out[c] = (short)pred;
        for (unsigned g = 0; g < 8; g++) {
            const BYTE *w = blk + 4 * channels + (g * channels + c) * 4;
            for (unsigned n = 0; n < 8 && g * 8 + n < 63; n++) {
                int nib = (w[n >> 1] >> ((n & 1) * 4)) & 15, step = IMA_STEP[index];
                int diff = step >> 3;
                if (nib & 4)
                    diff += step;
                if (nib & 2)
                    diff += step >> 1;
                if (nib & 1)
                    diff += step >> 2;
                pred += (nib & 8) ? -diff : diff;
                if (pred > 32767)
                    pred = 32767;
                if (pred < -32768)
                    pred = -32768;
                index += IMA_INDEX[nib];
                if (index < 0)
                    index = 0;
                if (index > 88)
                    index = 88;
                out[(g * 8 + n + 1) * channels + c] = (short)pred;
            }
        }
    }
}

/* ------------------------------------------------------------------ voices */

typedef struct Voice {
    Format fmt;
    DWORD frequency;  /* playback rate (SetFrequency) */
    LONG volume;      /* millibels */
    DWORD headroom;   /* millibels of attenuation (Xbox default 600 = -6 dB) */
    DWORD mask;       /* mix-bin mask */
    float gain[2][2]; /* [source channel][L, R] from the mix bins */
    int clip_logged;  /* diagnostics: logged once when this voice alone exceeds full scale */
    /* ADPCM decode cache */
    const BYTE *cache_src;
    short cache[64 * 8];
} Voice;

static float mb_to_amp(LONG mb)
{
    if (mb <= -10000)
        return 0.0f;
    return (float)pow(10.0, (double)mb / 2000.0);
}

/* Master volume: REJET_VOLUME=<percent> or the settings (default 100). */
static float master_volume(void)
{
    static float m = -1.0f;
    if (m < 0.0f) {
        char env[16] = "";
        m = GetEnvironmentVariableA("REJET_VOLUME", env, sizeof env) ? (float)atoi(env) / 100.0f
                                                                      : (float)g_settings.volume / 100.0f;
        if (m < 0.0f)
            m = 0.0f;
    }
    return m;
}

static float voice_amp(const Voice *v)
{
    return mb_to_amp(v->volume - (LONG)v->headroom) * master_volume();
}

/* Speaker bins -> stereo. Mono: every bin in the mask carries the channel. Stereo: the mask's
 * speaker bits are taken in order, channel 0, 1, 0, 1, ... (verify against the game's masks; the
 * log lists each mask once). Folded with the loudest bin per side, not the sum (ReWhacked: the
 * sum of front, back and centre clipped); back and centre at -3 dB. */
static void voice_set_mixmask(Voice *v, DWORD mask)
{
    static const DWORD bins[6] = {MIXBIN_FRONT_LEFT, MIXBIN_FRONT_RIGHT, MIXBIN_CENTER, MIXBIN_LFE, MIXBIN_BACK_LEFT,
                                  MIXBIN_BACK_RIGHT};
    static const float to_l[6] = {1.0f, 0.0f, 0.7071f, 0.0f, 0.7071f, 0.0f};
    static const float to_r[6] = {0.0f, 1.0f, 0.7071f, 0.0f, 0.0f, 0.7071f};
    unsigned k = 0;
    v->mask = mask;
    memset(v->gain, 0, sizeof v->gain);
    if (!(mask & 0x3F)) { /* default routing */
        if (v->fmt.channels == 1) {
            v->gain[0][0] = v->gain[0][1] = 0.7071f;
        }
        else {
            v->gain[0][0] = 1.0f;
            v->gain[1][1] = 1.0f;
        }
        return;
    }
    for (unsigned i = 0; i < 6; i++) {
        unsigned ch;
        if (!(mask & bins[i]))
            continue;
        ch = v->fmt.channels == 1 ? 0 : k++ % 2;
        v->gain[ch][0] = max(v->gain[ch][0], to_l[i]);
        v->gain[ch][1] = max(v->gain[ch][1], to_r[i]);
    }
    {
        static DWORD seen[32];
        static unsigned nseen;
        unsigned i;
        for (i = 0; i < nseen && seen[i] != mask; i++)
            ;
        if (i == nseen && nseen < 32) {
            seen[nseen++] = mask;
            log_printf("dsound: mix-bin mask %08lX (%u ch) -> gains %.2f %.2f / %.2f %.2f\n", mask, v->fmt.channels,
                       v->gain[0][0], v->gain[0][1], v->gain[1][0], v->gain[1][1]);
        }
    }
}

static void voice_init(Voice *v, const XWAVEFORMATEX *w, DWORD mask)
{
    memset(v, 0, sizeof *v);
    format_from(&v->fmt, w);
    v->frequency = v->fmt.rate;
    {
        static DWORD seen[32];
        static unsigned nseen;
        DWORD key = (DWORD)v->fmt.tag << 24 ^ (DWORD)v->fmt.channels << 20 ^ v->fmt.rate ^ (DWORD)v->fmt.bits << 16 ^
                    (DWORD)v->fmt.align << 8;
        unsigned i;
        for (i = 0; i < nseen && seen[i] != key; i++)
            ;
        if (i == nseen && nseen < 32) {
            seen[nseen++] = key;
            log_printf("dsound: format tag %04X, %u ch, %lu Hz, %u bits, align %u\n", v->fmt.tag, v->fmt.channels,
                       v->fmt.rate, v->fmt.bits, v->fmt.align);
        }
    }
    v->headroom = 600;
    voice_set_mixmask(v, mask);
}

/* Frame `f` of `data` as floats in out[0..1] (mono duplicated). */
static void voice_frame(Voice *v, const BYTE *data, DWORD f, float out[2])
{
    unsigned ch = v->fmt.channels > 2 ? 2 : v->fmt.channels;
    short s[2] = {0, 0};
    if (is_adpcm(&v->fmt)) {
        const BYTE *blk = data + (f / 64) * v->fmt.align;
        if (v->cache_src != blk) {
            adpcm_block(blk, v->fmt.channels > 8 ? 8 : v->fmt.channels, v->cache);
            v->cache_src = blk;
        }
        for (unsigned c = 0; c < ch; c++)
            s[c] = v->cache[(f % 64) * v->fmt.channels + c];
    }
    else if (v->fmt.bits == 8) {
        for (unsigned c = 0; c < ch; c++)
            s[c] = (short)((data[f * v->fmt.align + c] - 128) << 8);
    }
    else {
        for (unsigned c = 0; c < ch; c++)
            s[c] = *(const short *)(data + f * v->fmt.align + c * 2);
    }
    out[0] = s[0] / 32768.0f;
    out[1] = (ch > 1 ? s[1] : s[0]) / 32768.0f;
}

/* ------------------------------------------------------------------ objects */

typedef struct DsObject { /* IDirectSound: the engine and ADX only use it through the C wrappers */
    void *vtable;
    LONG refs;
} DsObject;

typedef struct Buffer {
    void *vtable;
    LONG refs;
    Voice v;
    BYTE *data;
    DWORD bytes;
    int playing, looping;
    DWORD loop_start, loop_frames; /* SetLoopRegion, in frames (0 frames = the whole buffer) */
    double pos;                    /* frames */
} Buffer;

static volatile LONGLONG g_mixed; /* output frames mixed so far (the mix clock) */

static double mix_seconds(void)
{
    return (double)g_mixed / OUT_RATE;
}

#define MAX_VOICES 1024
static DsObject g_ds = {NULL, 1};
static CRITICAL_SECTION g_lock;
static Buffer *g_buffers[MAX_VOICES];
static unsigned g_buffer_count;

/* Log the first call of each accepted-but-not-modelled entry point. */
#define TODO_ONCE(name)                                                                        \
    do {                                                                                       \
        static LONG seen;                                                                      \
        if (!InterlockedExchange(&seen, 1))                                                    \
            log_printf("dsound: %s accepted, not modelled yet (first call from %08X)\n", name, \
                       (unsigned)(UINT_PTR)_ReturnAddress());                                  \
    } while (0)

/* ------------------------------------------------------------------ mixer */

/* Diagnostics: log a voice once if it alone exceeds full scale. */
static void check_clip(Voice *v, float l, float r)
{
    if (!v->clip_logged && (fabsf(l) > 1.0f || fabsf(r) > 1.0f)) {
        v->clip_logged = 1;
        log_printf("dsound: buffer clips (%.2f %.2f): tag %04X %u ch %lu Hz (plays at %lu) vol %ld headroom %lu "
                   "mask %08lX\n", l, r, v->fmt.tag, v->fmt.channels, v->fmt.rate, v->frequency, v->volume,
                   v->headroom, v->mask);
    }
}

static void mix_buffer(Buffer *b, float *out, unsigned frames)
{
    DWORD total = b->data ? frames_in(&b->v.fmt, b->bytes) : 0;
    DWORD loop_start = 0, loop_end = total;
    double step = (double)b->v.frequency / OUT_RATE;
    float amp = voice_amp(&b->v);
    if (!total) {
        b->playing = 0;
        return;
    }
    if (b->looping && b->loop_frames && b->loop_start + b->loop_frames <= total) {
        loop_start = b->loop_start;
        loop_end = b->loop_start + b->loop_frames;
    }
    for (unsigned i = 0; i < frames; i++) {
        DWORD f0 = (DWORD)b->pos, f1 = f0 + 1;
        float a[2], c[2], t = (float)(b->pos - f0);
        if (f0 >= (b->looping ? loop_end : total)) {
            if (!b->looping) {
                b->playing = 0;
                b->pos = 0;
                return;
            }
            b->pos = loop_start + fmod(b->pos - loop_start, (double)(loop_end - loop_start));
            f0 = (DWORD)b->pos;
            f1 = f0 + 1;
        }
        if (f1 >= (b->looping ? loop_end : total))
            f1 = b->looping ? loop_start : f0;
        voice_frame(&b->v, b->data, f0, a);
        voice_frame(&b->v, b->data, f1, c);
        a[0] += (c[0] - a[0]) * t;
        a[1] += (c[1] - a[1]) * t;
        {
            float l = amp * (a[0] * b->v.gain[0][0] + a[1] * b->v.gain[1][0]);
            float r = amp * (a[0] * b->v.gain[0][1] + a[1] * b->v.gain[1][1]);
            check_clip(&b->v, l, r);
            out[2 * i] += l;
            out[2 * i + 1] += r;
        }
        b->pos += step;
    }
}

/* Render `frames` of output (48 kHz stereo 16-bit). */
static void mix_chunk(short *out, unsigned frames)
{
    static float acc[CHUNK * 2];
    memset(acc, 0, frames * 2 * sizeof acc[0]);
    EnterCriticalSection(&g_lock);
    for (unsigned i = 0; i < g_buffer_count; i++)
        if (g_buffers[i]->playing)
            mix_buffer(g_buffers[i], acc, frames);
    LeaveCriticalSection(&g_lock);
    {
        /* Every 10 s: busiest moment's voice count and the peak level, for the log. */
        static unsigned chunks, most;
        static float peak;
        unsigned n = 0;
        EnterCriticalSection(&g_lock);
        for (unsigned i = 0; i < g_buffer_count; i++)
            n += g_buffers[i]->playing != 0;
        LeaveCriticalSection(&g_lock);
        if (n > most)
            most = n;
        for (unsigned i = 0; i < frames * 2; i++)
            if (fabsf(acc[i]) > peak)
                peak = fabsf(acc[i]);
        if (++chunks == 1000) {
            log_printf("dsound: last 10 s: up to %u buffers playing, peak %.2f\n", most, peak);
            chunks = most = 0;
            peak = 0;
        }
    }
    g_mixed += frames;
    if (g_settings.mute_in_background) { /* settings: silent while another program has the focus */
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid != GetCurrentProcessId())
            memset(acc, 0, frames * 2 * sizeof acc[0]);
    }
    {
        /* Limiter (ReWhacked): when the mix would exceed the ceiling, turn the whole mix down at
         * once and recover over ~150 ms, instead of clipping. */
        static float gain = 1.0f;
        const float ceiling = 0.95f, release = 1.0f / (0.15f * OUT_RATE);
        for (unsigned i = 0; i < frames; i++) {
            float l = acc[2 * i], r = acc[2 * i + 1], m = max(fabsf(l), fabsf(r)), x;
            if (m * gain > ceiling)
                gain = ceiling / m;
            else if (gain < 1.0f)
                gain = min(1.0f, gain + release);
            x = l * gain * 32767.0f;
            out[2 * i] = (short)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int)x);
            x = r * gain * 32767.0f;
            out[2 * i + 1] = (short)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int)x);
        }
    }
    {
        /* REJET_AUDIODUMP=1: record the final mix to mix.wav (48 kHz stereo 16-bit; the header's
         * sizes are left at their maximum since the run may end abruptly). */
        static FILE *rec;
        static int checked;
        if (!checked) {
            char env[4] = "";
            checked = 1;
            if (GetEnvironmentVariableA("REJET_AUDIODUMP", env, sizeof env) && env[0] == '1' &&
                (rec = fopen("mix.wav", "wb")) != NULL) {
                static const unsigned char hdr[44] = {'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0x7F, 'W', 'A', 'V', 'E',
                                                      'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                                                      0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0, 4, 0, 16, 0,
                                                      'd', 'a', 't', 'a', 0xDB, 0xFF, 0xFF, 0x7F};
                fwrite(hdr, 1, sizeof hdr, rec);
                log_printf("dsound: recording the mix to mix.wav\n");
            }
        }
        if (rec) {
            fwrite(out, 4, frames, rec);
            fflush(rec);
        }
    }
}

/* Host thread: renders chunks and keeps XAudio2 fed (or, without output, keeps time). */
static DWORD WINAPI mixer_thread(void *unused)
{
    static short ring[QUEUED + 1][CHUNK * 2];
    IXAudio2 *xa = NULL;
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *voice = NULL;
    unsigned slot = 0;
    char env[4] = "";
    (void)unused;

    if (!(GetEnvironmentVariableA("REJET_NOAUDIO", env, sizeof env) && env[0] == '1')) {
        WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, OUT_RATE, OUT_RATE * 4, 4, 16, 0};
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        (void)hr;
        if (FAILED(hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
            FAILED(hr = IXAudio2_CreateMasteringVoice(xa, &master, 2, OUT_RATE, 0, NULL, NULL,
                                                      AudioCategory_GameEffects)) ||
            FAILED(hr = IXAudio2_CreateSourceVoice(xa, &voice, &wf, 0, 2.0f, NULL, NULL, NULL)) ||
            FAILED(hr = IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW))) {
            log_printf("dsound: XAudio2 output unavailable (%08lX); running silent\n", hr);
            voice = NULL;
        }
        else {
            log_printf("dsound: XAudio2 output, %u Hz stereo\n", OUT_RATE);
        }
    }
    else {
        log_printf("dsound: REJET_NOAUDIO=1, running silent\n");
    }

    if (voice) {
        unsigned submitted = 0;
        UINT32 glitches = 0;
        for (;;) {
            XAUDIO2_VOICE_STATE st;
            IXAudio2SourceVoice_GetState(voice, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            while (st.BuffersQueued < QUEUED) {
                XAUDIO2_BUFFER xb;
                memset(&xb, 0, sizeof xb);
                mix_chunk(ring[slot], CHUNK);
                xb.AudioBytes = CHUNK * 4;
                xb.pAudioData = (const BYTE *)ring[slot];
                IXAudio2SourceVoice_SubmitSourceBuffer(voice, &xb, NULL);
                slot = (slot + 1) % (QUEUED + 1);
                st.BuffersQueued++;
                if (++submitted % 1000 == 0) { /* every 10 s: output underruns (audible skips) */
                    XAUDIO2_PERFORMANCE_DATA perf;
                    IXAudio2_GetPerformanceData(xa, &perf);
                    if (perf.GlitchesSinceEngineStarted != glitches)
                        log_printf("dsound: XAudio2 ran dry %lu times in the last 10 s\n",
                                   perf.GlitchesSinceEngineStarted - glitches);
                    glitches = perf.GlitchesSinceEngineStarted;
                }
            }
            Sleep(2);
        }
    }
    else { /* no output device: mix on a 10 ms clock so game timing still works */
        LARGE_INTEGER f, start, now;
        ULONGLONG chunks = 0;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&start);
        for (;;) {
            QueryPerformanceCounter(&now);
            while ((ULONGLONG)((now.QuadPart - start.QuadPart) * 100 / f.QuadPart) >= chunks) {
                mix_chunk(ring[0], CHUNK);
                chunks++;
            }
            Sleep(2);
        }
    }
}

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK start_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o;
    (void)p;
    (void)c;
    InitializeCriticalSection(&g_lock);
    CloseHandle(CreateThread(NULL, 1 << 20, mixer_thread, NULL, 0, NULL));
    return TRUE;
}

static void ensure_started(void)
{
    InitOnceExecuteOnce(&g_once, start_once, NULL, NULL);
}

/* ------------------------------------------------------------------ DirectSound */

static HRESULT XBAPI hle_DirectSoundCreate(void *pGuid, DsObject **ppDS, void *pUnkOuter)
{
    (void)pGuid;
    (void)pUnkOuter;
    ensure_started();
    InterlockedIncrement(&g_ds.refs);
    *ppDS = &g_ds;
    return S_OK;
}

static ULONG XBAPI hle_IDirectSound_AddRef(DsObject *ds)
{
    return (ULONG)InterlockedIncrement(&ds->refs);
}

/* One shared object that is never destroyed; the engine releases a second one it creates only
 * to download the effects image (0x168C10). */
static ULONG XBAPI hle_IDirectSound_Release(DsObject *ds)
{
    LONG left = InterlockedDecrement(&ds->refs);
    if (left < 1)
        InterlockedExchange(&ds->refs, left = 1);
    return (ULONG)left;
}

/* Xbox DSCAPS: free 2D voices, free 3D voices, free buffer SGEs, memory allocated. */
static HRESULT XBAPI hle_IDirectSound_GetCaps(DsObject *ds, DWORD *pCaps)
{
    (void)ds;
    pCaps[0] = 256;
    pCaps[1] = 64;
    pCaps[2] = 2047;
    pCaps[3] = 0;
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSound_GetSpeakerConfig(DsObject *ds, DWORD *pConfig)
{
    (void)ds;
    *pConfig = 0x00000002; /* DSSPEAKER_STEREO */
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSound_DownloadEffectsImage(DsObject *ds, const void *pvImage, DWORD dwSize,
                                                           const void *pLoc, void **ppDesc)
{
    static DWORD empty_desc[4]; /* no effects (the engine does not read it) */
    (void)ds;
    (void)pvImage;
    (void)pLoc;
    log_printf("dsound: DownloadEffectsImage (%lu bytes) skipped (no DSP yet)\n", dwSize);
    if (ppDesc)
        *ppDesc = empty_desc;
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSound_GetEffectData(DsObject *ds, DWORD dwEffectIndex, DWORD dwOffset, void *pvData,
                                                    DWORD dwDataSize)
{
    (void)ds;
    (void)dwEffectIndex;
    (void)dwOffset;
    TODO_ONCE("IDirectSound_GetEffectData (returns zeros)");
    if (pvData)
        memset(pvData, 0, dwDataSize);
    return S_OK;
}

static void XBAPI hle_DirectSoundDoWork(void)
{
}

/* ------------------------------------------------------------------ buffers */

static HRESULT XBAPI hle_IDirectSound_CreateSoundBuffer(DsObject *ds, const XDSBUFFERDESC *desc, Buffer **ppBuffer,
                                                        void *pUnkOuter)
{
    Buffer *b = (Buffer *)calloc(1, sizeof *b);
    (void)ds;
    (void)pUnkOuter;
    ensure_started();
    b->refs = 1;
    voice_init(&b->v, desc->lpwfxFormat, desc->dwMixBinMask);
    b->bytes = desc->dwBufferBytes;
    if (b->bytes)
        b->data = (BYTE *)calloc(1, b->bytes);
    {
        static LONG n;
        if (InterlockedIncrement(&n) <= 40)
            log_printf("dsound: CreateSoundBuffer flags %08lX, %lu bytes, mask %08lX (from %08X)\n", desc->dwFlags,
                       desc->dwBufferBytes, desc->dwMixBinMask, (unsigned)(UINT_PTR)_ReturnAddress());
    }
    EnterCriticalSection(&g_lock);
    if (g_buffer_count < MAX_VOICES)
        g_buffers[g_buffer_count++] = b;
    else
        log_printf("dsound: more than %u buffers; extra ones are silent\n", MAX_VOICES);
    LeaveCriticalSection(&g_lock);
    *ppBuffer = b;
    return S_OK;
}

static ULONG XBAPI hle_IDirectSoundBuffer_AddRef(Buffer *b)
{
    return (ULONG)InterlockedIncrement(&b->refs);
}

static ULONG XBAPI hle_IDirectSoundBuffer_Release(Buffer *b)
{
    ULONG left;
    if (!b || !b->refs)
        return 0;
    EnterCriticalSection(&g_lock);
    left = (ULONG)--b->refs;
    if (!left) {
        for (unsigned i = 0; i < g_buffer_count; i++) {
            if (g_buffers[i] == b) {
                g_buffers[i] = g_buffers[--g_buffer_count];
                break;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    if (!left)
        free(b); /* data belongs to the game after SetBufferData; our own allocation is leaked */
    return left;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_SetBufferData(Buffer *b, void *pData, DWORD bytes)
{
    EnterCriticalSection(&g_lock);
    b->data = (BYTE *)pData;
    b->bytes = bytes;
    b->playing = 0;
    b->pos = 0;
    b->v.cache_src = NULL;
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

/* Lock(offset, bytes, ...) with bytes 0: ADX locks the whole buffer that way (0x142xxx). */
static HRESULT XBAPI hle_IDirectSoundBuffer_Lock(Buffer *b, DWORD offset, DWORD bytes, void **p1, DWORD *n1,
                                                 void **p2, DWORD *n2, DWORD flags)
{
    (void)flags;
    if (!b->data || !b->bytes)
        return E_FAIL;
    offset %= b->bytes;
    if (!bytes || bytes > b->bytes)
        bytes = b->bytes;
    *p1 = b->data + offset;
    *n1 = offset + bytes <= b->bytes ? bytes : b->bytes - offset;
    if (p2)
        *p2 = *n1 < bytes ? b->data : NULL;
    if (n2)
        *n2 = bytes - *n1;
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_Unlock(Buffer *b, void *p1, DWORD n1, void *p2, DWORD n2)
{
    (void)p1;
    (void)n1;
    (void)p2;
    (void)n2;
    b->v.cache_src = NULL; /* the game may have rewritten ADPCM blocks */
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_Play(Buffer *b, DWORD r1, DWORD r2, DWORD flags)
{
    (void)r1;
    (void)r2;
    {
        static LONG plays;
        if (InterlockedIncrement(&plays) <= 300)
            log_printf("dsound: %8.2f s play %p (%lu frames, %u ch %lu Hz, vol %ld%s)\n", mix_seconds(), (void *)b,
                       b->data ? frames_in(&b->v.fmt, b->bytes) : 0, b->v.fmt.channels, b->v.frequency, b->v.volume,
                       (flags & DSBPLAY_LOOPING) ? ", looping" : "");
    }
    EnterCriticalSection(&g_lock);
    b->playing = 1;
    b->looping = (flags & DSBPLAY_LOOPING) != 0;
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_Stop(Buffer *b)
{
    EnterCriticalSection(&g_lock);
    b->playing = 0;
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_GetStatus(Buffer *b, DWORD *pStatus)
{
    EnterCriticalSection(&g_lock);
    *pStatus = b->playing ? (DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0)) : 0;
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_GetCurrentPosition(Buffer *b, DWORD *pPlay, DWORD *pWrite)
{
    DWORD c;
    EnterCriticalSection(&g_lock);
    c = frames_to_bytes(&b->v.fmt, (DWORD)b->pos);
    if (b->bytes)
        c %= b->bytes;
    if (pPlay)
        *pPlay = c;
    if (pWrite) /* the write cursor leads by about one mix chunk */
        *pWrite = b->bytes ? (c + frames_to_bytes(&b->v.fmt, CHUNK * b->v.frequency / OUT_RATE + 64)) % b->bytes
                           : 0;
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_SetCurrentPosition(Buffer *b, DWORD pos)
{
    EnterCriticalSection(&g_lock);
    b->pos = frames_in(&b->v.fmt, pos);
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_SetLoopRegion(Buffer *b, DWORD dwLoopStart, DWORD dwLoopLength)
{
    EnterCriticalSection(&g_lock);
    b->loop_start = frames_in(&b->v.fmt, dwLoopStart);
    b->loop_frames = frames_in(&b->v.fmt, dwLoopLength);
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_SetFrequency(Buffer *b, DWORD freq)
{
    EnterCriticalSection(&g_lock);
    b->v.frequency = freq ? freq : b->v.fmt.rate; /* 0 = the format's own rate */
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_SetVolume(Buffer *b, LONG volume)
{
    b->v.volume = volume;
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_SetHeadroom(Buffer *b, DWORD headroom)
{
    b->v.headroom = headroom;
    return S_OK;
}

static HRESULT XBAPI hle_IDirectSoundBuffer_SetMixBins(Buffer *b, DWORD dwMixBinMask)
{
    EnterCriticalSection(&g_lock);
    voice_set_mixmask(&b->v, dwMixBinMask);
    LeaveCriticalSection(&g_lock);
    return S_OK;
}

/* ------------------------------------------------------------------ accepted, not modelled */

/* Typed no-ops for 3D positioning, listener and effects calls: each pops exactly the original's
 * argument bytes (config/dsound_abi.csv) and logs its first call. */
#pragma warning(push)
#pragma warning(disable : 4100) /* unreferenced parameters */
#define NOOP(name, ...)                         \
    static HRESULT XBAPI noop_##name(__VA_ARGS__) \
    {                                           \
        TODO_ONCE(#name);                       \
        return S_OK;                            \
    }
#define D DWORD
NOOP(CommitDeferredSettings, D a)
NOOP(SetEffectData, D a, D b, D c, D d, D e, D f)
NOOP(Listener_3args, D a, D b, D c)                    /* SetAllParameters, distance/doppler/rolloff, I3DL2 */
NOOP(Listener_5args, D a, D b, D c, D d, D e)          /* SetPosition, SetVelocity */
NOOP(Listener_SetOrientation, D a, D b, D c, D d, D e, D f, D g, D h)
NOOP(Buffer_2args, D a, D b)                           /* SetLFO, SetEG, SetFilter */
NOOP(Buffer_3args, D a, D b, D c)                      /* SetAllParameters, cone volume, distances, mode, I3DL2 */
NOOP(Buffer_4args, D a, D b, D c, D d)                 /* SetConeAngles */
NOOP(Buffer_5args, D a, D b, D c, D d, D e)            /* SetConeOrientation, SetPosition, SetVelocity */
#undef D
#undef NOOP

/* 0x19EFE3 (unnamed; called once after DirectSoundCreate, 0x168F9F): takes DirectSound's
 * internal lock and configures the APU. */
static void XBAPI noop_19EFE3(void)
{
    TODO_ONCE("DSOUND 0x19EFE3");
}
#pragma warning(pop)

/* ------------------------------------------------------------------ patch table */

#define P(name, impl) {XDK_DSOUND_##name, (void *)(impl), #name}
const HlePatch g_audio_patches[] = {
    P(DirectSoundCreate, hle_DirectSoundCreate),
    P(DirectSoundDoWork, hle_DirectSoundDoWork),
    {0x0019EFE3u, (void *)noop_19EFE3, "DSOUND 0x19EFE3"},
    /* (0x19EF7C and 0x19EF81 only return 0 and keep running as original code) */
    P(IDirectSound_AddRef, hle_IDirectSound_AddRef),
    P(IDirectSound_Release, hle_IDirectSound_Release),
    P(IDirectSound_GetCaps, hle_IDirectSound_GetCaps),
    P(IDirectSound_GetSpeakerConfig, hle_IDirectSound_GetSpeakerConfig),
    P(IDirectSound_DownloadEffectsImage, hle_IDirectSound_DownloadEffectsImage),
    P(IDirectSound_GetEffectData, hle_IDirectSound_GetEffectData),
    P(IDirectSound_SetEffectData, noop_SetEffectData),
    P(IDirectSound_CommitDeferredSettings, noop_CommitDeferredSettings),
    P(IDirectSound_CreateSoundBuffer, hle_IDirectSound_CreateSoundBuffer),
    P(IDirectSound_SetAllParameters, noop_Listener_3args),
    P(IDirectSound_SetDistanceFactor, noop_Listener_3args),
    P(IDirectSound_SetDopplerFactor, noop_Listener_3args),
    P(IDirectSound_SetRolloffFactor, noop_Listener_3args),
    P(IDirectSound_SetI3DL2Listener, noop_Listener_3args),
    P(IDirectSound_SetPosition, noop_Listener_5args),
    P(IDirectSound_SetVelocity, noop_Listener_5args),
    P(IDirectSound_SetOrientation, noop_Listener_SetOrientation),
    P(IDirectSoundBuffer_AddRef, hle_IDirectSoundBuffer_AddRef),
    P(IDirectSoundBuffer_Release, hle_IDirectSoundBuffer_Release),
    P(IDirectSoundBuffer_SetBufferData, hle_IDirectSoundBuffer_SetBufferData),
    P(IDirectSoundBuffer_Lock, hle_IDirectSoundBuffer_Lock),
    P(IDirectSoundBuffer_Unlock, hle_IDirectSoundBuffer_Unlock),
    P(IDirectSoundBuffer_Play, hle_IDirectSoundBuffer_Play),
    P(IDirectSoundBuffer_Stop, hle_IDirectSoundBuffer_Stop),
    P(IDirectSoundBuffer_GetStatus, hle_IDirectSoundBuffer_GetStatus),
    P(IDirectSoundBuffer_GetCurrentPosition, hle_IDirectSoundBuffer_GetCurrentPosition),
    P(IDirectSoundBuffer_SetCurrentPosition, hle_IDirectSoundBuffer_SetCurrentPosition),
    P(IDirectSoundBuffer_SetLoopRegion, hle_IDirectSoundBuffer_SetLoopRegion),
    P(IDirectSoundBuffer_SetFrequency, hle_IDirectSoundBuffer_SetFrequency),
    P(IDirectSoundBuffer_SetVolume, hle_IDirectSoundBuffer_SetVolume),
    P(IDirectSoundBuffer_SetHeadroom, hle_IDirectSoundBuffer_SetHeadroom),
    P(IDirectSoundBuffer_SetMixBins, hle_IDirectSoundBuffer_SetMixBins),
    P(IDirectSoundBuffer_SetLFO, noop_Buffer_2args),
    P(IDirectSoundBuffer_SetEG, noop_Buffer_2args),
    P(IDirectSoundBuffer_SetFilter, noop_Buffer_2args),
    P(IDirectSoundBuffer_SetAllParameters, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetConeOutsideVolume, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetMaxDistance, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetMinDistance, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetMode, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetDistanceFactor, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetDopplerFactor, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetRolloffFactor, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetI3DL2Source, noop_Buffer_3args),
    P(IDirectSoundBuffer_SetConeAngles, noop_Buffer_4args),
    P(IDirectSoundBuffer_SetConeOrientation, noop_Buffer_5args),
    P(IDirectSoundBuffer_SetPosition, noop_Buffer_5args),
    P(IDirectSoundBuffer_SetVelocity, noop_Buffer_5args),
};
#undef P
const unsigned g_audio_patch_count = sizeof g_audio_patches / sizeof g_audio_patches[0];
