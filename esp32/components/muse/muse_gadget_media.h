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
 * The Now Playing tile (media.update, CONFIG_MUSE_GADGET_COMMANDS with spoken
 * replies), left of the avatar: the track and cover Muse sends, back,
 * play/pause and skip buttons, and a tap anywhere else to refresh. A press is
 * a quiet turn to Muse (muse_hatch_quiet_turn), answered with a media.update.
 */

#include "lvgl.h"
#include "muse_now_playing.h"

/* Builds the tile's contents on `tile`, w x h. The UI task, once. */
void muse_media_build(lv_obj_t *tile, int w, int h);

/* The UI task, each frame: shows what media.update sent, swaps in a new
 * cover, and times a press's wait and the status line. */
void muse_media_frame(void);

/* media.update, already checked (muse_gadget_cmds.c). Any task. */
muse_np_art_t muse_media_update(const muse_np_fields_t *f);
