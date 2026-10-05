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
 * Reactions (avatar.react, muse_reactions.h), included by avatar/muse_pixel.c
 * just before its frame code, so they draw with its own stamp(), px(),
 * palette and Bayer dither and match its hearts and "!". One row per reaction
 * in REACTIONS says what it changes; the react_*() hooks muse_pixel_render()
 * calls apply that row.
 *
 * A reaction shows in idle, listening, thinking and speaking. Its face waits
 * until it's mostly in (react_amount 0.3) and gives way to petting; its mouth
 * shows only in idle and thinking, so speech keeps its lip-sync. Props and
 * glow fade with react_amount.
 */
#include "muse_reactions.h"

typedef enum { RE_KEEP, RE_BEAD, RE_WIDE, RE_HAPPY, RE_SHUT, RE_HALF, RE_SQUINT, RE_WINK, RE_SHADES } react_eyes_t;
typedef enum { RB_NONE, RB_UP, RB_WORRY, RB_ANGRY, RB_QUIZ } react_brows_t;
typedef enum { RM_KEEP, RM_SMILE, RM_GRIN, RM_O, RM_HMM, RM_FLAT, RM_FROWN, RM_WOBBLE } react_mouth_t;
typedef enum { RV_NONE, RV_BOUNCE, RV_HOP, RV_STARTLE, RV_SWAY, RV_NOD, RV_SHAKE, RV_SHIVER } react_move_t;
typedef enum { RA_KEEP, RA_UP, RA_HOLD } react_arms_t;
typedef enum { RG_NONE, RG_PINK, RG_BLUE, RG_GOLD, RG_STORM, RG_ICE } react_glow_t;

typedef void (*react_prop_t)(const avatar_t *j, const muse_pose_t *p, float a);

typedef struct {
    react_eyes_t eyes;
    react_brows_t brows;
    react_mouth_t mouth;
    float blush;          /* added to the cheeks */
    react_glow_t glow;
    react_move_t move;
    react_arms_t arms;
    react_prop_t back;    /* drawn behind Muse */
    react_prop_t front;   /* and in front */
} reaction_t;

/* Glow ramps (bright -> deep) and accents, blended over the mode's. */
static const scheme_t REACT_GLOWS[] = {
    [RG_PINK] = { { 0xffe8f2, 0xff9cc8, 0xff5c9a, 0xc2306b }, 0xff7ab0 },
    [RG_BLUE] = { { 0xe6f0ff, 0x9cc0ff, 0x5a86e0, 0x2f4fa8 }, 0x7aa4f0 },
    [RG_GOLD] = { { 0xfff6d6, 0xffd97a, 0xffb23f, 0xd9781f }, 0xffc24f },
    [RG_STORM] = { { 0xe0dcf0, 0x9a92c0, 0x605a8c, 0x2e2a52 }, 0x8a82b8 },
    [RG_ICE] = { { 0xf2fcff, 0xc0ecff, 0x7ccfee, 0x3a8fc0 }, 0xa0e0ff },
};

/* ---- Drawing helpers ---- */

/* A pixel that shows with probability a (dithered), so props fade in and out. */
static inline void fpx(int x, int y, uint8_t c, float a)
{
    if (a >= 1.0f || bayer(x, y) < a) {
        px(x, y, c);
    }
}

/* stamp(), faded by a. */
static void fstamp(const char *const *rows, int nrows, int x0, int y0, uint8_t fill, uint8_t alt, float a)
{
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; rows[r][c]; c++) {
            if (rows[r][c] == '#') {
                fpx(x0 + c, y0 + r, fill, a);
            } else if (rows[r][c] == 'o') {
                fpx(x0 + c, y0 + r, alt, a);
            }
        }
    }
}

/* Sky: rain, snow and wind only go where Muse isn't. */
static inline bool is_sky(int x, int y)
{
    uint8_t c = get_px(x, y);
    return (unsigned)x < W && (unsigned)y < H && (c == C_BG || c == C_AURA1 || c == C_AURA2 || c == C_SHADOW);
}

/* The face's layout, as muse_pixel_render() sets it. */
static inline float face_eye_y(const avatar_t *j)
{
    return j->fy - 0.5f;
}

static inline float face_eye_dx(const avatar_t *j)
{
    return j->fa * 0.48f;
}

