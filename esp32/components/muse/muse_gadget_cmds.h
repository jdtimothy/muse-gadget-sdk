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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Commands this gadget offers Muse beyond Home Link's own
 * (CONFIG_MUSE_GADGET_COMMANDS). skills/gadget-muse-s318/SKILL.md tells Muse
 * how to use them.
 */

struct cJSON;

/* Delivers an async command's result and takes it; noise_ctrl_send_command_result fits. */
typedef void (*muse_gadget_send_fn)(uint64_t session_generation, const char *request_id,
                                    struct cJSON *result);

/* Adds this gadget's commands to link.register's commands_v2 object. */
void muse_gadget_add_commands(struct cJSON *commands);

/*
 * Answers one of this gadget's commands: its result, or {"_async": true} when
 * send() delivers the result later. NULL if `command` isn't one of them.
 * Runs on the Noise session's task, so nothing here blocks.
 */
struct cJSON *muse_gadget_command(const char *command, struct cJSON *params,
                                  const char *request_id, uint64_t session_generation,
                                  muse_gadget_send_fn send);

#ifdef __cplusplus
}
#endif
