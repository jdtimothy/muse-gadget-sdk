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

/* Host previews of the avatar's reactions (tools/muse/react_gallery.py): plays
 * the states they're judged against, each reaction and some speaking blends
 * through the real renderer at the device frame rate, writing each as raw
 * RGB888 frames, <dir>/<name>.rgb, and a "<name> <frames> <size>" line on stdout.
 * Build: clang -O2 -I components/muse tools/muse/react_anim.c avatar/muse_pixel.c -lm */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "muse_pixel.h"
#include "muse_reactions.h"

#define S 3                     /* screen pixels a cell, as the board's full-size avatar */
#define N (MUSE_PX_W * S)
#define DT 0.04f                /* 40 ms, as muse_ui */
#define WARMUP 1.5f             /* palette blending and eyes settle first */
#define REACT_AT 0.2f           /* a reaction starts this far in */
#define REACT_SECS 4.0f

static uint16_t s_buf[N * N];
static uint8_t s_rgb[N * N * 3];

typedef struct {
    const char *name;
    muse_mode_t mode;
    float secs;
    float start_mode_t;
    bool talky;      /* drive `level` like live speech */
    bool pet;        /* a petting partway through */
    int reaction;
} anim_t;

/* As tools/muse/anim.c: a bursty syllable-like envelope in 0..1. */
static float speech_level(float t)
{
    float syl = fabsf(sinf(t * 6.3f)) * (0.55f + 0.45f * sinf(t * 1.7f + 1.0f));
    float gap = sinf(t * 0.9f) > -0.6f ? 1.0f : 0.15f;
    float v = syl * gap;
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* As muse_state_happiness(). */
static float happiness(float since_pet)
{
    float left = 1.6f - since_pet;
    if (since_pet < 0 || left <= 0) {
        return 0;
    }
    return left > 0.4f ? 1.0f : left / 0.4f;
}

static void play(const char *dir, const anim_t *an, float base)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.rgb", dir, an->name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    int frames = (int)(an->secs / DT + 0.5f), warm = (int)(WARMUP / DT + 0.5f);
    for (int i = -warm; i < frames; i++) {
        float rt = i * DT;
        muse_pose_t p = {
            .mode = an->mode,
            .t = base + rt,
            .mode_t = an->start_mode_t + rt,
            .level = an->talky ? speech_level(rt) : 0,
            .happy = an->pet ? happiness(rt - 0.5f) : 0,
        };
        if (p.mode_t < 0) {
            p.mode_t = 0;
        }
        if (an->reaction) {
            p.reaction = an->reaction;
            p.react_t = rt - REACT_AT;
            p.react_amount = muse_reaction_amount(rt - REACT_AT, REACT_SECS);
        }
        muse_pixel_render(&p);
        if (i < 0) {
            continue;
        }
        muse_pixel_scale(s_buf, N, 0, N - 1, 0, N - 1);
        for (int k = 0; k < N * N; k++) {
            uint16_t c = s_buf[k];
            s_rgb[3 * k] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            s_rgb[3 * k + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            s_rgb[3 * k + 2] = (uint8_t)((c & 31) * 255 / 31);
        }
        fwrite(s_rgb, 1, sizeof(s_rgb), f);
    }
    fclose(f);
    printf("%s %d %d\n", an->name, frames, N);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    muse_pixel_set_size(N);
    static const anim_t REF[] = {
        { "idle", MUSE_MODE_IDLE, 4.0f, 5 },
        { "listening", MUSE_MODE_LISTENING, 4.0f, 0, .talky = true },
        { "thinking", MUSE_MODE_THINKING, 4.0f, 0 },
        { "speaking", MUSE_MODE_SPEAKING, 4.0f, 0, .talky = true },
        { "happy", MUSE_MODE_IDLE, 3.0f, 9, .pet = true },
        { "error", MUSE_MODE_ERROR, 3.0f, 0 },
    };
    float base = 10.0f;
    for (size_t i = 0; i < sizeof(REF) / sizeof(REF[0]); i++, base += 100) {
        play(dir, &REF[i], base);
    }
    float secs = REACT_AT + REACT_SECS + 0.4f;   /* in, held, out, and a moment after */
    for (int r = 1; r < MUSE_REACT_COUNT; r++, base += 100) {
        anim_t an = { MUSE_REACTION_NAMES[r], MUSE_MODE_IDLE, secs, 5, .reaction = r };
        play(dir, &an, base);
    }
    static const int BLENDS[] = { MUSE_REACT_LOVE, MUSE_REACT_RAINY, MUSE_REACT_SAD, MUSE_REACT_SUNNY };
    for (size_t i = 0; i < sizeof(BLENDS) / sizeof(BLENDS[0]); i++, base += 100) {
        char name[48];
        snprintf(name, sizeof(name), "speaking-%s", MUSE_REACTION_NAMES[BLENDS[i]]);
        anim_t an = { name, MUSE_MODE_SPEAKING, secs, 0, .talky = true, .reaction = BLENDS[i] };
        play(dir, &an, base);
    }
    /* Muse calls tools while it thinks, so many reactions arrive then. */
    static const int THINKS[] = { MUSE_REACT_SURPRISED, MUSE_REACT_CONFUSED, MUSE_REACT_SAD, MUSE_REACT_RAINY };
    for (size_t i = 0; i < sizeof(THINKS) / sizeof(THINKS[0]); i++, base += 100) {
        char name[48];
        snprintf(name, sizeof(name), "thinking-%s", MUSE_REACTION_NAMES[THINKS[i]]);
        anim_t an = { name, MUSE_MODE_THINKING, secs, 0, .reaction = THINKS[i] };
        play(dir, &an, base);
    }
    return 0;
}
