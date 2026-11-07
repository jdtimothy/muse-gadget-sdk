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
    for (int r = 1; r < MUSE_ANIM_COUNT; r++) {
        render(MUSE_MODE_IDLE, r, 0, 0);
        CHECK(differs(plain, s_frame) >= 8, "%s barely shows (%d cells)", muse_anim_name(r),
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
        for (int r = 1; r < MUSE_ANIM_COUNT; r++) {
            render(MODES[m], r, 0, 0);
            CHECK(differs(plain, s_frame) == 0, "%s shows in mode %d", muse_anim_name(r), (int)MODES[m]);
        }
    }
}

/* Review focus 1: listening and speaking keep their own mouth; idle and thinking take the reaction's. */
static void test_speech_keeps_its_mouth(void)
{
    for (int r = 1; r < MUSE_ANIM_COUNT; r++) {
        muse_pose_t p = { .mode = MUSE_MODE_SPEAKING, .t = 1, .reaction = r, .react_amount = 1 };
        CHECK(!react_mouth(&p, 32, 30), "%s takes the talking mouth", muse_anim_name(r));
        p.mode = MUSE_MODE_LISTENING;
        CHECK(!react_mouth(&p, 32, 30), "%s takes the listening mouth", muse_anim_name(r));
        p.mode = MUSE_MODE_IDLE;
        bool own = REACTIONS[r].mouth != RM_KEEP;
        CHECK(react_mouth(&p, 32, 30) == own, "%s idle mouth", muse_anim_name(r));
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
    p.reaction = MUSE_ANIM_COUNT;
    CHECK(react_row(&p) == NULL, "an unknown id");
}

/* Every reaction has a row with something in it. */
static void test_table(void)
{
    for (int r = 1; r < MUSE_ANIM_COUNT; r++) {
        const reaction_t *x = &REACTIONS[r];
        bool any = x->eyes != RE_KEEP || x->brows != RB_NONE || x->mouth != RM_KEEP || x->glow != RG_NONE ||
                   x->move != RV_NONE || x->arms != RA_KEEP || x->back || x->front;
        CHECK(any, "%s has an empty row", muse_anim_name(r));
    }
}

/* Gallery review: the sun's rays go all the way round, in the same places every frame. */
static void test_sun_rays_ring_the_sun(void)
{
    avatar_t j = { .cx = 32, .cy = 33.5f, .a = 16, .b = 23 };
    int sx = iround(j.cx + 19), sy = iround(j.cy - j.b + 1);
    static const int8_t RAYS[8][2] = { { 5, 0 }, { 4, 4 }, { 0, 5 }, { -4, 4 }, { -5, 0 }, { -4, -4 }, { 0, -5 }, { 4, -4 } };
    for (float t = 0; t < 3.0f; t += 0.13f) {
        memset(s_fb, C_BG, sizeof(s_fb));
        muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = t, .reaction = MUSE_REACT_SUNNY, .react_amount = 1 };
        prop_sun(&j, &p, 1.0f);
        for (int k = 0; k < 8; k++) {
            CHECK(get_px(sx + RAYS[k][0], sy + RAYS[k][1]) == C_SUN, "ray %d missing at t=%.2f", k, t);
        }
    }
}

static int count(uint8_t c)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        n += s_fb[i] == c;
    }
    return n;
}

/* Review fix: in thinking and listening a reaction's brows replace the mode's
 * (one pair, two pixels each), and its front prop clears the thought dots. */
static void test_reaction_brows_replace_the_modes(void)
{
    static const muse_mode_t MODES[] = { MUSE_MODE_THINKING, MUSE_MODE_LISTENING, MUSE_MODE_IDLE };
    for (size_t m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++) {
        for (int r = 1; r < MUSE_ANIM_COUNT; r++) {
            if (REACTIONS[r].brows == RB_NONE) {
                continue;
            }
            render(MODES[m], r, 0, 0);
            CHECK(count(C_BROW) <= 4, "%s in mode %d draws %d brow pixels", muse_anim_name(r),
                  (int)MODES[m], count(C_BROW));
        }
    }
    muse_pose_t p = { .mode = MUSE_MODE_THINKING, .t = 1, .reaction = MUSE_REACT_CONFUSED, .react_amount = 1 };
    CHECK(react_hides_dots(&p), "the ? and the thought dots share a corner");
    p.reaction = MUSE_REACT_SUNNY;
    CHECK(!react_hides_dots(&p), "no front prop: the dots stay");
    p.reaction = MUSE_REACT_NONE;
    CHECK(!react_hides_dots(&p), "no reaction");
}

/* The scenes' motion, at known moments. */
static void test_activity_motion(void)
{
    muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = 50, .react_amount = 1 };
    float bob, lean, hop;
#define MOTION(id, rt) (p.reaction = (id), p.react_t = (rt), bob = lean = hop = 0, react_motion(&p, &bob, &lean, &hop))
    /* Pace: 9 px to a side by 2.3 s, back by 5.1 s, never further (Review Focus 5). */
    MOTION(MUSE_ACT_PACE, 2.5f);
    CHECK(fabsf(lean) >= 8.9f && fabsf(lean) <= 9.0f, "pace reaches the side (lean %.1f)", lean);
    for (float rt = 0; rt < 6.0f; rt += 0.05f) {
        MOTION(MUSE_ACT_PACE, rt);
        CHECK(fabsf(lean) <= 9.0f, "pace goes too far at %.2f", rt);
    }
    MOTION(MUSE_ACT_PACE, 5.5f);
    CHECK(fabsf(lean) < 0.01f, "pace ends in the middle");
    /* The dance bounces on the beat and sways. */
    MOTION(MUSE_ACT_DANCE, 0.25f);
    CHECK(hop > 1.5f, "dance bounces");
    float l1 = lean;
    MOTION(MUSE_ACT_DANCE, 1.5f);
    CHECK(l1 * lean < 0, "dance sways both ways");
    /* Doze snaps awake at 5 s. */
    MOTION(MUSE_ACT_DOZE, 5.05f);
    CHECK(hop > 1.0f, "doze snaps awake");
    /* The butterfly crosses the whole canvas; the hop follows it. */
    float x0, y0, x1, y1;
    p.reaction = MUSE_ACT_BUTTERFLY;
    p.react_t = 0;
    butterfly_at(&p, &x0, &y0);
    p.react_t = 4.6f;
    butterfly_at(&p, &x1, &y1);
    CHECK(x0 < 0 && x1 > W, "butterfly crosses (%.0f to %.0f)", x0, x1);
    MOTION(MUSE_ACT_BUTTERFLY, 3.3f);
    CHECK(hop > 2.0f, "a hop after the butterfly");
#undef MOTION
}

/* The dance wears headphones: outline pixels above the face that the plain idle frame lacks. */
static void test_dance_wears_headphones(void)
{
    render(MUSE_MODE_IDLE, 0, 0, 0);
    static uint8_t plain[W * H];
    memcpy(plain, s_frame, sizeof(plain));
    render(MUSE_MODE_IDLE, MUSE_ACT_DANCE, 0, 0);
    int band = 0;
    for (int i = 0; i < W * H; i++) {
        band += s_frame[i] == C_OUT && plain[i] != C_OUT;
    }
    CHECK(band >= 20, "the headphones barely show (%d new outline pixels)", band);
}

int main(void)
{
    test_table();
    test_reaction_brows_replace_the_modes();
    test_sun_rays_ring_the_sun();
    test_each_reaction_shows();
    test_some_modes_ignore_reactions();
    test_speech_keeps_its_mouth();
    test_petting_wins();
    test_activity_motion();
    test_dance_wears_headphones();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
