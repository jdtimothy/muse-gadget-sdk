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
#include "muse_gadget_sound.h"
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
    cJSON *r = voices_result(v, n, 70, "pqHfZKP75CvOlQylNhV4", 64);
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p, "voices")) == 3);
    cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(p, "voices"), 0);
    CHECK(!strcmp(cJSON_GetObjectItem(first, "category")->valuestring, "premade"));
    CHECK(!cJSON_GetObjectItem(first, "voice_id"));   /* names only: the list is for people */
    CHECK(!cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(p, "voices"), 1), "category"));
    CHECK(!strcmp(cJSON_GetObjectItem(p, "current")->valuestring, "Sarah - Mature"));
    CHECK(cJSON_GetObjectItem(p, "not_listed")->valueint == 67);
    cJSON_Delete(r);

    r = voices_result(v, n, n, "someone-else", 64);
    p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(p, "current")));
    CHECK(!cJSON_GetObjectItem(p, "not_listed"));
    cJSON_Delete(r);

    r = selected_result(&v[0]);
    CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "payload"), "voice")->valuestring, v[0].name));
    cJSON_Delete(r);
}

static bool valid_utf8(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int need = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (need < 0) {
            return false;
        }
        p++;
        for (int i = 0; i < need; i++, p++) {
            if ((*p & 0xC0) != 0x80) {
                return false;
            }
        }
    }
    return true;
}

/* Review fix: byte limits never split a UTF-8 character. */
static void test_truncation_keeps_utf8_whole(void)
{
    char json[512] = "{\"voices\":[{\"voice_id\":\"abc\",\"name\":\"";
    for (int i = 0; i < 40; i++) {
        strcat(json, "\xC3\xA9");   /* é: 80 bytes, past the 64-byte name */
    }
    strcat(json, "\"}]}");
    gadget_voice_t v[1];
    CHECK(scan_voices(json, strlen(json), v, 1) == 1);
    CHECK(valid_utf8(v[0].name) && strlen(v[0].name) == 64);

    char odd[512] = "{\"voices\":[{\"voice_id\":\"abc\",\"name\":\"x";
    for (int i = 0; i < 40; i++) {
        strcat(odd, "\xC3\xA9");
    }
    strcat(odd, "\"}]}");
    CHECK(scan_voices(odd, strlen(odd), v, 1) == 1);
    CHECK(valid_utf8(v[0].name) && strlen(v[0].name) == 63);

    char err[160];
    gadget_settings_t s = BASE;
    char key[256] = "{\"x";
    for (int i = 0; i < 30; i++) {
        strcat(key, "\xC3\xA9");
    }
    strcat(key, "\":1}");
    CHECK(!parse(key, &s, err) && valid_utf8(err));

    char name[80] = "x";
    for (int i = 0; i < 30; i++) {
        strcat(name, "\xC3\xA9");
    }
    char msg[512];
    voice_choice_error(msg, sizeof(msg), v, 1, name, -1);
    CHECK(valid_utf8(msg));
    char small[40];
    gadget_voice_t wide = { "id", "", "" };
    for (int i = 0; i < 30; i++) {
        strcat(wide.name, "\xC3\xA9");
    }
    voice_choice_error(small, sizeof(small), &wide, 1, "zz", -1);
    CHECK(valid_utf8(small));
}

/* Review fix: the build's voice has no name until one is chosen; say its ID. */
static void test_voice_label(void)
{
    CHECK(!strcmp(voice_label("George", "JBF"), "George"));
    CHECK(!strcmp(voice_label("", "JBF"), "JBF"));
    CHECK(voice_label("", "") == NULL);
}

/* Review fix: the list Muse reads is capped, but the current voice is found
 * among every stored one. */
static void test_voices_result_list_cap(void)
{
    gadget_voice_t v[8];
    int n = scan_voices(VOICES, strlen(VOICES), v, 8);
    cJSON *r = voices_result(v, n, n, "pqHfZKP75CvOlQylNhV4", 2);
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(p, "voices")) == 2);
    CHECK(cJSON_GetObjectItem(p, "not_listed")->valueint == 1);
    CHECK(!strcmp(cJSON_GetObjectItem(p, "current")->valuestring, "Sarah - Mature"));
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

/* parse_sound with the JSON parsed and the argument copied out before it's freed. */
static bool sound(const char *command, const char *json, gadget_sound_t *s, char *err)
{
    static char arg[1024];
    cJSON *p = NULL;
    if (json) {
        p = cJSON_Parse(json);
        if (!p) {
            fprintf(stderr, "bad test JSON: %s\n", json);
            exit(2);
        }
    }
    err[0] = '\0';
    bool ok = parse_sound(command, p, s, err, 160);
    if (ok) {
        snprintf(arg, sizeof(arg), "%s", s->arg);
        s->arg = arg;
    }
    cJSON_Delete(p);
    return ok;
}

/* {"key":"<prefix><n - strlen(prefix) a's>"} */
static const char *json_of(const char *key, const char *prefix, size_t n)
{
    static char j[1200];
    int w = snprintf(j, sizeof(j), "{\"%s\":\"%s", key, prefix);
    memset(j + w, 'a', n - strlen(prefix));
    strcpy(j + w + n - strlen(prefix), "\"}");
    return j;
}

static void test_parse_say(void)
{
    gadget_sound_t s;
    char err[160];
    CHECK(sound("voice.say", "{\"text\":\"Dinner's ready\"}", &s, err) && s.kind == MUSE_SOUND_SAY &&
          !strcmp(s.arg, "Dinner's ready") && s.caption);
    CHECK(sound("voice.say", "{\"text\":\"hi\",\"caption\":false}", &s, err) && !s.caption);
    CHECK(sound("voice.say", json_of("text", "", 600), &s, err) && strlen(s.arg) == 600);
    CHECK(!sound("voice.say", json_of("text", "", 601), &s, err) && strstr(err, "600"));
    CHECK(!sound("voice.say", "{\"text\":\"\"}", &s, err) && strstr(err, "600"));
    CHECK(!sound("voice.say", NULL, &s, err) && strstr(err, "text"));
    CHECK(!sound("voice.say", "{}", &s, err) && strstr(err, "text"));
    CHECK(!sound("voice.say", "{\"text\":5}", &s, err) && strstr(err, "string"));
    CHECK(!sound("voice.say", "{\"text\":\"hi\",\"caption\":\"no\"}", &s, err) && strstr(err, "caption"));
    /* A misspelled or unknown parameter is refused, not ignored. */
    CHECK(!sound("voice.say", "{\"text\":\"hi\",\"voice\":\"George\"}", &s, err) && strstr(err, "voice"));
    CHECK(!sound("voice.say", "[\"hi\"]", &s, err));
}

/* Review focus 1: only a usable https URL is queued. */
static void test_parse_url(void)
{
    gadget_sound_t s;
    char err[160];
    CHECK(sound("audio.play_url", "{\"url\":\"https://example.com/a.mp3\"}", &s, err) &&
          s.kind == MUSE_SOUND_URL && !strcmp(s.arg, "https://example.com/a.mp3"));
    CHECK(sound("audio.play_url", "{\"url\":\"HTTPS://example.com/a.mp3\"}", &s, err));
    CHECK(sound("audio.play_url", json_of("url", "https://", 512), &s, err));
    CHECK(!sound("audio.play_url", json_of("url", "https://", 513), &s, err) && strstr(err, "512"));
    CHECK(!sound("audio.play_url", "{\"url\":\"http://example.com/a.mp3\"}", &s, err) && strstr(err, "https"));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://example.com/a b.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://example.com/a\\n.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":7}", &s, err) && strstr(err, "string"));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://x/a.mp3\",\"caption\":true}", &s, err) &&
          strstr(err, "caption"));
}

