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

#include "sdkconfig.h"

#if CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH
#include "muse_gadget_art.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"

#include "muse_now_playing.h"
#include "muse_tts_elevenlabs.h"   /* muse_tts_google_roots() */

static const char *TAG = "muse_art";

#define ART_MAX_BYTES (1024 * 1024)
#define ART_DEADLINE_US (20LL * 1000000)
#define ART_TIMEOUT_MS 10000              /* per socket operation */
#define ART_REDIRECTS 3
#define ART_SIDE_MAX 4096                 /* decoded, before fitting (muse_np_art_fit's limit) */
#define ART_SRC_MAX_PX (1024 * 1024)
#define JPEG_POOL_BYTES 3100              /* the ROM decoder's work pool, as main/image_fetch.c */

static int s_w, s_h;
static uint16_t *s_buf[2];
static TaskHandle_t s_task;
static atomic_uint s_gen;                 /* bumped by each muse_art_fetch() */

/* Under s_lock: the wanted cover, and which buffer is which. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char *s_url;                       /* waiting for the task, or NULL */
static const char *s_cur;                 /* the one the task is fetching, or NULL */
static char *s_loaded;                    /* the last one decoded, or NULL */
static int s_front = -1;                  /* the buffer the tile shows, or -1 */
static int s_ready = -1;                  /* a finished buffer not yet taken, or -1 */

typedef struct {
    esp_http_client_handle_t http;
    unsigned gen;
    int64_t deadline_us;
    size_t bytes;
    bool too_big;
    uint16_t *src;                        /* the decoded cover, dw x dh */
    int dw, dh;
} art_t;

static bool current(const art_t *a)
{
    return atomic_load(&s_gen) == a->gen && esp_timer_get_time() < a->deadline_us;
}

/* The host of a URL, for logs: never the URL itself. */
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

/* The decoder's input: the response body, or skipped bytes when buf is NULL. */
static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT len)
{
    art_t *a = jd->device;
    uint8_t skip[64];
    UINT done = 0;
    while (done < len && current(a)) {
        UINT want = len - done;
        if (!buf && want > sizeof(skip)) {
            want = sizeof(skip);
        }
        int n = esp_http_client_read(a->http, (char *)(buf ? buf + done : skip), (int)want);
        if (n <= 0) {
            break;
        }
        a->bytes += (size_t)n;
        if (a->bytes > ART_MAX_BYTES) {
            a->too_big = true;
            return 0;
        }
        done += (UINT)n;
    }
    return done;
}

/* The decoder's output: an MCU of RGB888 into the decoded cover, as RGB565. */
static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    art_t *a = jd->device;
    if (!current(a)) {
        return 0;   /* replaced or too slow: stop */
    }
    const uint8_t *rgb = bitmap;
    int w = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int i = 0; i < w; i++, rgb += 3) {
            int x = rect->left + i;
            if (x < a->dw && y < a->dh) {
                a->src[(size_t)y * a->dw + x] = muse_np_rgb565(rgb[0], rgb[1], rgb[2]);
            }
        }
    }
    return 1;
}

/* Fits and dims the decoded cover into dst, w x h. */
static void fit(const art_t *a, uint16_t *dst)
{
    muse_np_fit_t f = muse_np_art_fit(a->dw, a->dh, s_w, s_h);
    for (int y = 0; y < s_h; y++) {
        int dim = muse_np_dim(y, s_h);
        const uint16_t *row = a->src + (size_t)((f.oy + (uint32_t)y * f.step) >> 16) * a->dw;
        uint16_t *out = dst + (size_t)y * s_w;
        for (int x = 0; x < s_w; x++) {
            out[x] = muse_np_dim565(row[(f.ox + (uint32_t)x * f.step) >> 16], dim);
        }
    }
}

