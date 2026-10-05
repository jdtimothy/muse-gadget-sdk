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

#include <stdatomic.h>
#include <stdlib.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_chat_priv.h"   /* muse_hatch_caption_at */
#include "muse_gadget_play.h"
#include "muse_input.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_tts_elevenlabs.h"

_Static_assert(PLAY_RATE == MUSE_AUDIO_RATE, "the player makes the speaker's rate");

static const char *TAG = "muse_play";

#define PCM_BYTES (PLAY_RATE * 2 * 2)       /* 2 s between the player and the speaker */
#define CLIP_MAX_BYTES (1024 * 1024)
#define CLIP_MAX_FRAMES (PLAY_RATE * 60)
#define CLIP_TIMEOUT_MS 15000
#define CLIP_REDIRECTS 3
#define TTS_START_MS 3000                   /* a reply's last fetch may still be stopping */
#define NOTICE_MS 3000                      /* a caption shown in place of a sound stays this long */

typedef struct {
    muse_sound_kind_t kind;
    bool caption;
    char arg[];   /* the text, URL or chime name */
} job_t;

static QueueHandle_t s_jobs;            /* job_t *, oldest first */
static StreamBufferHandle_t s_pcm;      /* PLAY_RATE mono, from the player to the speaker */
static TaskHandle_t s_player;
static job_t *s_job;                    /* the player's; the voice task frees it once the player is done */
static atomic_bool s_stop;              /* the speaker side gave up on s_job */
static atomic_bool s_made;              /* the player is done with s_job */
static atomic_bool s_failed;            /* ... and made nothing worth playing */
static atomic_uint s_total;             /* ... and it came to this many frames */
static mp3s_t *s_mp3;
static int16_t *s_out;                  /* MP3S_OUT_MAX */

static const char *kind_name(muse_sound_kind_t kind)
{
    return kind == MUSE_SOUND_SAY ? "speech" : kind == MUSE_SOUND_URL ? "clip" : "chime";
}

/* ---- Player task: a job into PCM ---- */

/* Hands PCM to the speaker side, waiting for room. False once told to stop. */
static bool send_pcm(const int16_t *pcm, size_t n)
{
    size_t bytes = n * sizeof(int16_t), sent = 0;
    while (sent < bytes) {
        if (atomic_load(&s_stop)) {
            return false;
        }
        sent += xStreamBufferSend(s_pcm, (const uint8_t *)pcm + sent, bytes - sent, pdMS_TO_TICKS(100));
    }
    return true;
}

/* Sends what s_mp3 can decode now. False once told to stop, or at `limit` frames. */
static bool drain_mp3(uint32_t *frames, uint32_t limit)
{
    bool done;
    size_t n;
    while ((n = mp3s_pull(s_mp3, s_out, &done))) {
        if (n > limit - *frames) {
            n = limit - *frames;
        }
        if (!send_pcm(s_out, n)) {
            return false;
        }
        *frames += (uint32_t)n;
        if (*frames >= limit) {
            return false;
        }
    }
    return true;
}

static bool make_chime(const char *name, uint32_t *frames)
{
    int i = muse_chime_find(name);
    if (i < 0) {
        return false;
    }
    const muse_chime_t *c = &MUSE_CHIMES[i];
    size_t total = chime_frames(c);
    for (size_t at = 0; at < total; at += MP3S_OUT_MAX) {
        size_t n = total - at < MP3S_OUT_MAX ? total - at : MP3S_OUT_MAX;
        chime_render(c, at, s_out, n);
        if (!send_pcm(s_out, n)) {
            break;
        }
        *frames += (uint32_t)n;
    }
    return true;
}

