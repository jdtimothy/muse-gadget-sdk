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
 * Colours the reactions' props need beyond the avatar's own (muse_reactions.inc).
 * avatar/muse_pixel.c puts the ids before C_COUNT and the values in FIXED.
 */
#define REACT_COLOR_IDS \
    C_WATER, C_WATER_D, C_CLOUD, C_CLOUD_D, C_SUN, C_SUN_D, C_RED, C_ORANGE, C_YELLOW, C_GREEN, C_BLUE, \
    C_VIOLET, C_SCARF,

#define REACT_COLOR_VALUES                                                                  \
    [C_WATER] = 0x9fd8ff, [C_WATER_D] = 0x4f9be0, [C_CLOUD] = 0xe4e2f0, [C_CLOUD_D] = 0x8f8aa8, \
    [C_SUN] = 0xffe066, [C_SUN_D] = 0xf2a01f, [C_RED] = 0xff5a5a, [C_ORANGE] = 0xffa84f,      \
    [C_YELLOW] = 0xffe066, [C_GREEN] = 0x6ee07a, [C_BLUE] = 0x5aa8ff, [C_VIOLET] = 0xa77dff,   \
    [C_SCARF] = 0xe8504f,