static void decode(esp_http_client_handle_t c, const char *url, const char *host, unsigned gen)
{
    art_t a = { .http = c, .gen = gen, .deadline_us = esp_timer_get_time() + ART_DEADLINE_US };
    void *pool = malloc(JPEG_POOL_BYTES);
    if (!pool) {
        ESP_LOGW(TAG, "art from %s: no memory", host);
        return;
    }
    const char *why = NULL;
    int s = 0;
    JDEC jd;
    JRESULT rc = jd_prepare(&jd, jpeg_in, pool, JPEG_POOL_BYTES, &a);
    if (rc == JDR_OK) {
        s = muse_np_art_scale(jd.width, jd.height, s_w, s_h);
        a.dw = (jd.width + (1 << s) - 1) >> s;
        a.dh = (jd.height + (1 << s) - 1) >> s;
        if (a.dw > ART_SIDE_MAX || a.dh > ART_SIDE_MAX || (size_t)a.dw * a.dh > ART_SRC_MAX_PX) {
            why = "too large";
        } else if (!(a.src = heap_caps_calloc((size_t)a.dw * a.dh, sizeof(uint16_t), MALLOC_CAP_SPIRAM))) {
            why = "no memory";
        } else {
            rc = jd_decomp(&jd, jpeg_out, (uint8_t)s);
        }
    }
    if (!why && rc != JDR_OK) {
        if (atomic_load(&s_gen) != gen) {
            free(a.src);
            free(pool);
            return;   /* replaced: the newer one logs */
        }
        why = a.too_big                                  ? "over 1 MB"
              : esp_timer_get_time() >= a.deadline_us    ? "took over 20 s"
              : rc == JDR_INP                            ? "download cut off"
              : rc == JDR_FMT3                           ? "unsupported JPEG: use baseline, not progressive"
                                                         : "not a JPEG";
    }
    if (why) {
        ESP_LOGW(TAG, "art from %s: %s", host, why);
    } else {
        taskENTER_CRITICAL(&s_lock);
        int back = s_front == 0 ? 1 : 0;
        s_ready = -1;   /* a cover not yet taken may be in `back`: it's dropped */
        taskEXIT_CRITICAL(&s_lock);
        fit(&a, s_buf[back]);
        char *loaded = strdup(url), *old = NULL;
        taskENTER_CRITICAL(&s_lock);
        if (atomic_load(&s_gen) == gen) {
            s_ready = back;
            old = s_loaded;
            s_loaded = loaded;
            loaded = NULL;
        }
        taskEXIT_CRITICAL(&s_lock);
        free(old);
        free(loaded);
        ESP_LOGI(TAG, "art from %s: %dx%d at 1/%d, %u bytes", host, (int)jd.width, (int)jd.height, 1 << s,
                 (unsigned)a.bytes);
    }
    free(a.src);
    free(pool);
}

/* Opens the cover, following up to ART_REDIRECTS redirects on the same scheme. */
static esp_err_t open_art(esp_http_client_handle_t c, bool https, int *status)
{
    for (int i = 0;; i++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            return err;
        }
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        bool redirect = *status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308;
        if (!redirect || i == ART_REDIRECTS) {
            return ESP_OK;
        }
        err = esp_http_client_set_redirection(c);
        esp_http_client_close(c);
        if (err != ESP_OK) {
            return err;
        }
        char scheme[16];   /* get_url truncates; the scheme is all that's needed */
        if (esp_http_client_get_url(c, scheme, sizeof(scheme)) != ESP_OK ||
            (strncasecmp(scheme, "https://", 8) == 0) != https) {
            return ESP_ERR_INVALID_ARG;   /* no change of scheme */
        }
    }
}

/* Connects, checking an https server against IDF's bundle, or Google Trust
 * Services' roots alone if `google` is set. */
static esp_http_client_handle_t connect_art(const char *url, bool https, bool google, int *status, esp_err_t *err)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = ART_TIMEOUT_MS,
        .buffer_size = 2048,   /* whole header lines must fit */
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    if (https && google) {
        cfg.cert_pem = muse_tts_google_roots();
    } else if (https) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    *err = c ? open_art(c, https, status) : ESP_ERR_NO_MEM;
    return c;
}

