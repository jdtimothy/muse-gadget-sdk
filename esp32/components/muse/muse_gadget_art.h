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
 * Now Playing's cover art (muse_gadget_media.c, CONFIG_MUSE_GADGET_COMMANDS
 * with spoken replies): downloads a baseline JPEG over http or https, fits it
 * to the tile and dims it (muse_now_playing.h) into the one of two buffers the
 * tile isn't showing, so a half-drawn cover never shows.
 */

#include <stdbool.h>
#include <stdint.h>

/* The tile's size; allocates the two buffers (PSRAM) and starts the download
 * task. False without memory. The UI task, once. */
bool muse_art_init(int w, int h);

/* Downloads and decodes `url` in the background, replacing one in flight and
 * dropping a finished cover not yet taken. NULL just stops and drops. Any task. */
void muse_art_fetch(const char *url);

/* The UI task: a newly finished cover (w x h RGB565, LVGL's byte order), or
 * NULL. It stays untouched until the next non-NULL muse_art_take(). */
const uint16_t *muse_art_take(void);
