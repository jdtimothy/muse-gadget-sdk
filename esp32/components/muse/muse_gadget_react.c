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

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "muse_reactions.h"
#include "muse_state.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_id;
static int64_t s_start_us;
static float s_secs;

void muse_react_set(int id, int seconds)
{
    taskENTER_CRITICAL(&s_lock);
    s_id = id;
    s_start_us = esp_timer_get_time();
    s_secs = (float)seconds;
    taskEXIT_CRITICAL(&s_lock);
    if (id != MUSE_REACT_NONE) {
        muse_state_set_asleep(false);   /* to be seen; it sleeps again on its own timer */
    }
}

int muse_react_pose(float *t, float *amount)
{
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
