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
 * Spoken replies through ElevenLabs' streaming text-to-speech
 * (POST /v1/text-to-speech/{voice}/stream). The request runs on a task of its
 * own so a slow answer never holds up the voice session; its MP3 reaches the
 * session through a stream buffer in PSRAM, which that task alone writes and
 * the session alone reads. TLS buffers go to PSRAM too
 * (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC), so a second connection leaves Wi-Fi and
 * BLE their internal RAM.
 *
 * The API key is never logged: only whether it's set and the voice.
 */
#include "muse_tts_elevenlabs.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_MUSE_TTS_ELEVENLABS_API_KEY   /* the Voice PE, which has no Muse menu */
#define CONFIG_MUSE_TTS_ELEVENLABS_API_KEY ""
#define CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID ""
#endif

static const char *TAG = "muse_tts";

#define TTS_URL "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=%s"
#define TTS_MODEL "eleven_flash_v2_5"     /* the quickest to start speaking */
#define TTS_FORMAT "mp3_22050_32"          /* light on Wi-Fi; decode() resamples it to 16 kHz */
#define TEXT_CAP 1024                      /* the session keeps a message's text in TEXT_MAX (1024) */
#define BUF_BYTES (32 * 1024)              /* ~8 s of speech between the task and the session */
#define CHUNK 1024                         /* ~0.25 s: each read waits for this much */
#define SEND_WAIT_MS 100                   /* how often a full buffer checks for a cancel */
#define TIMEOUT_MS 15000
#define ERR_BODY 200                       /* how much of an error reply to log */

/*
 * The roots api.elevenlabs.io chains to: Google Trust Services' GTS Root R1
 * (RSA, the current chain: elevenlabs.io < WR3 < R1) and R4 (ECDSA), from
 * ESP-IDF's own CA list (esp_crt_bundle/cacrt_all.pem). Not the bundle: the
 * server also sends R1 cross-signed by GlobalSign Root CA, which IDF v6 has
 * retired, and the bundle's check looks only for the top certificate's issuer,
 * so it fails. Given R1 itself, mbedTLS stops at it as WR3's parent.
 */
static const char GTS_ROOTS_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "gm3c\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYDVQQGEwJVUzEi\n"
    "MCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQw\n"
    "HhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZ\n"
    "R29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjO\n"
    "PQIBBgUrgQQAIgNiAATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzu\n"
    "hXyiQHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvRHYqjQjBA\n"
    "MA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBSATNbrdP9JNqPV2Py1\n"
    "PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/C\n"
    "r8deVl5c1RxYIigL9zC2L7F8AjEA8GE8p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh\n"
    "4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

static StreamBufferHandle_t s_buf;
static TaskHandle_t s_task;
static char *s_text;          /* TEXT_CAP: the text being fetched */
static char *s_chunk;         /* CHUNK */
static uint32_t s_next = 1;   /* the session's alone */
static atomic_uint s_want;    /* the fetch asked for; any other value stops it */
static atomic_uint s_ready;   /* the fetch whose MP3 s_buf holds */
static atomic_uint s_ended;   /* the last fetch to finish */
static atomic_bool s_ok;      /* whether all of s_ended's MP3 arrived */
static atomic_bool s_busy;    /* the task has a fetch, or one is on its way to it */

/* Sends the request and streams the reply into s_buf. True if all of it arrived. */
static bool request(esp_http_client_handle_t c, const char *body, uint32_t job)
{
    int len = (int)strlen(body);
    esp_err_t err = esp_http_client_open(c, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't reach ElevenLabs: %s", esp_err_to_name(err));
        return false;
    }
    if (esp_http_client_write(c, body, len) != len || esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "ElevenLabs request failed");
        return false;
    }
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        int n = esp_http_client_read(c, s_chunk, ERR_BODY);
        ESP_LOGW(TAG, "ElevenLabs said %d: %.*s", status, n > 0 ? n : 0, s_chunk);
        return false;
    }
    size_t total = 0;
    for (;;) {
        if (atomic_load(&s_want) != job) {
            ESP_LOGI(TAG, "stopped after %u bytes", (unsigned)total);
            return false;
        }
        int n = esp_http_client_read(c, s_chunk, CHUNK);
        if (n < 0) {
            ESP_LOGW(TAG, "speech cut off after %u bytes", (unsigned)total);
            return false;
        }
        if (n == 0) {
            bool whole = esp_http_client_is_complete_data_received(c);
            ESP_LOGI(TAG, "speech %s: %u bytes", whole ? "in" : "cut short", (unsigned)total);
            return whole;
        }
        for (int sent = 0; sent < n;) {
            if (atomic_load(&s_want) != job) {
                return false;
            }
            sent += (int)xStreamBufferSend(s_buf, s_chunk + sent, n - sent, pdMS_TO_TICKS(SEND_WAIT_MS));
        }
        total += n;
    }
}

