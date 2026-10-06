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

#pragma once

/*
 * The avatar's reactions (avatar.react): their ids and names, and how strong
 * one is over its time, shared by the command (muse_gadget_cmds.c), the
 * renderer (muse_reactions.inc, included by avatar/muse_pixel.c) and the host
 * previews (tools/muse/react_anim.c). No dependencies.
 */

#include <strings.h>

typedef enum {
    MUSE_REACT_NONE = 0,
    MUSE_REACT_LOVE,
    MUSE_REACT_LAUGH,
    MUSE_REACT_EXCITED,
    MUSE_REACT_SURPRISED,
    MUSE_REACT_CONFUSED,
    MUSE_REACT_SAD,
    MUSE_REACT_GRUMPY,
    MUSE_REACT_SLEEPY,
    MUSE_REACT_NERVOUS,
    MUSE_REACT_COOL,
    MUSE_REACT_WINK,
    MUSE_REACT_YES,
    MUSE_REACT_NO,
    MUSE_REACT_CELEBRATE,
    MUSE_REACT_SUNNY,
    MUSE_REACT_RAINY,
    MUSE_REACT_STORMY,
    MUSE_REACT_COLD,
    MUSE_REACT_WINDY,
    MUSE_REACT_HOT,
    MUSE_REACT_RAINBOW,
    MUSE_REACT_COUNT,
} muse_reaction_t;

static const char *const MUSE_REACTION_NAMES[MUSE_REACT_COUNT] = {
    "none", "love", "laugh", "excited", "surprised", "confused", "sad", "grumpy",
    "sleepy", "nervous", "cool", "wink", "yes", "no", "celebrate", "sunny",
    "rainy", "stormy", "cold", "windy", "hot", "rainbow",
};

#define MUSE_REACTION_NAMES_TEXT                                                           \
    "love, laugh, excited, surprised, confused, sad, grumpy, sleepy, nervous, cool, wink, " \
    "yes, no, celebrate, sunny, rainy, stormy, cold, windy, hot or rainbow"

/* Built-in animations the gadget starts itself (the idle picker and the
 * dance, muse_gadget_react.c), drawn like reactions. Not Muse's to call:
 * they're outside MUSE_REACTION_NAMES and muse_reaction_find(). */
typedef enum {
    MUSE_ACT_DANCE = MUSE_REACT_COUNT,
    MUSE_ACT_STRETCH,
    MUSE_ACT_HUM,
    MUSE_ACT_BUTTERFLY,
    MUSE_ACT_DOZE,
    MUSE_ACT_PACE,
    MUSE_ANIM_COUNT,
} muse_activity_t;

/* A reaction's or built-in animation's name, for logs and previews. */
static inline const char *muse_anim_name(int id)
{
    static const char *const ACTS[] = { "dance", "stretch", "hum", "butterfly", "doze", "pace" };
    if (id >= 0 && id < MUSE_REACT_COUNT) {
        return MUSE_REACTION_NAMES[id];
    }
    return id < MUSE_ANIM_COUNT ? ACTS[id - MUSE_REACT_COUNT] : "?";
}

/* How long a built-in animation plays, s. */
static inline float muse_act_secs(int id)
{
    static const float SECS[] = { 30, 3, 5, 5, 6, 6 };
    return id >= MUSE_REACT_COUNT && id < MUSE_ANIM_COUNT ? SECS[id - MUSE_REACT_COUNT] : 0;
}

#define MUSE_REACT_SECS_DEFAULT 4
#define MUSE_REACT_SECS_MAX 30
#define MUSE_REACT_IN 0.25f    /* seconds to come in */
#define MUSE_REACT_OUT 0.4f    /* and to go, as petting does */

/* The reaction called `name`, ignoring case: its id (MUSE_REACT_NONE for
 * "none"), or -1. */
static inline int muse_reaction_find(const char *name)
{
    for (int i = 0; i < MUSE_REACT_COUNT; i++) {
        if (!strcasecmp(MUSE_REACTION_NAMES[i], name)) {
            return i;
        }
    }
    return -1;
}

/* How strong a reaction is `elapsed` seconds into its `secs`: up over
 * MUSE_REACT_IN, held, down over its last MUSE_REACT_OUT; 0 outside it. */
static inline float muse_reaction_amount(float elapsed, float secs)
{
    if (elapsed < 0 || elapsed >= secs) {
        return 0;
    }
    float in = elapsed < MUSE_REACT_IN ? elapsed / MUSE_REACT_IN : 1.0f;
    float left = secs - elapsed;
    float out = left < MUSE_REACT_OUT ? left / MUSE_REACT_OUT : 1.0f;
    return in < out ? in : out;
}
