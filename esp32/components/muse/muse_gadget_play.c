/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Sounds Muse asks the gadget for (voice.say, audio.play_url and audio.chime
 * in muse_gadget_cmds.c): queued, made into 16 kHz PCM by a player task of
 * their own, and played by the voice task whenever it's idle (muse_voice.c),
 * so they never cut into a push-to-talk turn; a talk press cuts them off.
 * Speech reuses the reply path's parts: muse_tts_* for ElevenLabs, minimp3,
 * and muse_hatch_caption_at for the caption's pages.
 *
 * Everything from "Pure (host-tested)" to "Device" builds on the host:
 * tests/test_muse_gadget_cmds.py.
 */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
/* Always in the build (components/muse/CMakeLists.txt): nothing without spoken replies. */
#if (CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH) || !defined(ESP_PLATFORM)

/* ---- Pure (host-tested) ---- */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "minimp3.h"
#include "muse_gadget_sound.h"

#define PLAY_RATE 16000                                     /* the speaker's, MUSE_AUDIO_RATE */
#define MP3S_BUF 4096                                       /* MP3 bytes waiting to decode */
#define MP3S_HOLD (1441 + 4)                                /* as the reply's: the largest frame and the next header */
#define MP3S_OUT_MAX (MINIMP3_MAX_SAMPLES_PER_FRAME + 8)    /* PCM frames one MP3 frame can make */
#define SPEECH_CHARS_PER_S 14                               /* as the reply's, until the speech's length is known */
#define PLAY_TAU 6.2831853f

/* The reply path's linear resampler (muse_chat_session.cpp), copied so Meta's file stays as it is. */
typedef struct {
    uint32_t step, pos;
    int16_t prev;
} play_resampler_t;