static inline float head_top(const avatar_t *j)
{
    return j->cy - j->b;
}

static const char *const DROP[] = { ".#.", ".#.", "#o#", "###", ".#." };

/* ---- Props ---- */

static void prop_hearts(const avatar_t *j, const muse_pose_t *p, float a)
{
    draw_hearts(j->cx, head_top(j) + 1, p->t, a);
}

static void prop_twinkles(const avatar_t *j, const muse_pose_t *p, float a)
{
    float top = head_top(j);
    draw_sparkle(iround(j->cx - 18), iround(top + 5), (0.5f + 0.5f * sinf(p->t * 7.0f)) * a, true);
    draw_sparkle(iround(j->cx + 17), iround(top + 2), (0.5f + 0.5f * sinf(p->t * 7.0f + 2.1f)) * a, true);
}

static void prop_burst(const avatar_t *j, const muse_pose_t *p, float a)
{
    for (int i = 0; i < 6; i++) {
        float ang = i * TAU / 6 + p->t * 0.8f;
        float r = 22.0f + 2.0f * sinf(p->t * 6.0f + i);
        int x = iround(j->cx + cosf(ang) * r), y = iround(j->cy - 4 + sinf(ang) * r * 0.8f);
        draw_sparkle(x, y, (0.5f + 0.5f * sinf(p->t * 9.0f + i * 1.3f)) * a, true);
    }
}

static void prop_bang(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)p;
    if (a > 0.3f) {
        draw_alert(iround(j->cx + 18), iround(head_top(j) - 1));
    }
}

static void prop_question(const avatar_t *j, const muse_pose_t *p, float a)
{
    static const char *const QMARK[] = { ".##.", "#..#", "...#", "..#.", "..#.", "....", "..#." };
    fstamp(QMARK, 7, iround(j->cx + 15), iround(head_top(j) - 2 + sinf(p->t * 3.0f)), C_ACC, C_ACC, a);
}

static void prop_tear(const avatar_t *j, const muse_pose_t *p, float a)
{
    float slide = fracf(p->react_t * 0.45f);
    fstamp(DROP, 5, iround(j->fx - face_eye_dx(j)) - 1, iround(face_eye_y(j) + 1 + slide * 5), C_WATER, C_WHITE,
           a * (1.0f - slide * 0.6f));
}

static void prop_steam(const avatar_t *j, const muse_pose_t *p, float a)
{
    static const char *const PUFF[] = { ".##.", "####", ".##." };
    for (int i = 0; i < 2; i++) {
        float ph = fracf(p->react_t * 0.8f + i * 0.5f);
        int x = iround(j->cx + (i ? 6 : -9) + sinf(ph * TAU) * 1.5f);
        int y = iround(head_top(j) - 1 - ph * 7);
        fstamp(PUFF, 3, x, y, C_CLOUD, C_CLOUD, a * (1.0f - ph));
    }
}

static void prop_zzz(const avatar_t *j, const muse_pose_t *p, float a)
{
    /* A z needs room for its diagonal: 4 and 5 pixels, never 3 (that reads as an I). */
    static const char *const BIG[] = { "#####", "...#.", "..#..", ".#...", "#####" };
    static const char *const SMALL[] = { "####", "..#.", ".#..", "####" };
    /* Two, half a cycle apart so they never overlap, drifting up and out and
     * growing as they go. Not dithered out: a broken diagonal stops reading as a z. */
    for (int i = 0; i < 2; i++) {
        float ph = fracf(p->react_t * 0.4f + i * 0.5f);
        if (ph > 0.9f) {
            continue;
        }
        int x = iround(j->cx + 11 + ph * 10), y = iround(head_top(j) + 4 - ph * 14);
        if (ph > 0.45f) {
            fstamp(BIG, 5, x, y, C_ACC, C_ACC, a);
        } else {
            fstamp(SMALL, 4, x, y, C_ACC, C_ACC, a);
        }
    }
}

static void prop_sweat(const avatar_t *j, const muse_pose_t *p, float a)
{
    fstamp(DROP, 5, iround(j->cx + 12), iround(head_top(j) + 4 + sinf(p->t * 2.0f) * 0.6f), C_WATER, C_WHITE, a);
}

static void prop_wink(const avatar_t *j, const muse_pose_t *p, float a)
{
    draw_sparkle(iround(j->fx - face_eye_dx(j) - 5), iround(face_eye_y(j) - 4),
                 (0.6f + 0.4f * sinf(p->t * 6.0f)) * a, true);
}

static void prop_check(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)p;
    static const char *const TICK[] = { ".....#", "....##", "#..##.", "####..", ".##..." };
    fstamp(TICK, 5, iround(j->cx + 14), iround(head_top(j) - 1), C_GREEN, C_GREEN, a);
}

static void prop_cross(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)p;
    static const char *const CROSS[] = { "#...#", ".#.#.", "..#..", ".#.#.", "#...#" };
    fstamp(CROSS, 5, iround(j->cx + 15), iround(head_top(j) - 1), C_RED, C_RED, a);
}

static void prop_confetti(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)j;
    static const uint8_t COLORS[] = { C_RED, C_YELLOW, C_GREEN, C_BLUE, C_HEART, C_WHITE };
    for (int i = 0; i < 14; i++) {
        float seed = (float)hash16(i, 7) / 65536.0f;
        float ph = fracf(p->react_t * (0.35f + seed * 0.25f) + seed);
        int x = iround(4 + seed * 56 + sinf(p->t * 3.0f + i) * 1.5f), y = iround(ph * 60);
        uint8_t c = COLORS[i % 6];
        fpx(x, y, c, a);
        fpx(x + (i & 1), y + !(i & 1), c, a);
    }
}

static void prop_sun(const avatar_t *j, const muse_pose_t *p, float a)
{
    int cx = iround(j->cx + 19), cy = iround(head_top(j) + 1);
    for (int y = -3; y <= 3; y++) {
        for (int x = -3; x <= 3; x++) {
            int d = x * x + y * y;
            if (d <= 10) {
                fpx(cx + x, cy + y, d <= 4 ? C_SUN : C_SUN_D, a);
            }
        }
    }
    /* Eight rays all the way round, where they stay; the straight and the
     * diagonal ones take turns reaching out a pixel further, so the sun glows. */
    static const int8_t RAYS[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
    bool straight_long = sinf(p->t * 3.0f) > 0;
    for (int k = 0; k < 8; k++) {
        bool diagonal = k & 1;
        int from = diagonal ? 4 : 5;   /* a diagonal step is longer: start it a step in */
        int len = diagonal != straight_long ? 2 : 1;
        for (int r = from; r < from + len; r++) {
            fpx(cx + RAYS[k][0] * r, cy + RAYS[k][1] * r, C_SUN, a);
        }
    }
}

static void prop_rain(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)j;
    for (int i = 0; i < 16; i++) {
        float seed = (float)hash16(i, 3) / 65536.0f;
        int x = (int)(seed * W), y = iround(fracf(p->t * 1.1f + seed * 3.0f) * 70) - 4;
        for (int k = 0; k < 2; k++) {
            if (is_sky(x, y + k)) {
                fpx(x, y + k, k ? C_WATER : C_WATER_D, a);
            }
        }
    }
}

/* A red umbrella held up to the right, its paw by react_arms(RA_HOLD). */
static void prop_umbrella(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)p;
    static const char *const CANOPY[] = {
        "......#########......",
        "....##ooooooooo##....",
        "..##ooooooooooooo##..",
        ".#ooooooooooooooooo#.",
        "#ooooooooooooooooooo#",
        "#o#o#o#o#o#o#o#o#o#o#",
    };
    int x0 = iround(j->cx + 3), y0 = iround(head_top(j) - 8);
    fstamp(CANOPY, 6, x0, y0, C_OUT, C_RED, a);
    for (int y = y0 + 6; y <= iround(j->cy - 9); y++) {
        fpx(x0 + 10, y, C_OUT, a);
    }
}

static void prop_storm(const avatar_t *j, const muse_pose_t *p, float a)
{
    static const char *const CLOUD[] = {
        ".....###.......",
        "...##ooo##.##..",
        "..#ooooooo#oo#.",
        ".#ooooooooooo#.",
        "#ooooooooooooo#",
        ".#############.",
    };
    static const char *const BOLT[] = { "..##", ".##.", "####", "..#.", ".#..", "#..." };
    int x0 = iround(j->cx - 23), y0 = iround(head_top(j) - 6);
    fstamp(CLOUD, 6, x0, y0, C_CLOUD_D, C_CLOUD, a);
    if (fracf(p->t * 0.7f) < 0.12f) {
        fstamp(BOLT, 6, x0 + 6, y0 + 6, C_YELLOW, C_WHITE, a);
    }
}

