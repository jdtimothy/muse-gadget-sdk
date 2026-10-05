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
 * muse_chat_session.cpp) and the gadget's sound player (muse_gadget_play.c)
 * call it; muse_tts_start() gives one of them the fetch at a time.
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

#define MUSE_TTS_VOICE_ID_MAX 32
#define MUSE_TTS_VOICE_NAME_MAX 64

/*
 * The voice replies are spoken in: the one muse_tts_set_voice() saved, else
 * the build's (CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID, name unknown). Copies its
 * ID (MUSE_TTS_VOICE_ID_MAX + 1 bytes) and name (MUSE_TTS_VOICE_NAME_MAX + 1,
 * empty when unknown); either may be NULL.
 */
void muse_tts_voice(char *id, char *name);

/* Saves the voice in NVS; replies use it from the next one. */
bool muse_tts_set_voice(const char *id, const char *name);

/*
 * GET /v1/voices: the HTTP status, -1 when ElevenLabs can't be reached or the
 * list is cut short or too big, 0 with no key. *body (NUL-terminated, PSRAM)
 * is the caller's to free(). Blocks for the download: call from a task of
 * your own.
 */
int muse_tts_fetch_voices(char **body, size_t *len);

#ifdef __cplusplus
}
#endif
