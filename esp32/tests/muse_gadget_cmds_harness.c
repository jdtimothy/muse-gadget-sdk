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

/* Host harness for the pure section of components/muse/muse_gadget_cmds.c. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "gadget_pure.inc"

static int failures;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static const gadget_settings_t BASE = { 50, false, 80, 120 };

static bool parse(const char *json, gadget_settings_t *s, char *err)
{
    cJSON *p = NULL;
    if (json) {
        p = cJSON_Parse(json);
        if (!p) {
            fprintf(stderr, "bad test JSON: %s\n", json);
            exit(2);
        }
    }
    err[0] = '\0';
    bool ok = parse_settings(p, s, err, 160);
    cJSON_Delete(p);
    return ok;
}

static bool same(const gadget_settings_t *a, const gadget_settings_t *b)
{
    return a->volume == b->volume && a->muted == b->muted &&
           a->brightness == b->brightness && a->sleep_s == b->sleep_s;
}

static void test_parse_settings(void)
{
    char err[160];
    gadget_settings_t s = BASE;

    CHECK(parse(NULL, &s, err) && same(&s, &BASE));
    CHECK(parse("{}", &s, err) && same(&s, &BASE));

    CHECK(parse("{\"volume\":55}", &s, err) && s.volume == 55);
    CHECK(parse("{\"volume\":0}", &s, err) && s.volume == 0);
    CHECK(parse("{\"volume\":100}", &s, err) && s.volume == 100);

    s = BASE;
    CHECK(!parse("{\"volume\":101}", &s, err) && strstr(err, "volume") && same(&s, &BASE));
    CHECK(!parse("{\"volume\":-1}", &s, err) && same(&s, &BASE));
    CHECK(!parse("{\"volume\":50.5}", &s, err) && same(&s, &BASE));
    CHECK(!parse("{\"volume\":\"50\"}", &s, err) && same(&s, &BASE));
    CHECK(!parse("{\"volume\":1e12}", &s, err) && same(&s, &BASE));

    CHECK(parse("{\"muted\":true}", &s, err) && s.muted);
    CHECK(parse("{\"muted\":false}", &s, err) && !s.muted);
    CHECK(!parse("{\"muted\":1}", &s, err) && strstr(err, "muted"));

    s = BASE;
    CHECK(parse("{\"brightness\":10}", &s, err) && s.brightness == 10);
    CHECK(!parse("{\"brightness\":5}", &s, err) && strstr(err, "brightness") && s.brightness == 10);

    s = BASE;
    CHECK(parse("{\"screen_sleep_s\":300}", &s, err) && s.sleep_s == 300);
    CHECK(parse("{\"screen_sleep_s\":0}", &s, err) && s.sleep_s == 0);
    CHECK(!parse("{\"screen_sleep_s\":45}", &s, err) && strstr(err, "screen_sleep_s") && s.sleep_s == 0);

    /* Review focus 1: a misspelled parameter is refused, not ignored. */
    s = BASE;
    CHECK(!parse("{\"volume_percent\":40}", &s, err) && strstr(err, "volume_percent") && same(&s, &BASE));

    /* Review focus 2: one bad parameter changes nothing, the good one included. */
    s = BASE;
    CHECK(!parse("{\"volume\":40,\"brightness\":3}", &s, err) && same(&s, &BASE));

    /* Several good ones apply together. */
    s = BASE;
    CHECK(parse("{\"volume\":60,\"muted\":false,\"brightness\":30,\"screen_sleep_s\":600}", &s, err));
    CHECK(s.volume == 60 && !s.muted && s.brightness == 30 && s.sleep_s == 600);

    s = BASE;
    CHECK(!parse("[1]", &s, err) && same(&s, &BASE));
}

static void test_settings_result(void)
{
    gadget_settings_t s = { 60, true, 30, 0 };
    cJSON *r = settings_result(&s, 87, true, false, "George");
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    CHECK(cJSON_GetObjectItem(p, "volume")->valueint == 60);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p, "muted")));
    CHECK(cJSON_GetObjectItem(p, "brightness")->valueint == 30);
    CHECK(cJSON_GetObjectItem(p, "screen_sleep_s")->valueint == 0);
    CHECK(cJSON_GetObjectItem(p, "battery_percent")->valueint == 87);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p, "charging")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(p, "on_usb")));
    CHECK(!strcmp(cJSON_GetObjectItem(p, "voice")->valuestring, "George"));
    cJSON_Delete(r);

    r = settings_result(&s, -1, false, true, NULL);
    p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(p, "battery_percent")));
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(p, "voice")));
    cJSON_Delete(r);

    r = settings_result(&s, 50, false, false, "");
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "payload"), "voice")));
    cJSON_Delete(r);
}

static void test_gadget_error(void)
{
    cJSON *r = gadget_error("invalid_params", "volume must be 0-100");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    cJSON *e = cJSON_GetObjectItem(r, "error");
    CHECK(!strcmp(cJSON_GetObjectItem(e, "code")->valuestring, "invalid_params"));
    CHECK(!strcmp(cJSON_GetObjectItem(e, "message")->valuestring, "volume must be 0-100"));
    cJSON_Delete(r);
}

int main(void)
{
    test_parse_settings();
    test_settings_result();
    test_gadget_error();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
