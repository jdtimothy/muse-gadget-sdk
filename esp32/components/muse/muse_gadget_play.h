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

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "muse_gadget_sound.h"

/*
 * The gadget's sound player (CONFIG_MUSE_GADGET_COMMANDS with spoken
 * replies): voice.say, audio.play_url and audio.chime queue sounds here, and
 * the voice task plays them when it's idle.
 */

/* Queues a sound, copying `arg` (text, https URL or chime name). Returns its
 * place in the queue (1 = next), 0 if MUSE_SOUND_QUEUE are already waiting,
 * -1 out of memory. Any task. */
int muse_play_enqueue(muse_sound_kind_t kind, const char *arg, bool caption);

/* True while sounds wait to play. */
bool muse_play_pending(void);

/*
 * The voice task, idle: plays the queued sounds in order, then goes idle. A
 * talk press on `presses` (muse_input_event_t) stops them and clears the
 * queue: then it returns true with the press taken, for the caller to record.
 */
bool muse_play_run(QueueHandle_t presses);
