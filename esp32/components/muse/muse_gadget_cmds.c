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
 * Commands this gadget offers Muse beyond Home Link's own, advertised in
 * link.register and answered from on_ws_command() (main/app.c):
 *   device.settings  read or change volume, mute, brightness and screen
 *                    sleep; reports them with the battery and voice
 *   voice.select     list the ElevenLabs voices, or switch to one (async)
 * skills/gadget-muse-s318/SKILL.md tells Muse when to use each.
 *
 * Everything from "Pure (host-tested)" to "Device" builds on the host
 * against cJSON alone: tests/test_muse_gadget_cmds.py.
 */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
/* Always in the build (components/muse/CMakeLists.txt): nothing without the option. */
#if CONFIG_MUSE_GADGET_COMMANDS || !defined(ESP_PLATFORM)
#include "muse_gadget_cmds.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "muse_gadget_sound.h"

/* ---- Pure (host-tested) ---- */

static cJSON *gadget_error(const char *code, const char *message)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

static cJSON *gadget_ok(cJSON **payload)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    *payload = cJSON_AddObjectToObject(result, "payload");
    return result;
}

/* Cuts a UTF-8 sequence left unfinished at the end of s by a byte limit: Muse
 * reads results as JSON, which must be whole UTF-8. */
static void utf8_trim(char *s)
{
    size_t len = strlen(s), i = len;
    while (i && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && len - i < 3) {
        i--;
    }
    if (!i || ((unsigned char)s[i - 1] & 0x80) == 0) {
        return;   /* ASCII at the end */
    }
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (len - (i - 1) < need) {
        s[i - 1] = '\0';
    }
}

typedef struct {
    int volume;       /* 0..100 */
    bool muted;       /* replies are captioned, not spoken */
    int brightness;   /* 10..100 */
    int sleep_s;      /* one of SLEEP_CHOICES; 0 = never */
} gadget_settings_t;

/* The on-screen menu's choices (muse_settings_ui.c). */
static const int SLEEP_CHOICES[] = { 0, 30, 60, 120, 300, 600 };

static bool whole_in(const cJSON *v, int lo, int hi)
{
    return cJSON_IsNumber(v) && v->valuedouble >= lo && v->valuedouble <= hi &&
           v->valuedouble == (double)(int)v->valuedouble;
}

/*
 * Checks device.settings' parameters and, only if every one is good, applies
 * them to *s. A bad or unknown one changes nothing and says why in err.
 */
static bool parse_settings(const cJSON *params, gadget_settings_t *s, char *err, size_t cap)
{
    if (!params) {
        return true;
    }
    if (!cJSON_IsObject(params)) {
        snprintf(err, cap, "params must be an object");
        return false;
    }
    gadget_settings_t n = *s;
    const cJSON *v;
    cJSON_ArrayForEach(v, params) {
        const char *k = v->string;
        if (!strcmp(k, "volume")) {
            if (!whole_in(v, 0, 100)) {
                snprintf(err, cap, "volume must be a whole number from 0 to 100");
                return false;
            }
            n.volume = (int)v->valuedouble;
        } else if (!strcmp(k, "muted")) {
            if (!cJSON_IsBool(v)) {
                snprintf(err, cap, "muted must be true or false");
                return false;
            }
            n.muted = cJSON_IsTrue(v);
        } else if (!strcmp(k, "brightness")) {
            if (!whole_in(v, 10, 100)) {
                snprintf(err, cap, "brightness must be a whole number from 10 to 100");
                return false;
            }
            n.brightness = (int)v->valuedouble;
        } else if (!strcmp(k, "screen_sleep_s")) {
            bool allowed = false;
            for (size_t i = 0; i < sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0]); i++) {
                allowed = allowed || whole_in(v, SLEEP_CHOICES[i], SLEEP_CHOICES[i]);
            }
            if (!allowed) {
                snprintf(err, cap, "screen_sleep_s must be 0 (never), 30, 60, 120, 300 or 600");
                return false;
            }
            n.sleep_s = (int)v->valuedouble;
        } else {
            char key[41];
            snprintf(key, sizeof(key), "%.40s", k);
            utf8_trim(key);
            snprintf(err, cap, "unknown parameter %s: use volume, muted, brightness or screen_sleep_s", key);
            return false;
        }
    }
    *s = n;
    return true;
}

/* device.settings' report. battery_pct < 0: no reading; voice NULL or empty: unknown. */
static cJSON *settings_result(const gadget_settings_t *s, int battery_pct, bool charging, bool usb,
                              const char *voice)
{
    cJSON *p;
    cJSON *result = gadget_ok(&p);
    cJSON_AddNumberToObject(p, "volume", s->volume);
    cJSON_AddBoolToObject(p, "muted", s->muted);
    cJSON_AddNumberToObject(p, "brightness", s->brightness);
    cJSON_AddNumberToObject(p, "screen_sleep_s", s->sleep_s);
    if (battery_pct >= 0) {
        cJSON_AddNumberToObject(p, "battery_percent", battery_pct);
    } else {
        cJSON_AddNullToObject(p, "battery_percent");
    }
    cJSON_AddBoolToObject(p, "charging", charging);
    cJSON_AddBoolToObject(p, "on_usb", usb);
    if (voice && voice[0]) {
        cJSON_AddStringToObject(p, "voice", voice);
    } else {
        cJSON_AddNullToObject(p, "voice");
    }
    return result;
}

#define GADGET_VOICE_ID_MAX 32     /* ElevenLabs' are 20 */
#define GADGET_VOICE_NAME_MAX 64
#define GADGET_VOICES_MAX 64       /* listed for Muse */
#define GADGET_VOICES_SCAN 256     /* kept to match a name against */

typedef struct {
    char id[GADGET_VOICE_ID_MAX + 1];
    char name[GADGET_VOICE_NAME_MAX + 1];
    char category[16];   /* "premade", "cloned", ...; empty if none */
} gadget_voice_t;

/*
 * A forward-only JSON reader for ElevenLabs' voice list. The list can run to
 * hundreds of KB, and cJSON would build it as thousands of small nodes in
 * internal RAM; this keeps only the three strings it needs.
 */
typedef struct {
    const char *p, *end;
} scan_t;

static void scan_ws(scan_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) {
        s->p++;
    }
}

static bool scan_char(scan_t *s, char c)
{
    scan_ws(s);
    if (s->p < s->end && *s->p == c) {
        s->p++;
        return true;
    }
    return false;
}

/* A string, into out (cap bytes, or NULL to skip it). \uXXXX becomes '?'. */
static bool scan_str(scan_t *s, char *out, size_t cap)
{
    scan_ws(s);
    if (s->p >= s->end || *s->p != '"') {
        return false;
    }
    s->p++;
    size_t n = 0;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\') {
            if (s->p >= s->end) {
                return false;
            }
            c = *s->p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
                if (s->end - s->p < 4) {
                    return false;
                }
                s->p += 4;
                c = '?';
                break;
            default: break;   /* \" \\ \/ */
            }
        }
        if (out && n + 1 < cap) {
            out[n++] = c;
        }
    }
    if (s->p >= s->end) {
        return false;
    }
    s->p++;
    if (out && cap) {
        out[n] = '\0';
        utf8_trim(out);
    }
    return true;
}

/* Any value, unread. */
static bool scan_skip(scan_t *s, int depth)
{
    scan_ws(s);
    if (depth > 32 || s->p >= s->end) {
        return false;
    }
    char c = *s->p;
    if (c == '"') {
        return scan_str(s, NULL, 0);
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        s->p++;
        if (scan_char(s, close)) {
            return true;
        }
        do {
            if (c == '{' && !(scan_str(s, NULL, 0) && scan_char(s, ':'))) {
                return false;
            }
            if (!scan_skip(s, depth + 1)) {
                return false;
            }
        } while (scan_char(s, ','));
        return scan_char(s, close);
    }
    const char *start = s->p;
    while (s->p < s->end && (isalnum((unsigned char)*s->p) || *s->p == '-' || *s->p == '+' || *s->p == '.')) {
        s->p++;
    }
    return s->p > start;
}

/* One voice object: its own voice_id, name and category, not nested ones. */
static bool scan_voice(scan_t *s, gadget_voice_t *v)
{
    memset(v, 0, sizeof(*v));
    if (!scan_char(s, '{')) {
        return false;
    }
    if (scan_char(s, '}')) {
        return true;
    }
    do {
        char key[16];
        if (!scan_str(s, key, sizeof(key)) || !scan_char(s, ':')) {
            return false;
        }
        char *dst = NULL;
        size_t cap = 0;
        if (!strcmp(key, "voice_id")) {
            dst = v->id, cap = sizeof(v->id);
        } else if (!strcmp(key, "name")) {
            dst = v->name, cap = sizeof(v->name);
        } else if (!strcmp(key, "category")) {
            dst = v->category, cap = sizeof(v->category);
        }
        scan_ws(s);
        bool ok = dst && s->p < s->end && *s->p == '"' ? scan_str(s, dst, cap) : scan_skip(s, 2);
        if (!ok) {
            return false;
        }
    } while (scan_char(s, ','));
    return scan_char(s, '}');
}

/*
 * The voices in ElevenLabs' GET /v1/voices body ({"voices":[{...}], ...}):
 * how many have an id and a name, storing the first `max` of them in out.
 * -1 if the body isn't that shape or is cut short.
 */
static int scan_voices(const char *json, size_t len, gadget_voice_t *out, int max)
{
    scan_t s = { json, json + len };
    if (!scan_char(&s, '{') || scan_char(&s, '}')) {
        return -1;
    }
    do {
        char key[16];
        if (!scan_str(&s, key, sizeof(key)) || !scan_char(&s, ':')) {
            return -1;
        }
        if (strcmp(key, "voices")) {
            if (!scan_skip(&s, 1)) {
                return -1;
            }
            continue;
        }
        if (!scan_char(&s, '[')) {
            return -1;
        }
        int n = 0;
        if (scan_char(&s, ']')) {
            return 0;
        }
        do {
            gadget_voice_t v;
            if (!scan_voice(&s, &v)) {
                return -1;
            }
            if (v.id[0] && v.name[0]) {
                if (n < max) {
                    out[n] = v;
                }
                n++;
            }
        } while (scan_char(&s, ','));
        return scan_char(&s, ']') ? n : -1;
    } while (scan_char(&s, ','));
    return -1;
}

/*
 * The voice `name` means, ignoring case: one called exactly that or with that
 * ID, else the only one whose name starts with it (ElevenLabs' run on, as in
 * "George - Warm, Captivating Storyteller"). -1 none, -2 several.
 */
static int match_voice(const gadget_voice_t *v, int n, const char *name)
{
    size_t len = strlen(name);
    if (!len) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(v[i].name, name) || !strcmp(v[i].id, name)) {
            return i;
        }
    }
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (!strncasecmp(v[i].name, name, len)) {
            if (found >= 0) {
                return -2;
            }
            found = i;
        }
    }
    return found;
}

/* The message for a name that matched no voice (match -1) or several (-2), naming the candidates. */
static void voice_choice_error(char *out, size_t cap, const gadget_voice_t *v, int n,
                               const char *name, int match)
{
    char said[49];
    snprintf(said, sizeof(said), "%.48s", name);
    utf8_trim(said);
    int w = match == -2 ? snprintf(out, cap, "\"%s\" matches several voices: ", said)
                        : snprintf(out, cap, "no voice called \"%s\". Voices: ", said);
    if (w < 0 || (size_t)w >= cap) {
        utf8_trim(out);
        return;
    }
    size_t used = (size_t)w;
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (match == -2 && strncasecmp(v[i].name, name, strlen(name))) {
            continue;
        }
        w = snprintf(out + used, cap - used, "%s%s", first ? "" : ", ", v[i].name);
        if (w < 0 || (size_t)w >= cap - used) {
            if (cap >= 4) {
                out[cap - 4] = '\0';
                utf8_trim(out);
                strcat(out, "...");
            }
            return;
        }
        used += (size_t)w;
        first = false;
    }
}

/*
 * voice.select's list: the first list_max names and categories, the current
 * voice (found among all `stored`), and how many of `total` weren't listed.
 */
static cJSON *voices_result(const gadget_voice_t *v, int stored, int total, const char *current_id,
                            int list_max)
{
    cJSON *p;
    cJSON *result = gadget_ok(&p);
    cJSON *list = cJSON_AddArrayToObject(p, "voices");
    const char *current = NULL;
    int listed = stored < list_max ? stored : list_max;
    for (int i = 0; i < stored; i++) {
        if (i < listed) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", v[i].name);
            if (v[i].category[0]) {
                cJSON_AddStringToObject(item, "category", v[i].category);
            }
            cJSON_AddItemToArray(list, item);
        }
        if (current_id && !strcmp(v[i].id, current_id)) {
            current = v[i].name;
        }
    }
    stored = listed;
    if (current) {
        cJSON_AddStringToObject(p, "current", current);
    } else {
        cJSON_AddNullToObject(p, "current");
    }
    if (total > stored) {
        cJSON_AddNumberToObject(p, "not_listed", total - stored);
    }
    return result;
}

/* What to call the current voice: its name, else its ID (the build's voice has
 * no name until voice.select picks one); NULL if neither is known. */
static const char *voice_label(const char *name, const char *id)
{
    return name[0] ? name : id[0] ? id : NULL;
}

static cJSON *selected_result(const gadget_voice_t *v)
{
    cJSON *p;
    cJSON *result = gadget_ok(&p);
    cJSON_AddStringToObject(p, "voice", v->name);
    return result;
}

/* Why GET /v1/voices gave no list: an error code and *message, or NULL for 200.
 * status: the HTTP status; -1 unreachable or cut short; 0 no key. */
static const char *voice_fetch_error(int status, const char **message)
{
    if (status == 200) {
        return NULL;
    }
    if (status == 0) {
        *message = "spoken replies are off: no ElevenLabs key is set on the gadget";
        return "unavailable";
    }
    if (status == 401 || status == 403) {
        *message = "the ElevenLabs key can't list voices: give it the Voices read permission at elevenlabs.io";
        return "unavailable";
    }
    *message = status < 0 ? "couldn't reach ElevenLabs for the voice list"
                          : "ElevenLabs refused the voice list request";
    return "network";
}

typedef struct {
    muse_sound_kind_t kind;
    const char *arg;   /* in params: the text, URL or chime name */
    bool caption;      /* voice.say: show the text as it's said */
} gadget_sound_t;

/* A URL the player can fetch: https://, a host, no spaces or control bytes. */
static bool usable_url(const char *url, size_t len)
{
    if (len <= 8 || len > MUSE_SOUND_URL_MAX || strncasecmp(url, "https://", 8)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)url[i] <= ' ' || url[i] == 0x7f) {
            return false;
        }
    }
    return true;
}

/*
 * Checks voice.say ({text, caption}), audio.play_url ({url}) or audio.chime
 * ({name}) into *out; out->arg points into params. A missing, wrong or
 * unknown parameter is refused, saying why in err.
 */
static bool parse_sound(const char *command, const cJSON *params, gadget_sound_t *out, char *err, size_t cap)
{
    const char *key = "name";
    out->kind = MUSE_SOUND_CHIME;
    if (!strcmp(command, "voice.say")) {
        out->kind = MUSE_SOUND_SAY;
        key = "text";
    } else if (!strcmp(command, "audio.play_url")) {
        out->kind = MUSE_SOUND_URL;
        key = "url";
    }
    out->arg = NULL;
    out->caption = true;
    if (!cJSON_IsObject(params)) {
        snprintf(err, cap, "%s is required", key);
        return false;
    }
    const cJSON *v;
    cJSON_ArrayForEach(v, params) {
        if (!strcmp(v->string, key)) {
            if (!cJSON_IsString(v)) {
                snprintf(err, cap, "%s must be a string", key);
                return false;
            }
            out->arg = v->valuestring;
        } else if (out->kind == MUSE_SOUND_SAY && !strcmp(v->string, "caption")) {
            if (!cJSON_IsBool(v)) {
                snprintf(err, cap, "caption must be true or false");
                return false;
            }
            out->caption = cJSON_IsTrue(v);
        } else {
            char name[41];
            snprintf(name, sizeof(name), "%.40s", v->string);
            utf8_trim(name);
            snprintf(err, cap, "unknown parameter %s: use %s%s", name, key,
                     out->kind == MUSE_SOUND_SAY ? " and caption" : "");
            return false;
        }
    }
    if (!out->arg) {
        snprintf(err, cap, "%s is required", key);
        return false;
    }
    size_t len = strlen(out->arg);
    if (out->kind == MUSE_SOUND_SAY && (!len || len > MUSE_SOUND_SAY_MAX)) {
        snprintf(err, cap, "text must be 1 to %d bytes", MUSE_SOUND_SAY_MAX);
        return false;
    }
    if (out->kind == MUSE_SOUND_URL && !usable_url(out->arg, len)) {
        snprintf(err, cap, "url must be an https:// address of at most %d bytes, without spaces",
                 MUSE_SOUND_URL_MAX);
        return false;
    }
    if (out->kind == MUSE_SOUND_CHIME && muse_chime_find(out->arg) < 0) {
        snprintf(err, cap, "name must be " MUSE_CHIME_NAMES);
        return false;
    }
    return true;
}

/* What a sound's muse_play_enqueue() gave: its place in the queue (1 = next),
 * 0 when the queue is full, -1 out of memory. */
static cJSON *sound_result(int position)
{
    if (position == 0) {
        return gadget_error("busy", "sounds are already waiting to play; try again in a few seconds");
    }
    if (position < 0) {
        return gadget_error("out_of_memory", "no room for the sound");
    }
    cJSON *p;
    cJSON *result = gadget_ok(&p);
    cJSON_AddBoolToObject(p, "queued", true);
    cJSON_AddNumberToObject(p, "position", position);
    return result;
}

/* The speaker is off: speech with a caption is shown instead, anything else waits for unmuting. */
static cJSON *muted_error(muse_sound_kind_t kind, bool caption)
{
    return gadget_error("muted", kind == MUSE_SOUND_SAY && caption
                                     ? "the speaker is muted, so the text is shown on the screen instead"
                                     : "the speaker is muted: unmute it with device.settings first if the user wants sound");
}

/* What a new voice says first: "Hi, I'm George." for "George - Warm, Captivating Storyteller". */
static void hello_text(char *out, size_t cap, const char *name)
{
    const char *dash = strstr(name, " - ");
    size_t n = dash ? (size_t)(dash - name) : strlen(name);
    if (!n) {
        snprintf(out, cap, "Hi, this is my new voice.");
        return;
    }
    snprintf(out, cap, "Hi, I'm %.*s.", (int)(n > GADGET_VOICE_NAME_MAX ? GADGET_VOICE_NAME_MAX : n), name);
    utf8_trim(out);
}

/* ---- Device ---- */

#include "esp_log.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#if CONFIG_MUSE_HATCH
#include "muse_tts_elevenlabs.h"
#endif

#include "muse_settings.h"
#include "muse_state.h"

static const char *TAG = "muse_gadget";

static gadget_settings_t current_settings(void)
{
    return (gadget_settings_t){
        .volume = muse_settings_volume(),
        .muted = !muse_settings_speaker_on(),
        .brightness = muse_settings_brightness(),
        .sleep_s = muse_settings_sleep_s(),
    };
}

static cJSON *settings_command(const cJSON *params)
{
    gadget_settings_t was = current_settings();
    gadget_settings_t s = was;
    char err[160];
    if (!parse_settings(params, &s, err, sizeof(err))) {
        return gadget_error("invalid_params", err);
    }
    /* Only what changed: each setter writes flash. The setters already run from
     * the BLE task too (muse_ble.c); brightness, sleep and the speaker are polled. */
    if (s.volume != was.volume) {
        muse_settings_set_volume(s.volume);
    }
    if (s.muted != was.muted) {
        muse_settings_set_speaker_on(!s.muted);
    }
    if (s.brightness != was.brightness) {
        muse_settings_set_brightness(s.brightness);
    }
    if (s.sleep_s != was.sleep_s) {
        muse_settings_set_sleep_s(s.sleep_s);
    }
    if (params && cJSON_GetArraySize(params)) {
        ESP_LOGI(TAG, "settings: volume %d%s, brightness %d, sleep %ds", s.volume,
                 s.muted ? " (muted)" : "", s.brightness, s.sleep_s);
    }
    muse_power_t power = muse_state_power();
    char voice_id[GADGET_VOICE_ID_MAX + 1] = "";
    char voice[GADGET_VOICE_NAME_MAX + 1] = "";
#if CONFIG_MUSE_HATCH
    muse_tts_voice(voice_id, voice);
#endif
    return settings_result(&s, power.battery_pct, power.charging, power.usb,
                           voice_label(voice, voice_id));
}

#if CONFIG_MUSE_HATCH
_Static_assert(GADGET_VOICE_ID_MAX == MUSE_TTS_VOICE_ID_MAX, "voice ID sizes differ");
_Static_assert(GADGET_VOICE_NAME_MAX == MUSE_TTS_VOICE_NAME_MAX, "voice name sizes differ");

/* With instructions and rodata in PSRAM, flash writes leave the cache on, so a
 * PSRAM stack may save to NVS; otherwise the task needs an internal one. */
#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA
#define VOICE_TASK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define VOICE_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

typedef struct {
    uint64_t session_generation;
    char request_id[64];
    char name[GADGET_VOICE_NAME_MAX + 1];   /* empty: list the voices */
    muse_gadget_send_fn send;
} voice_job_t;

static atomic_bool s_voice_busy;

static cJSON *voice_select(const char *name)
{
    char *body = NULL;
    size_t len = 0;
    int status = muse_tts_enabled() ? muse_tts_fetch_voices(&body, &len) : 0;
    const char *message = NULL;
    const char *code = voice_fetch_error(status, &message);
    if (code) {
        free(body);
        return gadget_error(code, message);
    }
    gadget_voice_t *v = heap_caps_calloc(GADGET_VOICES_SCAN, sizeof(*v), MALLOC_CAP_SPIRAM);
    int total = v ? scan_voices(body, len, v, GADGET_VOICES_SCAN) : -1;
    free(body);
    if (!v) {
        return gadget_error("out_of_memory", "no room for the voice list");
    }
    int stored = total < GADGET_VOICES_SCAN ? total : GADGET_VOICES_SCAN;
    char current[GADGET_VOICE_ID_MAX + 1];
    muse_tts_voice(current, NULL);
    cJSON *result;
    if (total < 0) {
        result = gadget_error("network", "ElevenLabs sent a voice list the gadget can't read");
    } else if (!name[0]) {
        result = voices_result(v, stored, total, current, GADGET_VOICES_MAX);
    } else {
        int i = match_voice(v, stored, name);
        if (i < 0) {
            char msg[512];
            voice_choice_error(msg, sizeof(msg), v, stored, name, i);
            result = gadget_error(i == -2 ? "ambiguous" : "not_found", msg);
        } else if (!muse_tts_set_voice(v[i].id, v[i].name)) {
            result = gadget_error("internal", "couldn't save the voice");
        } else {
            result = selected_result(&v[i]);
        }
    }
    free(v);
    return result;
}