static void test_parse_chime(void)
{
    gadget_sound_t s;
    char err[160];
    for (int i = 0; i < MUSE_CHIME_COUNT; i++) {
        char j[64];
        snprintf(j, sizeof(j), "{\"name\":\"%s\"}", MUSE_CHIMES[i].name);
        CHECK(sound("audio.chime", j, &s, err) && s.kind == MUSE_SOUND_CHIME);
    }
    CHECK(sound("audio.chime", "{\"name\":\"TADA\"}", &s, err) && !strcmp(s.arg, "TADA"));
    CHECK(!sound("audio.chime", "{\"name\":\"fanfare\"}", &s, err) && strstr(err, "ding") && strstr(err, "tada"));
    CHECK(!sound("audio.chime", "{\"name\":7}", &s, err));
    CHECK(!sound("audio.chime", "{}", &s, err) && strstr(err, "name"));
}

static void test_chime_names(void)
{
    CHECK(MUSE_CHIME_COUNT == 6);
    for (int i = 0; i < MUSE_CHIME_COUNT; i++) {
        CHECK(strstr(MUSE_CHIME_NAMES, MUSE_CHIMES[i].name) != NULL);
        CHECK(muse_chime_find(MUSE_CHIMES[i].name) == i);
        CHECK(MUSE_CHIMES[i].n >= 1 && MUSE_CHIMES[i].n <= 4 && MUSE_CHIMES[i].repeat >= 1);
    }
    CHECK(muse_chime_find("nope") == -1);
}

static const char *error_code(cJSON *r)
{
    return cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "code")->valuestring;
}

static const char *error_message(cJSON *r)
{
    return cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "message")->valuestring;
}

static void test_sound_results(void)
{
    cJSON *r = sound_result(2);
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(p, "queued")) && cJSON_GetObjectItem(p, "position")->valueint == 2);
    cJSON_Delete(r);
    r = sound_result(0);
    CHECK(!strcmp(error_code(r), "busy"));
    cJSON_Delete(r);
    r = sound_result(-1);
    CHECK(!strcmp(error_code(r), "out_of_memory"));
    cJSON_Delete(r);
    r = muted_error(MUSE_SOUND_SAY, true);
    CHECK(!strcmp(error_code(r), "muted") && strstr(error_message(r), "screen"));
    cJSON_Delete(r);
    r = muted_error(MUSE_SOUND_SAY, false);
    CHECK(!strcmp(error_code(r), "muted") && strstr(error_message(r), "device.settings"));
    cJSON_Delete(r);
    r = muted_error(MUSE_SOUND_CHIME, true);
    CHECK(strstr(error_message(r), "device.settings") != NULL);
    cJSON_Delete(r);
}

static void test_hello_text(void)
{
    char out[80];
    hello_text(out, sizeof(out), "George - Warm, Captivating Storyteller");
    CHECK(!strcmp(out, "Hi, I'm George."));
    hello_text(out, sizeof(out), "Rachel");
    CHECK(!strcmp(out, "Hi, I'm Rachel."));
    hello_text(out, sizeof(out), "");
    CHECK(!strcmp(out, "Hi, this is my new voice."));
    char wide[65] = "";
    for (int i = 0; i < 32; i++) {
        strcat(wide, "\xC3\xA9");
    }
    hello_text(out, sizeof(out), wide);
    CHECK(valid_utf8(out) && strlen(out) == 8 + 64 + 1);
    char small[20];
    hello_text(small, sizeof(small), wide);
    CHECK(valid_utf8(small));
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
    test_truncation_keeps_utf8_whole();
    test_voice_label();
    test_voices_result_list_cap();
    test_parse_say();
    test_parse_url();
    test_parse_chime();
    test_chime_names();
    test_sound_results();
    test_hello_text();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
