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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

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
            snprintf(err, cap, "unknown parameter %.40s: use volume, muted, brightness or screen_sleep_s", k);
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

/* ---- Device ---- */

#endif   /* CONFIG_MUSE_GADGET_COMMANDS */