static void fetch(const char *url, unsigned gen)
{
    char host[64];
    url_host(url, host, sizeof(host));
    bool https = !strncasecmp(url, "https://", 8);
    int status = 0;
    esp_err_t err;
    esp_http_client_handle_t c = connect_art(url, https, false, &status, &err);
    if (https && err == ESP_ERR_HTTP_CONNECT && atomic_load(&s_gen) == gen) {
        /* Hosts behind Google's front end send GTS Root R1 cross-signed by
         * the retired GlobalSign root, which the bundle refuses (as clips). */
        ESP_LOGI(TAG, "art from %s: trying Google's roots", host);
        esp_http_client_cleanup(c);
        c = connect_art(url, true, true, &status, &err);
    }
    if (!c) {
        ESP_LOGW(TAG, "art from %s: no memory", host);
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "art from %s: redirected to another scheme", host);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "art from %s: %s", host, esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGW(TAG, "art from %s: HTTP %d", host, status);
    } else if (esp_http_client_get_content_length(c) > ART_MAX_BYTES) {
        ESP_LOGW(TAG, "art from %s: over 1 MB", host);
    } else {
        decode(c, url, host, gen);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

static void art_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            taskENTER_CRITICAL(&s_lock);
            unsigned gen = atomic_load(&s_gen);
            char *url = s_url;
            s_url = NULL;
            s_cur = url;
            taskEXIT_CRITICAL(&s_lock);
            if (!url) {
                break;
            }
            fetch(url, gen);
            taskENTER_CRITICAL(&s_lock);
            s_cur = NULL;   /* done: decoded (s_loaded) or failed, and then fetched again if asked */
            taskEXIT_CRITICAL(&s_lock);
            free(url);
        }
    }
}

bool muse_art_init(int w, int h)
{
    if (s_task) {
        return true;
    }
    s_w = w;
    s_h = h;
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    /* Stack in PSRAM, as the sound player's: TLS runs on it for https. */
    if (!s_buf[0] || !s_buf[1] ||
        xTaskCreatePinnedToCoreWithCaps(art_task, "muse_art", 16 * 1024, NULL, 2, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no memory for cover art");
        heap_caps_free(s_buf[0]);
        heap_caps_free(s_buf[1]);
        s_buf[0] = s_buf[1] = NULL;
        s_task = NULL;
        return false;
    }
    return true;
}

void muse_art_fetch(const char *url)
{
    char *copy = url ? strdup(url) : NULL;
    if (url && !copy) {
        ESP_LOGW(TAG, "no memory for the cover's address");
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    char *old = s_url, *loaded = NULL;
    s_url = copy;
    s_ready = -1;   /* a finished cover not yet shown is out of date */
    if (!copy) {
        loaded = s_loaded;   /* cleared: the same cover again is a new fetch */
        s_loaded = NULL;
    }
    atomic_fetch_add(&s_gen, 1);
    taskEXIT_CRITICAL(&s_lock);
    free(old);
    free(loaded);
    if (s_task && copy) {
        xTaskNotifyGive(s_task);
    }
}

bool muse_art_has(const char *url)
{
    taskENTER_CRITICAL(&s_lock);
    bool has = (s_url && !strcmp(s_url, url)) || (s_cur && !strcmp(s_cur, url)) ||
               (s_loaded && !strcmp(s_loaded, url));
    taskEXIT_CRITICAL(&s_lock);
    return has;
}

const uint16_t *muse_art_take(void)
{
    const uint16_t *buf = NULL;
    taskENTER_CRITICAL(&s_lock);
    if (s_ready >= 0) {
        s_front = s_ready;
        s_ready = -1;
        buf = s_buf[s_front];
    }
    taskEXIT_CRITICAL(&s_lock);
    return buf;
}

#endif   /* CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH */
