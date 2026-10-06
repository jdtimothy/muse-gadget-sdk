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
/* Always in the build (components/muse/CMakeLists.txt): nothing without the option. */
#if CONFIG_MUSE_GADGET_COMMANDS
#include "muse_gadget_react.h"

#include <stdint.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "muse_avatar_idle.h"
#include "muse_reactions.h"
#include "muse_settings.h"
#include "muse_state.h"
#if CONFIG_MUSE_HATCH
#include "muse_gadget_options.h"
#endif

/* What the avatar shows: a reaction from Muse, or one of the gadget's own scenes. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_id;
static int64_t s_start_us;
static float s_secs;

/* The idle picker's, on the UI task. */
static muse_idle_t s_idle;
static float s_last_frame;

static void show(int id, float secs)
{
    taskENTER_CRITICAL(&s_lock);
    s_id = id;
    s_start_us = esp_timer_get_time();
    s_secs = secs;
    taskEXIT_CRITICAL(&s_lock);
}

void muse_react_set(int id, int seconds)
{
    show(id, (float)seconds);
    if (id != MUSE_REACT_NONE) {
        muse_state_set_asleep(false);   /* to be seen; it sleeps again on its own timer */
    }
}

/* Fades `id` out now (as reactions end), if it's still the one showing. */
static void fade_out(int id)
{
    taskENTER_CRITICAL(&s_lock);
    float elapsed = (float)(esp_timer_get_time() - s_start_us) / 1e6f;
    if (s_id == id && elapsed + MUSE_REACT_OUT < s_secs) {
        s_secs = elapsed + MUSE_REACT_OUT;
    }
    taskEXIT_CRITICAL(&s_lock);
}

/* 30 s of dancing; already dancing, 30 s from now without starting over. */
static void start_dance(void)
{
    float secs = muse_act_secs(MUSE_ACT_DANCE);
    taskENTER_CRITICAL(&s_lock);
    float elapsed = (float)(esp_timer_get_time() - s_start_us) / 1e6f;
    bool dancing = s_id == MUSE_ACT_DANCE && elapsed < s_secs;
    if (dancing) {
        s_secs = elapsed + secs;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!dancing) {
        show(MUSE_ACT_DANCE, secs);
    }
}

void muse_react_media(const char *state, bool press)
{
    switch (muse_dance_on_media(state, press)) {
    case MUSE_DANCE_START:
        start_dance();
        break;
    case MUSE_DANCE_STOP:
        fade_out(MUSE_ACT_DANCE);
        break;
    default:
        break;
    }
}

/* Each frame the avatar is drawn (the UI task): ends a scene something
 * interrupts, and lets the idle picker start one. */
static void idle_frame(void)
{
    int64_t now_us = esp_timer_get_time();
    float now = (float)now_us / 1e6f;
    taskENTER_CRITICAL(&s_lock);
    int id = s_id;
    float elapsed = (float)(now_us - s_start_us) / 1e6f, secs = s_secs;
    taskEXIT_CRITICAL(&s_lock);
    bool showing = id != MUSE_REACT_NONE && elapsed < secs;
    muse_mode_t mode = muse_state_mode(NULL);
    bool talking = mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING;
    bool busy = mode != MUSE_MODE_IDLE || muse_state_happiness() > 0;
#if CONFIG_MUSE_HATCH
    busy = busy || muse_options_showing();
#endif
    if (showing && id >= MUSE_REACT_COUNT && (id == MUSE_ACT_DANCE ? talking : busy)) {
        fade_out(id);   /* the dance gives way to a turn; a scene to anything */
    }
    /* Not drawn for a while (asleep, another tile, a picture): a fresh gap. */
    bool seen = now - s_last_frame < 1.0f;
    s_last_frame = now;
    bool calm = seen && !busy && !showing && !muse_state_asleep() && muse_state_idle_secs() >= 1.0f;
    int pick = muse_idle_step(&s_idle, now, calm, muse_state_idle_secs(),
                              muse_idle_doze_after(muse_settings_sleep_s()), esp_random());
    if (pick) {
        show(pick, muse_act_secs(pick));   /* no waking: it only plays on an awake screen */
    }
}

int muse_react_pose(float *t, float *amount)
{
    idle_frame();
    taskENTER_CRITICAL(&s_lock);
    int id = s_id;
    int64_t start = s_start_us;
    float secs = s_secs;
    taskEXIT_CRITICAL(&s_lock);
    float elapsed = (float)(esp_timer_get_time() - start) / 1e6f;
    float a = id != MUSE_REACT_NONE ? muse_reaction_amount(elapsed, secs) : 0;
    *t = a > 0 ? elapsed : 0;
    *amount = a;
    return a > 0 ? id : MUSE_REACT_NONE;
}
#endif   /* CONFIG_MUSE_GADGET_COMMANDS */
