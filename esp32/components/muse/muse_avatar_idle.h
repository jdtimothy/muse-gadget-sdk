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
 * When the avatar's built-in animations (muse_reactions.h) start: the idle
 * picker's scenes and the dance after a media event. muse_gadget_react.c runs
 * them each frame. No dependencies, so the host tests can use it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "muse_reactions.h"

#define MUSE_IDLE_GAP_MIN 10.0f
#define MUSE_IDLE_GAP_MAX 30.0f
#define MUSE_IDLE_DOZE_AFTER 300.0f   /* quiet this long before a doze */

typedef struct {
    float next_at;   /* when the next scene may start, s; 0 before the first frame */
    int last;        /* the scene picked last, 0 before any */
} muse_idle_t;

/* A gap between scenes, 10-30 s, from a random number. */
static inline float muse_idle_gap(uint32_t rnd)
{
    return MUSE_IDLE_GAP_MIN + (MUSE_IDLE_GAP_MAX - MUSE_IDLE_GAP_MIN) * (float)(rnd % 1001u) / 1000.0f;
}

/* Quiet needed before a doze: 5 minutes, or 20 s before the screen sleeps if
 * that's sooner (sleep_s 0: never sleeps), and at least 10 s. */
static inline float muse_idle_doze_after(int sleep_s)
{
    float before = (float)sleep_s - 20.0f;
    if (sleep_s <= 0 || before >= MUSE_IDLE_DOZE_AFTER) {
        return MUSE_IDLE_DOZE_AFTER;
    }
    return before < 10.0f ? 10.0f : before;
}

/*
 * One frame of the idle picker. `calm`: the avatar is idle, awake, on screen,
 * and nothing is showing or touching it. `quiet_s`: seconds since the last
 * touch, press or turn. Returns the scene to start now, or 0. Anything not
 * calm (a scene playing included) restarts the gap.
 */
static inline int muse_idle_step(muse_idle_t *s, float now, bool calm, float quiet_s, float doze_after, uint32_t rnd)
{
    if (!calm || s->next_at <= 0) {
        s->next_at = now + muse_idle_gap(rnd);
        return 0;
    }
    if (now < s->next_at) {
        return 0;
    }
    static const int POOL[] = { MUSE_ACT_STRETCH, MUSE_ACT_HUM, MUSE_ACT_BUTTERFLY, MUSE_ACT_PACE, MUSE_ACT_DOZE };
    int cand[5], n = 0;
    for (int i = 0; i < 5; i++) {
        if (POOL[i] != s->last && (POOL[i] != MUSE_ACT_DOZE || quiet_s >= doze_after)) {
            cand[n++] = POOL[i];
        }
    }
    int pick = cand[(rnd >> 11) % (uint32_t)n];
    s->last = pick;
    s->next_at = now + muse_act_secs(pick) + muse_idle_gap(rnd >> 5);
    return pick;
}

typedef enum { MUSE_DANCE_KEEP, MUSE_DANCE_START, MUSE_DANCE_STOP } muse_dance_t;

/* What a media event does to the dance: a press of back, play/pause or skip,
 * or an update to playing, starts (or restarts) it; paused or idle stops it. */
static inline muse_dance_t muse_dance_on_media(const char *state, bool press)
{
    if (press || (state && !strcmp(state, "playing"))) {
        return MUSE_DANCE_START;
    }
    return state ? MUSE_DANCE_STOP : MUSE_DANCE_KEEP;
}