static bool fetch(uint32_t job)
{
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return false;
    }
    cJSON_AddStringToObject(req, "text", s_text);
    cJSON_AddStringToObject(req, "model_id", TTS_MODEL);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return false;
    }
    char url[192];
    snprintf(url, sizeof(url), TTS_URL, CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID, TTS_FORMAT);
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .cert_pem = GTS_ROOTS_PEM,
        .buffer_size = 2048,              /* whole header lines must fit */
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        esp_http_client_set_header(c, "xi-api-key", CONFIG_MUSE_TTS_ELEVENLABS_API_KEY);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_header(c, "Accept", "audio/mpeg");
        int64_t t0 = esp_timer_get_time();
        ok = request(c, body, job);
        ESP_LOGI(TAG, "fetch took %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    cJSON_free(body);
    return ok;
}

static void tts_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t job = atomic_load(&s_want);
        if (job) {
            /* Safe here: the session reads without waiting, so no task blocks on s_buf. */
            xStreamBufferReset(s_buf);
            atomic_store(&s_ready, job);
            atomic_store(&s_ok, fetch(job));
            atomic_store(&s_ended, job);
        }
        atomic_store(&s_busy, false);
    }
}

bool muse_tts_init(void)
{
    if (s_task) {
        return true;
    }
    if (!CONFIG_MUSE_TTS_ELEVENLABS_API_KEY[0] || !CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID[0]) {
        ESP_LOGI(TAG, "ElevenLabs TTS off: no API key or voice");
        return false;
    }
    s_buf = xStreamBufferCreateWithCaps(BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_text = heap_caps_malloc(TEXT_CAP, MALLOC_CAP_SPIRAM);
    s_chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    /* Stack in PSRAM, as the voice session's: TLS runs here. */
    if (!s_buf || !s_text || !s_chunk ||
        xTaskCreateWithCaps(tts_task, "muse_tts", 12 * 1024, NULL, 4, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "ElevenLabs TTS failed to start");
        s_task = NULL;
        return false;
    }
    ESP_LOGI(TAG, "ElevenLabs TTS on, voice %s", CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID);
    return true;
}

bool muse_tts_enabled(void)
{
    return s_task != NULL;
}

/* Drops a UTF-8 sequence cut short at the end, which ElevenLabs would refuse. */
static size_t utf8_whole(const char *s, size_t len)
{
    size_t i = len;
    while (i && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && len - i < 3) {
        i--;
    }
    if (!i || ((unsigned char)s[i - 1] & 0x80) == 0) {
        return len;   /* plain ASCII at the end */
    }
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return len - (i - 1) >= need ? len : i - 1;
}

uint32_t muse_tts_start(const char *text, size_t len)
{
    if (!s_task || atomic_load(&s_busy)) {
        return 0;
    }
    if (len > TEXT_CAP - 1) {
        len = TEXT_CAP - 1;
    }
    len = utf8_whole(text, len);
    memcpy(s_text, text, len);
    s_text[len] = '\0';
    uint32_t job = s_next++;
    if (!s_next) {
        s_next = 1;
    }
    atomic_store(&s_busy, true);
    atomic_store(&s_want, job);
    xTaskNotifyGive(s_task);
    return job;
}

size_t muse_tts_read(uint32_t job, uint8_t *dst, size_t cap, bool *ended, bool *ok)
{
    *ended = false;
    if (!job || atomic_load(&s_ready) != job) {
        return 0;   /* not started yet */
    }
    /* Ended before the read: then an empty read means all of it has been taken. */
    bool fin = atomic_load(&s_ended) == job;
    size_t n = xStreamBufferReceive(s_buf, dst, cap, 0);
    if (!n && fin) {
        *ended = true;
        *ok = atomic_load(&s_ok);
    }
    return n;
}

void muse_tts_cancel(void)
{
    atomic_store(&s_want, 0);
}
