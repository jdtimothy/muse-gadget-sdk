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
 * Spoken replies through ElevenLabs (CONFIG_MUSE_TTS_ELEVENLABS_API_KEY).
 * One fetch at a time, on a task of its own: start one with the reply's text,
 * then read its MP3 as it streams in. The voice session (start_tts in
 * muse_chat_session.cpp) is the only caller.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the task. False with no API key set, or if it can't start. */
bool muse_tts_init(void);

/* True once muse_tts_init has succeeded. */
bool muse_tts_enabled(void);

/* Fetches speech for `len` bytes of `text`. Returns the fetch's number, or 0
 * while the last one is still stopping (try again shortly). */
uint32_t muse_tts_start(const char *text, size_t len);

/* Copies up to `cap` bytes of fetch `job`'s MP3 into `dst`, without waiting.
 * When it returns 0 with *ended set, the fetch is over: *ok says whether all
 * of it arrived. */
size_t muse_tts_read(uint32_t job, uint8_t *dst, size_t cap, bool *ended, bool *ok);

/* Stops the current fetch. */
void muse_tts_cancel(void);

#ifdef __cplusplus
}
#endif