static void prop_cold(const avatar_t *j, const muse_pose_t *p, float a)
{
    /* A striped scarf just below the face, over the fur but not the outline. */
    int y0 = iround(j->fy + j->fb + 1.0f);
    for (int row = 0; row < 2; row++) {
        int y = y0 + row;
        for (int x = 0; x < W && (unsigned)y < H; x++) {
            uint8_t m = s_mask[y * W + x];
            if ((m == M_BODY || m == M_FACE) && get_px(x, y) != C_OUT) {
                fpx(x, y, ((x + row) / 2) & 1 ? C_SCARF : C_WHITE, a);
            }
        }
    }
    static const char *const TAIL[] = { "##", "#o", "##", "o#", "##" };
    fstamp(TAIL, 5, iround(j->cx + 5), y0 + 2, C_SCARF, C_WHITE, a);
    for (int i = 0; i < 12; i++) {
        float seed = (float)hash16(i, 11) / 65536.0f;
        int x = iround(seed * W + sinf(p->t * 1.5f + i) * 2), y = iround(fracf(p->t * 0.25f + seed * 2) * 68) - 2;
        if (is_sky(x, y)) {
            fpx(x, y, C_WHITE, a);
        }
        if (i % 3 == 0) {
            static const int8_t ARMS[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4; k++) {
                if (is_sky(x + ARMS[k][0], y + ARMS[k][1])) {
                    fpx(x + ARMS[k][0], y + ARMS[k][1], C_SPK, a);
                }
            }
        }
    }
}

static void prop_wind(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)j;
    static const float ROWS[3] = { 14, 30, 44 };
    for (int k = 0; k < 3; k++) {
        int x0 = iround(-10 + fracf(p->t * 0.7f + k * 0.37f) * 84), y = iround(ROWS[k] + sinf(p->t * 2 + k));
        for (int i = 0; i < 9; i++) {
            if (i != 3 && i != 6 && is_sky(x0 + i, y)) {
                fpx(x0 + i, y, i < 3 ? C_AURA2 : C_SPK, a);
            }
        }
        if (is_sky(x0 + 9, y - 1)) {
            fpx(x0 + 9, y - 1, C_SPK, a);
        }
    }
}

static void prop_heat(const avatar_t *j, const muse_pose_t *p, float a)
{
    prop_sweat(j, p, a);
    for (int k = 0; k < 3; k++) {
        float ph = fracf(p->t * 0.6f + k / 3.0f);
        int x = iround(j->cx - 8 + k * 8), y = iround(head_top(j) - 1 - ph * 6);
        for (int i = 0; i < 4; i++) {
            fpx(x + ((i + (int)(p->t * 6)) & 1), y + i, C_SUN_D, a * (1 - ph));
        }
    }
}

static void prop_rainbow(const avatar_t *j, const muse_pose_t *p, float a)
{
    (void)p;
    static const uint8_t BANDS[] = { C_RED, C_ORANGE, C_YELLOW, C_GREEN, C_BLUE, C_VIOLET };
    float cx = j->cx, cy = j->cy + 6;
    for (int y = 0; y < H && y < cy; y++) {
        for (int x = 0; x < W; x++) {
            float dx = x + 0.5f - cx, dy = (y + 0.5f - cy) * 1.05f;
            int band = (int)(33.0f - sqrtf(dx * dx + dy * dy));
            if (band >= 0 && band < 6 && is_sky(x, y)) {
                fpx(x, y, BANDS[band], a);
            }
        }
    }
}

/* ---- The reactions ---- */

static const reaction_t REACTIONS[MUSE_REACT_COUNT] = {
    /*                     eyes       brows     mouth      blush   glow      move        arms     back          front */
    [MUSE_REACT_LOVE] = { RE_HAPPY, RB_NONE, RM_GRIN, 0.45f, RG_PINK, RV_NONE, RA_KEEP, NULL, prop_hearts },
    [MUSE_REACT_LAUGH] = { RE_HAPPY, RB_NONE, RM_GRIN, 0.25f, RG_NONE, RV_BOUNCE, RA_KEEP, NULL, prop_twinkles },
    [MUSE_REACT_EXCITED] = { RE_WIDE, RB_UP, RM_GRIN, 0.2f, RG_NONE, RV_HOP, RA_UP, NULL, prop_burst },
    [MUSE_REACT_SURPRISED] = { RE_WIDE, RB_UP, RM_O, 0, RG_NONE, RV_STARTLE, RA_KEEP, NULL, prop_bang },
    [MUSE_REACT_CONFUSED] = { RE_BEAD, RB_QUIZ, RM_HMM, 0, RG_NONE, RV_NONE, RA_KEEP, NULL, prop_question },
    [MUSE_REACT_SAD] = { RE_BEAD, RB_WORRY, RM_FROWN, -0.3f, RG_BLUE, RV_NONE, RA_KEEP, NULL, prop_tear },
    [MUSE_REACT_GRUMPY] = { RE_BEAD, RB_ANGRY, RM_FROWN, 0, RG_NONE, RV_NONE, RA_KEEP, NULL, prop_steam },
    [MUSE_REACT_SLEEPY] = { RE_HALF, RB_NONE, RM_O, 0, RG_NONE, RV_SWAY, RA_KEEP, NULL, prop_zzz },
    [MUSE_REACT_NERVOUS] = { RE_WIDE, RB_WORRY, RM_WOBBLE, 0, RG_NONE, RV_NONE, RA_KEEP, NULL, prop_sweat },
    [MUSE_REACT_COOL] = { RE_SHADES, RB_NONE, RM_SMILE, 0, RG_NONE, RV_NONE, RA_KEEP, NULL, NULL },
    [MUSE_REACT_WINK] = { RE_WINK, RB_NONE, RM_SMILE, 0.15f, RG_NONE, RV_NONE, RA_KEEP, NULL, prop_wink },
    [MUSE_REACT_YES] = { RE_KEEP, RB_NONE, RM_SMILE, 0, RG_NONE, RV_NOD, RA_KEEP, NULL, prop_check },
    [MUSE_REACT_NO] = { RE_KEEP, RB_NONE, RM_FLAT, 0, RG_NONE, RV_SHAKE, RA_KEEP, NULL, prop_cross },
    [MUSE_REACT_CELEBRATE] = { RE_HAPPY, RB_NONE, RM_GRIN, 0.3f, RG_NONE, RV_HOP, RA_UP, NULL, prop_confetti },
    [MUSE_REACT_SUNNY] = { RE_KEEP, RB_NONE, RM_SMILE, 0.15f, RG_GOLD, RV_NONE, RA_KEEP, prop_sun, NULL },
    [MUSE_REACT_RAINY] = { RE_KEEP, RB_NONE, RM_SMILE, 0, RG_BLUE, RV_NONE, RA_HOLD, prop_rain, prop_umbrella },
    [MUSE_REACT_STORMY] = { RE_WIDE, RB_WORRY, RM_FLAT, 0, RG_STORM, RV_NONE, RA_KEEP, prop_storm, NULL },
    [MUSE_REACT_COLD] = { RE_SHUT, RB_NONE, RM_FLAT, 0.2f, RG_ICE, RV_SHIVER, RA_KEEP, NULL, prop_cold },
    [MUSE_REACT_WINDY] = { RE_SQUINT, RB_NONE, RM_FLAT, 0, RG_NONE, RV_SWAY, RA_KEEP, NULL, prop_wind },
    [MUSE_REACT_HOT] = { RE_KEEP, RB_NONE, RM_FLAT, 0.45f, RG_GOLD, RV_NONE, RA_KEEP, NULL, prop_heat },
    [MUSE_REACT_RAINBOW] = { RE_HAPPY, RB_NONE, RM_SMILE, 0.15f, RG_NONE, RV_NONE, RA_KEEP, prop_rainbow, NULL },
};

/* ---- Hooks muse_pixel_render() calls ---- */

static bool react_mode_ok(muse_mode_t m)
{
    return m == MUSE_MODE_IDLE || m == MUSE_MODE_LISTENING || m == MUSE_MODE_THINKING || m == MUSE_MODE_SPEAKING;
}

/* The reaction showing, for its glow and props; NULL if none. */
static const reaction_t *react_row(const muse_pose_t *p)
{
    if (p->reaction <= MUSE_REACT_NONE || p->reaction >= MUSE_REACT_COUNT || p->react_amount <= 0 ||
        !react_mode_ok(p->mode)) {
        return NULL;
    }
    return &REACTIONS[p->reaction];
}

/* And for its face, motion and arms: once it's mostly in, and never while
 * petting, which a touch always gets. */
static const reaction_t *react_face_row(const muse_pose_t *p)
{
    const reaction_t *r = react_row(p);
    return r && p->react_amount >= 0.3f && p->happy <= 0.2f ? r : NULL;
}

/* The glow to aim for: the mode's, blended toward the reaction's. */
static const scheme_t *react_scheme(const muse_pose_t *p, const scheme_t *base)
{
    const reaction_t *r = react_row(p);
    if (!r || r->glow == RG_NONE) {
        return base;
    }
    static scheme_t mixed;
    const scheme_t *to = &REACT_GLOWS[r->glow];
    for (int i = 0; i < 5; i++) {
        rgb_t c = mix(hex_rgb(i < 4 ? base->f[i] : base->acc), hex_rgb(i < 4 ? to->f[i] : to->acc), p->react_amount);
        uint32_t v = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
        if (i < 4) {
            mixed.f[i] = v;
        } else {
            mixed.acc = v;
        }
    }
    return &mixed;
}

static void react_motion(const muse_pose_t *p, float *bob, float *lean, float *hop)
{
    const reaction_t *r = react_face_row(p);
    if (!r) {
        return;
    }
    float a = p->react_amount, t = p->t, rt = p->react_t;
    switch (r->move) {
    case RV_BOUNCE: *hop += fabsf(sinf(t * 12.0f)) * 1.5f * a; break;
    case RV_HOP: *hop += fabsf(sinf(t * 9.0f)) * 3.0f * a; break;
    case RV_STARTLE: *hop += 3.0f * a * expf(-rt * 5.0f); break;
    case RV_SWAY: *lean += sinf(t * 1.6f) * 1.5f * a; break;
    case RV_NOD: *bob += sinf(rt * 9.0f) * 1.6f * a; break;
    case RV_SHAKE: *lean += sinf(rt * 14.0f) * 1.6f * a; break;
    case RV_SHIVER: *lean += sinf(t * 40.0f) * 0.6f * a; break;
    default: break;
    }
}

static void react_arms(const muse_pose_t *p, const avatar_t *j, limb_t arms[2])
{
    const reaction_t *r = react_row(p);
    if (!r || p->happy > 0.2f) {
        return;
    }
    float adx = j->a + 0.3f;
    if (r->arms == RA_UP && p->react_amount >= 0.3f) {
        float wig = sinf(p->t * 14.0f) * 0.25f;
        arms[0] = (limb_t){ j->cx - adx - 1.0f, j->cy - 4.0f, 2.4f + wig };
        arms[1] = (limb_t){ j->cx + adx + 1.0f, j->cy - 4.0f, -2.4f - wig };
    } else if (r->arms == RA_HOLD) {
        arms[1] = (limb_t){ j->cx + 13.0f, j->cy - 5.0f, -0.15f };   /* holding the umbrella's shaft */
    }
}

static void react_prop_back(const muse_pose_t *p, const avatar_t *j)
{
    const reaction_t *r = react_row(p);
    if (r && r->back) {
        r->back(j, p, p->react_amount);
    }
}

static void react_prop_front(const muse_pose_t *p, const avatar_t *j)
{
    const reaction_t *r = react_row(p);
    if (r && r->front) {
        r->front(j, p, p->react_amount);
    }
}

