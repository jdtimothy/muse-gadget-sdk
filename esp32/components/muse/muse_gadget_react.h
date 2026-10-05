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
 * The avatar's current reaction (avatar.react, CONFIG_MUSE_GADGET_COMMANDS):
 * set by the command, read by the UI for each frame (muse_ui.c).
 */

/* Shows reaction `id` (muse_reactions.h) for `seconds`, waking the screen;
 * MUSE_REACT_NONE clears. Any task. */
void muse_react_set(int id, int seconds);

/* For a frame: the reaction showing (0 if none), with its seconds in and its
 * strength (muse_reaction_amount). */
int muse_react_pose(float *t, float *amount);
