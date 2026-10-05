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

/* Host harness for the reactions in avatar/muse_pixel.c (components/muse/muse_reactions.inc).
 * Includes the renderer itself, to reach its frame buffer and state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../avatar/muse_pixel.c"

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK(%s) ", __FILE__, __LINE__, #cond);   \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static uint8_t s_frame[W * H];

/* Renders 1 s from a fresh start (same blinks and gaze every time) and keeps the last grid. */
static void render(muse_mode_t mode, int reaction, float level, float happy)
{
    s_scheme_init = false;
    s_eyes = (eyes_t){ .next_blink = 1.5f, .blink_start = -10, .next_gaze = 1.0f };
    s_rng = 0x9e3779b9u;
    for (float t = 0; t < 1.0f; t += 0.04f) {
        muse_pose_t p = {
            .mode = mode, .t = 20.0f + t, .mode_t = 5.0f + t, .level = level, .happy = happy,
            .reaction = reaction, .react_t = 0.5f + t, .react_amount = reaction ? 1.0f : 0.0f,
        };
        muse_pixel_render(&p);
    }
    memcpy(s_frame, s_fb, sizeof(s_frame));
}

static int differs(const uint8_t *a, const uint8_t *b)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        n += a[i] != b[i];
    }
    return n;
}

/* Every reaction shows: its frame differs from the plain one by more than a blink. */
static void test_each_reaction_shows(void)
{
    static uint8_t plain[W * H];
    render(MUSE_MODE_IDLE, 0, 0, 0);
    memcpy(plain, s_frame, sizeof(plain));
    for (int r = 1; r < MUSE_REACT_COUNT; r++) {
        render(MUSE_MODE_IDLE, r, 0, 0);
        CHECK(differs(plain, s_frame) >= 8, "%s barely shows (%d cells)", MUSE_REACTION_NAMES[r],
              differs(plain, s_frame));
    }
}

/* Review focus 2: boot, error and off ignore reactions. */
static void test_some_modes_ignore_reactions(void)
{
    static const muse_mode_t MODES[] = { MUSE_MODE_ERROR, MUSE_MODE_BOOT, MUSE_MODE_OFF };
    static uint8_t plain[W * H];
    for (size_t m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++) {
        render(MODES[m], 0, 0, 0);
        memcpy(plain, s_frame, sizeof(plain));
        for (int r = 1; r < MUSE_REACT_COUNT; r++) {
            render(MODES[m], r, 0, 0);
            CHECK(differs(plain, s_frame) == 0, "%s shows in mode %d", MUSE_REACTION_NAMES[r], (int)MODES[m]);
        }
    }
}

/* Review focus 1: listening and speaking keep their own mouth; idle and thinking take the reaction's. */
static void test_speech_keeps_its_mouth(void)
{
    for (int r = 1; r < MUSE_REACT_COUNT; r++) {
        muse_pose_t p = { .mode = MUSE_MODE_SPEAKING, .t = 1, .reaction = r, .react_amount = 1 };
        CHECK(!react_mouth(&p, 32, 30), "%s takes the talking mouth", MUSE_REACTION_NAMES[r]);
        p.mode = MUSE_MODE_LISTENING;
        CHECK(!react_mouth(&p, 32, 30), "%s takes the listening mouth", MUSE_REACTION_NAMES[r]);
        p.mode = MUSE_MODE_IDLE;
        bool own = REACTIONS[r].mouth != RM_KEEP;
        CHECK(react_mouth(&p, 32, 30) == own, "%s idle mouth", MUSE_REACTION_NAMES[r]);
    }
}

/* Review focus 3: petting wins the face. */
static void test_petting_wins(void)
{
    muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = 1, .happy = 1, .reaction = MUSE_REACT_SAD, .react_amount = 1 };
    CHECK(react_face_row(&p) == NULL, "petting");
    p.happy = 0;
    CHECK(react_face_row(&p) != NULL, "no petting");
    p.react_amount = 0.2f;
    CHECK(react_face_row(&p) == NULL, "the face waits until it's mostly in");
    CHECK(react_row(&p) != NULL, "the prop and glow fade in");
    p.react_amount = 0;
    CHECK(react_row(&p) == NULL, "gone");
    p.react_amount = 1;
    p.reaction = MUSE_REACT_COUNT;
    CHECK(react_row(&p) == NULL, "an unknown id");
}

/* Every reaction has a row with something in it. */
static void test_table(void)
{
    for (int r = 1; r < MUSE_REACT_COUNT; r++) {
        const reaction_t *x = &REACTIONS[r];
        bool any = x->eyes != RE_KEEP || x->brows != RB_NONE || x->mouth != RM_KEEP || x->glow != RG_NONE ||
                   x->move != RV_NONE || x->arms != RA_KEEP || x->back || x->front;
        CHECK(any, "%s has an empty row", MUSE_REACTION_NAMES[r]);
    }
}

int main(void)
{
    test_table();
    test_each_reaction_shows();
    test_some_modes_ignore_reactions();
    test_speech_keeps_its_mouth();
    test_petting_wins();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