static void resampler_init(play_resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls. out must hold n*out/in + 2. */
static size_t resample(play_resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    /* Position 0 is the previous call's last sample, k is in[k-1]. */
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        out[o++] = (int16_t)(a + (((b - a) * (int32_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= (uint32_t)(n << 16);
    r->prev = in[n - 1];
    return o;
}

/* One pass of a chime's notes, in frames. */
static size_t chime_pass(const muse_chime_t *c)
{
    size_t ms = 0;
    for (int i = 0; i < c->n; i++) {
        ms += (size_t)c->notes[i].ms + c->notes[i].gap_ms;
    }
    return ms * (PLAY_RATE / 1000);
}

static size_t chime_frames(const muse_chime_t *c)
{
    return chime_pass(c) * c->repeat;
}

/*
 * Frames [at, at + n) of chime c, so it can be made a piece at a time. Each
 * note is a soft bell: a 5 ms rise, then a fall to nothing by its end, with a
 * touch of the octave above.
 */
static void chime_render(const muse_chime_t *c, size_t at, int16_t *out, size_t n)
{
    size_t pass = chime_pass(c);
    for (size_t k = 0; k < n; k++) {
        size_t f = (at + k) % pass;
        float s = 0;
        for (int i = 0; i < c->n; i++) {
            size_t len = (size_t)c->notes[i].ms * (PLAY_RATE / 1000);
            size_t gap = (size_t)c->notes[i].gap_ms * (PLAY_RATE / 1000);
            if (f < len) {
                float x = (float)f / (float)len;
                float rise = f < 80 ? (float)f / 80.0f : 1.0f;
                float env = rise * (1.0f - x) * (1.0f - x);
                /* The phase from whole cycles dropped first, so it stays exact. */
                float ph = PLAY_TAU * (float)((c->notes[i].hz * f) % PLAY_RATE) / PLAY_RATE;
                s = (sinf(ph) + 0.25f * sinf(2.0f * ph)) * env;
                break;
            }
            if (f < len + gap) {
                break;   /* the silence after it */
            }
            f -= len + gap;
        }
        out[k] = (int16_t)(s * 9000.0f);
    }
}

/* The host of `url` alone, for logs: its path, query and user info can hold a token. */
static void url_host(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, "/?#");
    const char *at = memchr(p, '@', n);
    if (at) {
        n -= (size_t)(at + 1 - p);
        p = at + 1;
    }
    snprintf(out, cap, "%.*s", (int)n, p);
}

/* The byte of a `len`-byte text being said `played` frames in, of `total` (0: not known yet). */
static size_t speech_pos(uint32_t played, uint32_t total, size_t len)
{
    uint32_t frames = total ? total : (uint32_t)(len * PLAY_RATE / SPEECH_CHARS_PER_S);
    size_t at = frames ? (size_t)((uint64_t)played * len / frames) : 0;
    return at < len ? at : len ? len - 1 : 0;
}

/*
 * MP3 bytes in as they arrive, 16 kHz mono PCM out a frame at a time, by the
 * reply's rule (decode() in muse_chat_session.cpp): minimp3 only takes a frame
 * once it can see the next one's header, so until the stream ends the last
 * MP3S_HOLD bytes wait for more. A leading ID3 tag (album art can be large)
 * is skipped, not decoded.
 */
typedef struct {
    mp3dec_t dec;
    uint8_t in[MP3S_BUF];
    size_t len;
    size_t skip;      /* bytes of ID3 tag still to drop */
    bool checked;     /* looked for the tag */
    bool ended;
    int rate;         /* the MP3's, once a frame has decoded */
    play_resampler_t rs;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} mp3s_t;

static void mp3s_init(mp3s_t *s)
{
    memset(s, 0, sizeof(*s));
    mp3dec_init(&s->dec);
}

/* How many bytes mp3s_push() takes now. */
static size_t mp3s_room(const mp3s_t *s)
{
    return s->ended ? 0 : MP3S_BUF - s->len;
}

/* Adds up to mp3s_room() bytes of `data`; returns how many it took. */
static size_t mp3s_push(mp3s_t *s, const uint8_t *data, size_t n)
{
    size_t room = mp3s_room(s);
    n = n < room ? n : room;
    memcpy(s->in + s->len, data, n);
    s->len += n;
    return n;
}

/* No more bytes are coming: the held ones decode too. */
static void mp3s_end(mp3s_t *s)
{
    s->ended = true;
}

static void mp3s_drop(mp3s_t *s, size_t n)
{
    memmove(s->in, s->in + n, s->len - n);
    s->len -= n;
}

/*
 * The next frame's PCM into out (MP3S_OUT_MAX frames): how many it wrote. 0
 * means it needs more bytes, or, with *done set, that everything pushed
 * before mp3s_end() has been decoded.
 */
static size_t mp3s_pull(mp3s_t *s, int16_t *out, bool *done)
{
    *done = false;
    if (!s->checked && (s->len >= 10 || s->ended)) {
        s->checked = true;
        if (s->len >= 10 && !memcmp(s->in, "ID3", 3)) {
            s->skip = 10 + ((size_t)(s->in[6] & 0x7f) << 21 | (size_t)(s->in[7] & 0x7f) << 14 |
                            (size_t)(s->in[8] & 0x7f) << 7 | (size_t)(s->in[9] & 0x7f)) +
                      (s->in[5] & 0x10 ? 10 : 0);   /* a footer */
        }
    }
    if (s->skip) {
        size_t n = s->skip < s->len ? s->skip : s->len;
        mp3s_drop(s, n);
        s->skip -= n;
    }
    size_t hold = s->ended ? 0 : MP3S_HOLD;
    while (s->checked && !s->skip && s->len > hold) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&s->dec, s->in, (int)s->len, s->pcm, &info);
        if (!info.frame_bytes) {
            if (s->ended || s->len == MP3S_BUF) {
                s->len = 0;   /* junk at the end, or a buffer full of it */
            }
            break;
        }
        mp3s_drop(s, (size_t)info.frame_bytes);
        if (!samples) {
            continue;   /* skipped junk */
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                s->pcm[k] = (int16_t)((s->pcm[2 * k] + s->pcm[2 * k + 1]) / 2);
            }
        }
        if (s->rate != info.hz) {
            s->rate = info.hz;
            resampler_init(&s->rs, info.hz, PLAY_RATE);
        }
        return resample(&s->rs, s->pcm, (size_t)samples, out);
    }
    *done = s->ended && !s->len;
    return 0;
}

/* ---- Device ---- */

#endif   /* CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH */
