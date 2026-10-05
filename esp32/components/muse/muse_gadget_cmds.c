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

#include "esp_log.h"

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
    return settings_result(&s, power.battery_pct, power.charging, power.usb, NULL);
}

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
}

cJSON *muse_gadget_command(const char *command, cJSON *params, const char *request_id,
                           uint64_t session_generation, muse_gadget_send_fn send)
{
    (void)request_id;
    (void)session_generation;
    (void)send;
    if (!strcmp(command, "device.settings")) {
        return settings_command(params);
    }
    return NULL;
}

#endif   /* CONFIG_MUSE_GADGET_COMMANDS */
