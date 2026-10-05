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
 * Reply options as buttons (display.options): the limits muse_gadget_cmds.c
 * checks, and the message a tap sends, which muse_gadget_options.c builds. No
 * dependencies, so the host tests can use it.
 */

#include <stdio.h>
#include <string.h>

#define MUSE_OPTIONS_MIN 2
#define MUSE_OPTIONS_MAX 4
#define MUSE_OPTIONS_LABEL_MAX 24                        /* bytes of UTF-8 */
#define MUSE_OPTIONS_TAG " [tapped on the gadget]"         /* how Muse knows a tap came from here */
#define MUSE_OPTIONS_MSG_MAX (MUSE_OPTIONS_LABEL_MAX + sizeof(MUSE_OPTIONS_TAG))

/* What a tap on `label` sends Muse: the label, then the tag. */
static inline void muse_options_message(char *out, size_t cap, const char *label)
{
    snprintf(out, cap, "%.24s%s", label, MUSE_OPTIONS_TAG);
}