/* Draws both eyes the reaction's way; false leaves them to the mode. */
static bool react_eyes(const muse_pose_t *p, float lx, float rx, float ey, float open, float gx, float gy)
{
    const reaction_t *r = react_face_row(p);
    if (!r || r->eyes == RE_KEEP) {
        return false;
    }
    switch (r->eyes) {
    case RE_BEAD:
    case RE_WIDE:
    case RE_HAPPY: {
        eye_style_t s = r->eyes == RE_WIDE ? EYES_WIDE : r->eyes == RE_HAPPY ? EYES_HAPPY : EYES_NORMAL;
        draw_eye(lx, ey, open, s, gx, gy);
        draw_eye(rx, ey, open, s, gx, gy);
        break;
    }
    case RE_SHUT:
        draw_eye(lx, ey, 0, EYES_NORMAL, 0, 0);
        draw_eye(rx, ey, 0, EYES_NORMAL, 0, 0);
        break;
    case RE_HALF: {
        float o = open < 0.45f ? open : 0.45f;
        draw_eye(lx, ey, o, EYES_NORMAL, gx, 0.4f);
        draw_eye(rx, ey, o, EYES_NORMAL, gx, 0.4f);
        break;
    }
    case RE_SQUINT: {
        static const char *const L[] = { "##..", "..##" };
        static const char *const R[] = { "..##", "##.." };
        stamp(L, 2, iround(lx) - 2, iround(ey), C_IRIS, C_IRIS);
        stamp(R, 2, iround(rx) - 2, iround(ey), C_IRIS, C_IRIS);
        break;
    }
    case RE_WINK:
        draw_eye(lx, ey, 1, EYES_HAPPY, 0, 0);
        draw_eye(rx, ey, open, EYES_NORMAL, gx, gy);
        break;
    case RE_SHADES: {
        int l = iround(lx), rr = iround(rx), y = iround(ey) - 1;
        for (int x = l - 3; x <= rr + 2; x++) {
            px(x, y, C_OUT);   /* the frame's top bar */
        }
        for (int k = 1; k <= 2; k++) {
            for (int x = l - 3; x <= l + 1; x++) {
                px(x, y + k, C_IRIS);
            }
            for (int x = rr - 2; x <= rr + 2; x++) {
                px(x, y + k, C_IRIS);
            }
        }
        px(l - 2, y + 1, C_SHINE);
        px(rr - 1, y + 1, C_SHINE);
        break;
    }
    default:
        return false;
    }
    return true;
}

static void react_brows(const muse_pose_t *p, int bl, int br, int by)
{
    const reaction_t *r = react_face_row(p);
    if (!r) {
        return;
    }
    switch (r->brows) {
    case RB_UP:
        px(bl - 1, by - 2, C_BROW); px(bl, by - 2, C_BROW);
        px(br - 1, by - 2, C_BROW); px(br, by - 2, C_BROW);
        break;
    case RB_WORRY:   /* inner ends up */
        px(bl - 1, by, C_BROW); px(bl, by - 1, C_BROW);
        px(br - 1, by - 1, C_BROW); px(br, by, C_BROW);
        break;
    case RB_ANGRY:   /* inner ends down */
        px(bl - 1, by - 1, C_BROW); px(bl, by, C_BROW);
        px(br - 1, by, C_BROW); px(br, by - 1, C_BROW);
        break;
    case RB_QUIZ:    /* one up */
        px(bl - 1, by, C_BROW); px(bl, by, C_BROW);
        px(br - 1, by - 1, C_BROW); px(br, by - 2, C_BROW);
        break;
    default:
        break;
    }
}

static float react_blush(const muse_pose_t *p, float blush)
{
    const reaction_t *r = react_face_row(p);
    float b = r ? blush + r->blush : blush;
    return b < 0 ? 0 : b > 1 ? 1 : b;
}

/* Draws the reaction's mouth: only in idle and thinking, so listening and
 * speech keep theirs. False leaves the mouth to the mode. */
static bool react_mouth(const muse_pose_t *p, int x, int y)
{
    const reaction_t *r = react_face_row(p);
    if (!r || r->mouth == RM_KEEP || (p->mode != MUSE_MODE_IDLE && p->mode != MUSE_MODE_THINKING)) {
        return false;
    }
    switch (r->mouth) {
    case RM_SMILE: draw_mouth(x, y, MOUTH_SMILE, 0); break;
    case RM_GRIN: draw_mouth(x, y, MOUTH_GRIN, 0); break;
    case RM_O: draw_mouth(x, y, MOUTH_O, 0); break;
    case RM_HMM: draw_mouth(x, y, MOUTH_HMM, 0); break;
    case RM_FLAT: draw_mouth(x, y, MOUTH_FLAT, 0); break;
    case RM_FROWN: {
        static const char *const S[] = { ".##.", "#..#" };
        stamp(S, 2, x - 2, y, C_MOUTH, C_MOUTH);
        break;
    }
    case RM_WOBBLE: {
        static const char *const S[] = { ".#.#", "#.#." };
        stamp(S, 2, x - 2, y + 1, C_MOUTH, C_MOUTH);
        break;
    }
    default:
        return false;
    }
    return true;
}