static void voice_task(void *arg)
{
    voice_job_t *job = arg;
    job->send(job->session_generation, job->request_id, voice_select(job->name));
    free(job);
    atomic_store(&s_voice_busy, false);
    vTaskDeleteWithCaps(NULL);
}

static cJSON *voice_select_start(const cJSON *params, const char *request_id,
                                 uint64_t session_generation, muse_gadget_send_fn send)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (name && !cJSON_IsString(name)) {
        return gadget_error("invalid_params", "name must be a string");
    }
    const char *n = name ? name->valuestring : "";
    if (strlen(n) > GADGET_VOICE_NAME_MAX) {
        return gadget_error("invalid_params", "name is longer than any voice's");
    }
    if (!request_id || strlen(request_id) >= sizeof(((voice_job_t *)0)->request_id)) {
        return gadget_error("invalid_params", "request_id is invalid");
    }
    if (atomic_exchange(&s_voice_busy, true)) {
        return gadget_error("busy", "already looking up the voices; try again in a few seconds");
    }
    voice_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    if (job) {
        job->session_generation = session_generation;
        strlcpy(job->request_id, request_id, sizeof(job->request_id));
        strlcpy(job->name, n, sizeof(job->name));
        job->send = send;
    }
    /* Stack as the TTS task's: TLS runs here. */
    if (!job || xTaskCreateWithCaps(voice_task, "gadget_voice", 12 * 1024, job, 3, NULL,
                                    VOICE_TASK_CAPS) != pdPASS) {
        free(job);
        atomic_store(&s_voice_busy, false);
        return gadget_error("out_of_memory", "couldn't start the voice lookup");
    }
    cJSON *async = cJSON_CreateObject();
    cJSON_AddBoolToObject(async, "_async", true);
    return async;
}
#endif

static cJSON *param(const char *type, const char *description)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

/* As add_command() in noise_control.cpp. Keep descriptions short: link.register
 * must fit in 8 KB, and the skill says the rest. */
static cJSON *add_command(cJSON *commands, const char *name, const char *description,
                          cJSON *optional)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "description", description);
    cJSON_AddItemToObject(c, "required", cJSON_CreateObject());
    cJSON_AddItemToObject(c, "optional", optional ? optional : cJSON_CreateObject());
    cJSON_AddItemToObject(commands, name, c);
    return c;
}

void muse_gadget_add_commands(cJSON *commands)
{
    cJSON *opt = cJSON_CreateObject();
    cJSON_AddItemToObject(opt, "volume", param("integer", "Speaker volume, 0-100."));
    cJSON_AddItemToObject(opt, "muted", param("boolean", "true: replies are shown, not spoken."));
    cJSON_AddItemToObject(opt, "brightness", param("integer", "Screen brightness, 10-100."));
    cJSON_AddItemToObject(opt, "screen_sleep_s",
                          param("integer", "Screen sleeps after 0 (never), 30, 60, 120, 300 or 600 s."));
    add_command(commands, "device.settings",
                "Read or change volume, mute, brightness and screen sleep. "
                "Reports them all, with the battery and voice.",
                opt);
#if CONFIG_MUSE_HATCH
    cJSON *vopt = cJSON_CreateObject();
    cJSON_AddItemToObject(vopt, "name", param("string", "Voice to switch to; without it, lists them."));
    cJSON *v = add_command(commands, "voice.select",
                           "List the ElevenLabs voices replies can be spoken in, or switch to one.", vopt);
    cJSON_AddNumberToObject(v, "timeout_ms", 30000);
#endif
}

cJSON *muse_gadget_command(const char *command, cJSON *params, const char *request_id,
                           uint64_t session_generation, muse_gadget_send_fn send)
{
    if (!strcmp(command, "device.settings")) {
        return settings_command(params);
    }
#if CONFIG_MUSE_HATCH
    if (!strcmp(command, "voice.select")) {
        return voice_select_start(params, request_id, session_generation, send);
    }
#else
    (void)request_id;
    (void)session_generation;
    (void)send;
#endif
    return NULL;
}

#endif   /* CONFIG_MUSE_GADGET_COMMANDS */
