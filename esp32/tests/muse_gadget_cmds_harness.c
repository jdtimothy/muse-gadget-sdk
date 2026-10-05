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
#include <ctype.h>
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

/* The shape of ElevenLabs' GET /v1/voices, with the traps: nested "name" and
 * "voice_id"-like keys, escapes, nulls and arrays. */
static const char VOICES[] =
    "{\"voices\":["
    "{\"voice_id\":\"JBFqnCBsd6RMkjVDRZzb\",\"name\":\"George - Warm, Captivating Storyteller\","
    " \"samples\":null,\"category\":\"premade\",\"fine_tuning\":{\"name\":\"not me\",\"state\":{}},"
    " \"labels\":{\"accent\":\"british\"},\"sharing\":{\"original_voice_id\":\"zzz\"},"
    " \"verified_languages\":[{\"language\":\"en\",\"preview_url\":\"https://x/y\"}],"
    " \"settings\":{\"stability\":0.5,\"use_speaker_boost\":true}},"
    "{\"name\":\"Sarah \\\"Sunny\\\" \\u00e9\",\"voice_id\":\"EXAVITQu4vr4xnSDxMaL\",\"category\":null},"
    "{\"voice_id\":\"pqHfZKP75CvOlQylNhV4\",\"name\":\"Sarah - Mature\",\"category\":\"premade\"},"
    "{\"voice_id\":\"\",\"name\":\"no id\"}"
    "],\"has_more\":false}";

static void test_scan_voices(void)
{
    gadget_voice_t v[8];
    int n = scan_voices(VOICES, strlen(VOICES), v, 8);
    CHECK(n == 3);
    CHECK(!strcmp(v[0].id, "JBFqnCBsd6RMkjVDRZzb"));
    CHECK(!strcmp(v[0].name, "George - Warm, Captivating Storyteller"));
    CHECK(!strcmp(v[0].category, "premade"));
    CHECK(!strcmp(v[1].id, "EXAVITQu4vr4xnSDxMaL"));
    CHECK(!strcmp(v[1].name, "Sarah \"Sunny\" ?"));
    CHECK(v[1].category[0] == '\0');
    CHECK(!strcmp(v[2].name, "Sarah - Mature"));

    /* Review focus 5: more voices than room; the count still says how many. */
    gadget_voice_t two[2];
    CHECK(scan_voices(VOICES, strlen(VOICES), two, 2) == 3);
    CHECK(!strcmp(two[1].id, "EXAVITQu4vr4xnSDxMaL"));

    const char *empty = "{\"voices\":[]}";
    CHECK(scan_voices(empty, strlen(empty), v, 8) == 0);
    const char *other = "{\"detail\":{\"status\":\"missing_permissions\"}}";
    CHECK(scan_voices(other, strlen(other), v, 8) == -1);
    CHECK(scan_voices(VOICES, 40, v, 8) == -1);   /* cut short */
    CHECK(scan_voices("", 0, v, 8) == -1);
    const char *notobj = "[1,2]";
    CHECK(scan_voices(notobj, strlen(notobj), v, 8) == -1);
}

static void test_match_voice(void)
{
    gadget_voice_t v[8];
    int n = scan_voices(VOICES, strlen(VOICES), v, 8);
    /* Review focus 3: loose names. */
    CHECK(match_voice(v, n, "george") == 0);
    CHECK(match_voice(v, n, "GEORGE - warm, captivating storyteller") == 0);
    CHECK(match_voice(v, n, "JBFqnCBsd6RMkjVDRZzb") == 0);
    CHECK(match_voice(v, n, "Sarah - Mature") == 2);
    CHECK(match_voice(v, n, "sarah") == -2);
    CHECK(match_voice(v, n, "Rachel") == -1);
    CHECK(match_voice(v, n, "") == -1);
}

static void test_voice_choice_error(void)
{
    gadget_voice_t v[8];
    int n = scan_voices(VOICES, strlen(VOICES), v, 8);
    char msg[512];
    voice_choice_error(msg, sizeof(msg), v, n, "sarah", -2);
    CHECK(strstr(msg, "several") && strstr(msg, "Sarah - Mature") && strstr(msg, "Sunny") && !strstr(msg, "George"));
    voice_choice_error(msg, sizeof(msg), v, n, "Rachel", -1);
    CHECK(strstr(msg, "Rachel") && strstr(msg, "George") && strstr(msg, "Sarah - Mature"));
    char small[40];
    voice_choice_error(small, sizeof(small), v, n, "Rachel", -1);
    CHECK(strlen(small) == sizeof(small) - 1 && !strcmp(small + sizeof(small) - 4, "..."));
}

static void test_voices_result(void)
{
    gadget_voice_t v[8];
    int n = scan_voices(VOICES, strlen(VOICES), v, 8);
    cJSON *r = voices_result(v, n, 70, "pqHfZKP75CvOlQylNhV4");
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p, "voices")) == 3);
    cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(p, "voices"), 0);
    CHECK(!strcmp(cJSON_GetObjectItem(first, "category")->valuestring, "premade"));
    CHECK(!cJSON_GetObjectItem(first, "voice_id"));   /* names only: the list is for people */
    CHECK(!cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(p, "voices"), 1), "category"));
    CHECK(!strcmp(cJSON_GetObjectItem(p, "current")->valuestring, "Sarah - Mature"));
    CHECK(cJSON_GetObjectItem(p, "not_listed")->valueint == 67);
    cJSON_Delete(r);

    r = voices_result(v, n, n, "someone-else");
    p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(p, "current")));
    CHECK(!cJSON_GetObjectItem(p, "not_listed"));
    cJSON_Delete(r);

    r = selected_result(&v[0]);
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "payload"), "voice")->valuestring, v[0].name));
    cJSON_Delete(r);
}

static void test_voice_fetch_error(void)
{
    const char *msg = NULL;
    CHECK(voice_fetch_error(200, &msg) == NULL);
    CHECK(!strcmp(voice_fetch_error(0, &msg), "unavailable") && strstr(msg, "key"));
    /* Review focus 4: a key without the Voices permission. */
    CHECK(!strcmp(voice_fetch_error(401, &msg), "unavailable") && strstr(msg, "permission"));
    CHECK(!strcmp(voice_fetch_error(403, &msg), "unavailable"));
    CHECK(!strcmp(voice_fetch_error(-1, &msg), "network"));
    CHECK(!strcmp(voice_fetch_error(500, &msg), "network"));
}

int main(void)
{
    test_parse_settings();
    test_settings_result();
    test_gadget_error();
    test_scan_voices();
    test_match_voice();
    test_voice_choice_error();
    test_voices_result();
    test_voice_fetch_error();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