/* Speech through the reply path's ElevenLabs fetch, in the voice replies use. */
static bool make_speech(const char *text, uint32_t *frames)
{
    uint32_t job;
    for (int waited = 0; !(job = muse_tts_start(text, strlen(text))); waited += 50) {
        if (atomic_load(&s_stop)) {
            return true;
        }
        if (waited >= TTS_START_MS || !muse_tts_enabled()) {
            ESP_LOGW(TAG, "speech: ElevenLabs is off or busy");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    mp3s_init(s_mp3);
    uint8_t chunk[512];
    for (;;) {
        if (atomic_load(&s_stop)) {
            muse_tts_cancel();
            return true;
        }
        bool ended = false, ok = false;
        size_t room = mp3s_room(s_mp3);
        size_t n = room ? muse_tts_read(job, chunk, room < sizeof(chunk) ? room : sizeof(chunk), &ended, &ok) : 0;
        mp3s_push(s_mp3, chunk, n);
        if (ended) {
            mp3s_end(s_mp3);
            drain_mp3(frames, UINT32_MAX);
            return ok || *frames > 0;   /* what came is worth playing */
        }
        if (!drain_mp3(frames, UINT32_MAX)) {
            muse_tts_cancel();
            return true;
        }
        if (!n) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

/* Opens the clip, following up to CLIP_REDIRECTS redirects that stay on https. */
static esp_err_t open_clip(esp_http_client_handle_t c, int *status)
{
    for (int i = 0;; i++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            return err;
        }
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        bool redirect = *status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308;
        if (!redirect || i == CLIP_REDIRECTS) {
            return ESP_OK;
        }
        err = esp_http_client_set_redirection(c);
        esp_http_client_close(c);
        if (err != ESP_OK) {
            return err;
        }
        char scheme[16];   /* get_url truncates; the scheme is all that's needed */
        if (esp_http_client_get_url(c, scheme, sizeof(scheme)) != ESP_OK || strncasecmp(scheme, "https://", 8)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
}

static bool stream_clip(esp_http_client_handle_t c, const char *host, uint32_t *frames)
{
    int64_t length = esp_http_client_get_content_length(c);
    if (length > CLIP_MAX_BYTES) {
        ESP_LOGW(TAG, "clip from %s: %lld bytes, over 1 MB", host, (long long)length);
        return false;
    }
    mp3s_init(s_mp3);
    uint8_t chunk[1024];
    size_t total = 0;
    for (bool more = true; more;) {
        size_t room = mp3s_room(s_mp3);
        if (room) {
            int n = esp_http_client_read(c, (char *)chunk, (int)(room < sizeof(chunk) ? room : sizeof(chunk)));
            if (n > 0 && total + (size_t)n > CLIP_MAX_BYTES) {
                ESP_LOGW(TAG, "clip from %s: over 1 MB, stopped there", host);
                n = 0;
            } else if (n < 0) {
                ESP_LOGW(TAG, "clip from %s: cut off after %u bytes", host, (unsigned)total);
            }
            if (n <= 0) {
                mp3s_end(s_mp3);
                more = false;
            } else {
                total += (size_t)n;
                mp3s_push(s_mp3, chunk, (size_t)n);
            }
        }
        if (!drain_mp3(frames, CLIP_MAX_FRAMES)) {
            break;   /* stopped, or 60 s played */
        }
    }
    if (!*frames && !atomic_load(&s_stop)) {
        ESP_LOGW(TAG, "clip from %s: not an MP3 (%u bytes)", host, (unsigned)total);
    }
    return *frames > 0 || atomic_load(&s_stop);
}

static bool make_clip(const char *url, uint32_t *frames)
{
    char host[64];
    url_host(url, host, sizeof(host));
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CLIP_TIMEOUT_MS,
        .buffer_size = 2048,   /* whole header lines must fit */
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "clip from %s: no memory", host);
        return false;
    }
    int status = 0;
    esp_err_t err = open_clip(c, &status);
    bool ok = false;
    if (err == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "clip from %s: redirected off https", host);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "clip from %s: %s", host, esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGW(TAG, "clip from %s: HTTP %d", host, status);
    } else {
        ok = stream_clip(c, host, frames);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        job_t *job = s_job;
        uint32_t frames = 0;
        bool ok = job->kind == MUSE_SOUND_CHIME ? make_chime(job->arg, &frames)
                  : job->kind == MUSE_SOUND_URL ? make_clip(job->arg, &frames)
                                                : make_speech(job->arg, &frames);
        atomic_store(&s_total, frames);
        atomic_store(&s_failed, !ok);
        atomic_store(&s_made, true);
    }
}

/* ---- Voice task side: playing ---- */

/* A talk press among the queued input: true, taking it, if one came. Input
 * after it stays queued for the recording. */
static bool pressed(QueueHandle_t presses)
{
    muse_input_event_t ev;
    while (xQueueReceive(presses, &ev, 0) == pdTRUE) {
        muse_state_poke();
        if (ev.type == MUSE_PTT_DOWN) {
            return true;
        }
    }
    return false;
}

/* Waits `ms`, or less if a talk press comes: true then. */
static bool hold(QueueHandle_t presses, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        if (pressed(presses)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

/* The caption: the page of `text` being said, as a reply's. */
static void show_page(const char *text, uint32_t played, uint32_t total)
{
    static char page[MUSE_CAPTION_MAX];
    if (muse_hatch_caption_at(text, speech_pos(played, total, strlen(text)), page, sizeof(page))) {
        muse_state_set_caption("%s", page);
    }
}

/* Waits for the player to finish the last job (a clip's read can take a while
 * to give up), frees it and drops its PCM. True if a talk press came first. */
static bool player_idle(QueueHandle_t presses)
{
    while (s_job && !atomic_load(&s_made)) {
        if (hold(presses, 20)) {
            return true;
        }
    }
    free(s_job);
    s_job = NULL;
    static uint8_t junk[256];
    while (xStreamBufferReceive(s_pcm, junk, sizeof(junk), 0)) {
    }
    return false;
}

/* Plays one job, taking it. True if a talk press cut it off. */
static bool play_one(job_t *job, QueueHandle_t presses)
{
    bool say = job->kind == MUSE_SOUND_SAY;
    bool shown = say && job->caption;
    muse_state_set_asleep(false);   /* to be seen; it sleeps again on its own timer */
    if (!muse_settings_speaker_on()) {
        /* Muted: speech is shown, not said; other sounds are dropped. */
        if (shown) {
            show_page(job->arg, 0, 0);
        }
        free(job);
        return shown && hold(presses, NOTICE_MS);
    }
    if (player_idle(presses)) {
        free(job);
        return true;
    }
    int chime = job->kind == MUSE_SOUND_CHIME ? muse_chime_find(job->arg) : -1;
    if (chime >= 0) {
        ESP_LOGI(TAG, "chime: %s", MUSE_CHIMES[chime].name);   /* the table's name, never the request's */
    } else {
        ESP_LOGI(TAG, "%s: starting", kind_name(job->kind));
    }
    atomic_store(&s_stop, false);
    atomic_store(&s_failed, false);
    atomic_store(&s_total, 0);
    atomic_store(&s_made, false);
    s_job = job;   /* the player's now; player_idle() frees it */
    xTaskNotifyGive(s_player);

    muse_state_set_mode(MUSE_MODE_THINKING);   /* until the sound starts */
    if (shown) {
        show_page(job->arg, 0, 0);
    }
    static int16_t buf[MUSE_AUDIO_CHUNK];
    static const int16_t silence[MUSE_AUDIO_CHUNK];
    uint32_t played = 0;
    bool speaking = false, cut = false;
    for (;;) {
        if (pressed(presses)) {
            cut = true;
            break;
        }
        bool made = atomic_load(&s_made);   /* before the read: then an empty buffer means all of it played */
        size_t n = xStreamBufferReceive(s_pcm, buf, sizeof(buf), pdMS_TO_TICKS(speaking ? 0 : 20)) / sizeof(int16_t);
        if (n) {
            if (!speaking) {
                speaking = true;
                muse_state_set_mode(MUSE_MODE_SPEAKING);
            }
            muse_state_set_level(muse_audio_level(buf, n));
            muse_audio_write(buf, n);
            played += (uint32_t)n;
        } else if (made) {
            break;
        } else if (speaking) {
            /* The player is behind: keep the speaker fed so it doesn't replay stale DMA (as hatch_reply). */
            muse_state_set_level(0);
            muse_audio_write(silence, MUSE_AUDIO_CHUNK);
        }
        if (shown && speaking) {
            show_page(job->arg, played, made ? atomic_load(&s_total) : 0);
        }
    }
    muse_state_set_level(0);
    if (cut) {
        atomic_store(&s_stop, true);
        if (say) {
            muse_tts_cancel();
        }
        ESP_LOGI(TAG, "%s cut off by a talk press", kind_name(job->kind));
        return true;
    }
    ESP_LOGI(TAG, "%s: %.1f s played", kind_name(job->kind), (double)played / PLAY_RATE);
    if (!atomic_load(&s_failed)) {
        return false;
    }
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", say ? "COULDN'T SPEAK" : "COULDN'T PLAY THE CLIP");
    if (!shown) {
        return hold(presses, NOTICE_MS);
    }
    if (hold(presses, NOTICE_MS / 2)) {
        return true;
    }
    show_page(job->arg, 0, 0);
    return hold(presses, NOTICE_MS);
}

/* ---- Public ---- */

static bool play_init(void)
{
    static atomic_int state;   /* 0 not yet, 1 starting, 2 ready, 3 failed */
    int expect = 0;
    if (atomic_compare_exchange_strong(&state, &expect, 1)) {
        s_jobs = xQueueCreateWithCaps(MUSE_SOUND_QUEUE, sizeof(job_t *), MALLOC_CAP_SPIRAM);
        s_pcm = xStreamBufferCreateWithCaps(PCM_BYTES, 1, MALLOC_CAP_SPIRAM);
        s_mp3 = heap_caps_calloc(1, sizeof(*s_mp3), MALLOC_CAP_SPIRAM);
        s_out = heap_caps_malloc(MP3S_OUT_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        /* Stack in PSRAM, as the voice session's: TLS and minimp3 (~16 KB of scratch) run here. */
        bool ok = s_jobs && s_pcm && s_mp3 && s_out &&
                  xTaskCreatePinnedToCoreWithCaps(player_task, "gadget_play", 32 * 1024, NULL, 4, &s_player, 0,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        if (!ok) {
            ESP_LOGE(TAG, "the sound player failed to start");
        }
        atomic_store(&state, ok ? 2 : 3);
    }
    while (atomic_load(&state) == 1) {
        vTaskDelay(1);
    }
    return atomic_load(&state) == 2;
}

int muse_play_enqueue(muse_sound_kind_t kind, const char *arg, bool caption)
{
    if (!play_init()) {
        return -1;
    }
    size_t len = strlen(arg);
    job_t *job = heap_caps_malloc(sizeof(*job) + len + 1, MALLOC_CAP_SPIRAM);
    if (!job) {
        return -1;
    }
    job->kind = kind;
    job->caption = caption;
    memcpy(job->arg, arg, len + 1);
    /* Counted first: once sent, the voice task may take it before a count after. */
    int ahead = (int)uxQueueMessagesWaiting(s_jobs);
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        free(job);
        return 0;
    }
    muse_state_nudge();   /* out of the voice task's resting wait */
    return ahead + 1;
}

bool muse_play_pending(void)
{
    return s_jobs && uxQueueMessagesWaiting(s_jobs) > 0;
}

bool muse_play_run(QueueHandle_t presses)
{
    job_t *job;
    while (s_jobs && xQueueReceive(s_jobs, &job, 0) == pdTRUE) {
        if (play_one(job, presses)) {
            while (xQueueReceive(s_jobs, &job, 0) == pdTRUE) {
                free(job);
            }
            ESP_LOGI(TAG, "talk press: queued sounds dropped");
            return true;
        }
    }
    muse_state_set_mode(MUSE_MODE_IDLE);
    muse_state_set_caption("%s", "");
    return false;
}

#endif   /* CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH */
