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
 * The sounds Muse can ask the gadget for (CONFIG_MUSE_GADGET_COMMANDS):
 * muse_gadget_cmds.c checks the commands against these, and
 * muse_gadget_play.c plays them. No dependencies, so both files' host-tested
 * sections can include it.
 */

#include <stdint.h>
#include <strings.h>

typedef enum {
    MUSE_SOUND_SAY,     /* text, spoken through ElevenLabs */
    MUSE_SOUND_URL,     /* an MP3 clip over https */
    MUSE_SOUND_CHIME,   /* one of MUSE_CHIMES, by name */
} muse_sound_kind_t;

#define MUSE_SOUND_SAY_MAX 600   /* bytes of text */
#define MUSE_SOUND_URL_MAX 512   /* bytes of URL */
#define MUSE_SOUND_QUEUE 4       /* sounds waiting to play */

typedef struct {
    uint16_t hz, ms, gap_ms;   /* a tone, then silence */
} muse_note_t;

typedef struct {
    const char *name;
    uint8_t repeat;   /* the notes play this many times over */
    uint8_t n;
    muse_note_t notes[4];
} muse_chime_t;

/* 0.4-2.7 s each. */
static const muse_chime_t MUSE_CHIMES[] = {
    { "ding", 1, 1, { { 1319, 600, 0 } } },
    { "success", 1, 2, { { 784, 120, 0 }, { 1175, 300, 0 } } },
    { "error", 1, 2, { { 392, 180, 40 }, { 294, 320, 0 } } },
    { "alert", 3, 1, { { 1568, 90, 70 } } },
    { "timer", 4, 2, { { 1047, 100, 60 }, { 1047, 100, 400 } } },
    { "tada", 1, 4, { { 523, 90, 10 }, { 659, 90, 10 }, { 784, 90, 10 }, { 1047, 450, 0 } } },
};
#define MUSE_CHIME_COUNT ((int)(sizeof(MUSE_CHIMES) / sizeof(MUSE_CHIMES[0])))
#define MUSE_CHIME_NAMES "ding, success, error, alert, timer or tada"

/* The chime called `name`, ignoring case: its index in MUSE_CHIMES, or -1. */
static inline int muse_chime_find(const char *name)
{
    for (int i = 0; i < MUSE_CHIME_COUNT; i++) {
        if (!strcasecmp(MUSE_CHIMES[i].name, name)) {
            return i;
        }
    }
    return -1;
}
