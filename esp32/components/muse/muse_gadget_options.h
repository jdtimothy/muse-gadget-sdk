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
 * Reply options as buttons (display.options, CONFIG_MUSE_GADGET_COMMANDS with
 * spoken replies): the command sets them, muse_ui.c shows them each frame, and
 * the voice task takes a tap.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"
#include "muse_reply_options.h"

/* Shows these `n` labels once the gadget is idle, replacing any. Any task. */
void muse_options_set(char labels[][MUSE_OPTIONS_LABEL_MAX + 1], int n);

/* The UI task, each frame: shows, hides and times the buttons in the box `h`
 * px tall from `top` (from the screen's centre), `w` px wide, on `parent`.
 * True while they're showing. */
bool muse_options_frame(lv_obj_t *parent, int top, int h, int w);

/* True while buttons are on screen (the UI task). */
bool muse_options_showing(void);

/* The voice task: the tapped option's message and label, if one was tapped
 * since the last call. */
bool muse_options_take_tap(char *message, size_t cap, char *label, size_t label_cap);
