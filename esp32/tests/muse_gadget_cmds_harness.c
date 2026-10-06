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
#include "muse_reactions.h"
#include "muse_reply_options.h"
#include "muse_now_playing.h"
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
    /* Review fix: what ESP-IDF's URL parser refuses is refused here, since it
     * logs the whole URL, token and all, when it fails. */
    CHECK(sound("audio.play_url", "{\"url\":\"https://example.com:8443/a%20b.mp3?x=1&y=2#t\"}", &s, err));
    CHECK(sound("audio.play_url", "{\"url\":\"https://user:pw@cdn.example.com/a.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://u:pw@x@host/a.mp3?token=1\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://host:abc/a.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://host:99999/a.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://host:/a.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://:8443/a.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://ho<st/a.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://example.com/a\\\"b.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https://example.com/\xC3\xA9.mp3\"}", &s, err));
    CHECK(!sound("audio.play_url", "{\"url\":\"https:///a.mp3\"}", &s, err));
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
    CHECK(MUSE_CHIME_COUNT == 7);
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

/* parse_react on parsed JSON. */
static bool react(const char *json, int *id, int *secs, char *err)
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
    bool ok = parse_react(p, id, secs, err, 256);
    cJSON_Delete(p);
    return ok;
}

static void test_parse_react(void)
{
    int id, secs;
    char err[256];
    CHECK(react("{\"name\":\"rainy\"}", &id, &secs, err) && id == MUSE_REACT_RAINY && secs == 4);
    CHECK(react("{\"name\":\"Rainy\",\"seconds\":10}", &id, &secs, err) && id == MUSE_REACT_RAINY && secs == 10);
    CHECK(react("{\"name\":\"none\"}", &id, &secs, err) && id == MUSE_REACT_NONE);
    for (int i = 1; i < MUSE_REACT_COUNT; i++) {
        char j[64];
        snprintf(j, sizeof(j), "{\"name\":\"%s\"}", MUSE_REACTION_NAMES[i]);
        CHECK(react(j, &id, &secs, err) && id == i);
    }
    CHECK(react("{\"name\":\"love\",\"seconds\":1}", &id, &secs, err) && secs == 1);
    CHECK(react("{\"name\":\"love\",\"seconds\":30}", &id, &secs, err) && secs == 30);
    CHECK(!react("{\"name\":\"love\",\"seconds\":0}", &id, &secs, err) && strstr(err, "seconds"));
    CHECK(!react("{\"name\":\"love\",\"seconds\":31}", &id, &secs, err) && strstr(err, "30"));
    CHECK(!react("{\"name\":\"love\",\"seconds\":2.5}", &id, &secs, err));
    CHECK(!react("{\"name\":\"love\",\"seconds\":\"5\"}", &id, &secs, err));
    CHECK(!react("{\"name\":\"dance\"}", &id, &secs, err) && strstr(err, "love") && strstr(err, "rainbow") &&
          strstr(err, "none"));
    CHECK(!react("{\"name\":5}", &id, &secs, err) && strstr(err, "name"));
    CHECK(!react("{}", &id, &secs, err) && strstr(err, "name"));
    CHECK(!react(NULL, &id, &secs, err) && strstr(err, "name"));
    /* A misspelled parameter is refused, not ignored. */
    CHECK(!react("{\"name\":\"love\",\"secs\":3}", &id, &secs, err) && strstr(err, "secs"));
}

static void test_reaction_names(void)
{
    CHECK(MUSE_REACT_COUNT == 22);
    CHECK(!strcmp(MUSE_REACTION_NAMES[MUSE_REACT_NONE], "none"));
    for (int i = 1; i < MUSE_REACT_COUNT; i++) {
        CHECK(strstr(MUSE_REACTION_NAMES_TEXT, MUSE_REACTION_NAMES[i]) != NULL);
        CHECK(muse_reaction_find(MUSE_REACTION_NAMES[i]) == i);
    }
    CHECK(muse_reaction_find("NONE") == MUSE_REACT_NONE);
    CHECK(muse_reaction_find("dance") == -1);
}

static bool near(float a, float b)
{
    return a > b - 0.001f && a < b + 0.001f;
}

static void test_reaction_amount(void)
{
    CHECK(near(muse_reaction_amount(-0.1f, 4), 0));
    CHECK(near(muse_reaction_amount(0, 4), 0));
    CHECK(near(muse_reaction_amount(0.125f, 4), 0.5f));   /* half way in */
    CHECK(near(muse_reaction_amount(1, 4), 1));
    CHECK(near(muse_reaction_amount(3.8f, 4), 0.5f));     /* half way out */
    CHECK(near(muse_reaction_amount(4, 4), 0));
    CHECK(near(muse_reaction_amount(9, 4), 0));
    CHECK(muse_reaction_amount(0.5f, 1) > 0.99f);         /* a 1 s reaction still reaches full */
}

static void test_react_result(void)
{
    cJSON *r = react_result(MUSE_REACT_RAINY, 4);
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    CHECK(!strcmp(cJSON_GetObjectItem(p, "reaction")->valuestring, "rainy"));
    CHECK(cJSON_GetObjectItem(p, "seconds")->valueint == 4);
    cJSON_Delete(r);
    r = react_result(MUSE_REACT_NONE, 4);
    p = cJSON_GetObjectItem(r, "payload");
    CHECK(!strcmp(cJSON_GetObjectItem(p, "reaction")->valuestring, "none"));
    CHECK(!cJSON_GetObjectItem(p, "seconds"));
    cJSON_Delete(r);
}

/* parse_options on parsed JSON. */
static bool options(const char *json, char labels[][MUSE_OPTIONS_LABEL_MAX + 1], int *n, char *err)
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
    bool ok = parse_options(p, labels, n, err, 160);
    cJSON_Delete(p);
    return ok;
}

/* Review focus 3: what Muse might send. */
static void test_parse_options(void)
{
    char l[MUSE_OPTIONS_MAX][MUSE_OPTIONS_LABEL_MAX + 1];
    int n;
    char err[160];
    CHECK(options("{\"options\":[\"Yes, play it\",\"No thanks\"]}", l, &n, err) && n == 2 &&
          !strcmp(l[0], "Yes, play it") && !strcmp(l[1], "No thanks"));
    CHECK(options("{\"options\":[\"a\",\"b\",\"c\",\"d\"]}", l, &n, err) && n == 4 && !strcmp(l[3], "d"));
    CHECK(options("{\"options\":[\"123456789012345678901234\",\"x\"]}", l, &n, err) && strlen(l[0]) == 24);
    CHECK(options("{\"options\":[\"Caf\xC3\xA9 au lait\",\"Th\xC3\xA9\"]}", l, &n, err) &&
          !strcmp(l[0], "Caf\xC3\xA9 au lait"));
    CHECK(!options("{\"options\":[\"1234567890123456789012345\",\"x\"]}", l, &n, err) && strstr(err, "24"));
    CHECK(!options("{\"options\":[\"only one\"]}", l, &n, err) && strstr(err, "2"));
    CHECK(!options("{\"options\":[\"a\",\"b\",\"c\",\"d\",\"e\"]}", l, &n, err) && strstr(err, "4"));
    CHECK(!options("{\"options\":[\"a\",\"\"]}", l, &n, err));
    CHECK(!options("{\"options\":[\"a\",\"   \"]}", l, &n, err));
    CHECK(!options("{\"options\":[\"a\",\"two\\nlines\"]}", l, &n, err));
    CHECK(!options("{\"options\":[\"a\",5]}", l, &n, err) && strstr(err, "string"));
    CHECK(!options("{\"options\":\"a, b\"}", l, &n, err) && strstr(err, "list"));
    CHECK(!options("{}", l, &n, err) && strstr(err, "options"));
    CHECK(!options(NULL, l, &n, err) && strstr(err, "options"));
    /* A misspelled or unknown parameter is refused, not ignored. */
    CHECK(!options("{\"options\":[\"a\",\"b\"],\"seconds\":60}", l, &n, err) && strstr(err, "seconds"));
}

/* Review fixes: a set waiting for the gadget to go idle. */
static void test_options_step(void)
{
    CHECK(muse_options_step(true, false, 1) == MUSE_OPTIONS_SHOW);
    CHECK(muse_options_step(false, false, 20) == MUSE_OPTIONS_WAIT);   /* mid-reply: after the speech */
    /* A talk press drops a set offered with the reply it cut off. */
    CHECK(muse_options_step(false, true, 5) == MUSE_OPTIONS_DROP);
    /* A set that waited too long is stale. */
    CHECK(muse_options_step(true, false, MUSE_OPTIONS_WAIT_MAX_S + 1) == MUSE_OPTIONS_DROP);
    CHECK(muse_options_step(false, false, MUSE_OPTIONS_WAIT_MAX_S + 1) == MUSE_OPTIONS_DROP);
    /* Muse's tool layer may send the list as a JSON string. */
    char l[MUSE_OPTIONS_MAX][MUSE_OPTIONS_LABEL_MAX + 1];
    int n;
    char err[160];
    CHECK(options("{\"options\":\"[\\\"Yes\\\",\\\"No\\\"]\"}", l, &n, err) && n == 2 && !strcmp(l[1], "No"));
    CHECK(!options("{\"options\":\"[\\\"only\\\"]\"}", l, &n, err));
}

static void test_options_message(void)
{
    char out[MUSE_OPTIONS_MSG_MAX];
    muse_options_message(out, sizeof(out), "Tell me more");
    CHECK(!strcmp(out, "Tell me more [tapped on the gadget]"));
    muse_options_message(out, sizeof(out), "123456789012345678901234");
    CHECK(strlen(out) == 24 + strlen(MUSE_OPTIONS_TAG));   /* the longest label still fits */
    cJSON *r = options_result(3);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "payload"), "shown")->valueint == 3);
    cJSON_Delete(r);
}

static void test_np_message(void)
{
    char out[MUSE_NP_MSG_MAX];
    muse_np_message(out, sizeof(out), MUSE_NP_NEXT, "ap1234");
    CHECK(!strcmp(out, "next [pressed on the gadget's Now Playing tile; player_id: ap1234]"));
    /* Review Focus 1: no player id stored yet (an older skill sends none). */
    muse_np_message(out, sizeof(out), MUSE_NP_REFRESH, NULL);
    CHECK(!strcmp(out, "refresh [pressed on the gadget's Now Playing tile]"));
    muse_np_message(out, sizeof(out), MUSE_NP_PLAY_PAUSE, "");
    CHECK(!strcmp(out, "play_pause [pressed on the gadget's Now Playing tile]"));
    /* The longest id still fits whole. */
    char id[MUSE_NP_PLAYER_ID_MAX + 1];
    memset(id, 'x', MUSE_NP_PLAYER_ID_MAX);
    id[MUSE_NP_PLAYER_ID_MAX] = '\0';
    muse_np_message(out, sizeof(out), MUSE_NP_PLAY_PAUSE, id);
    CHECK(strlen(out) == strlen("play_pause") + strlen(MUSE_NP_TAG) + strlen("; player_id: ") + MUSE_NP_PLAYER_ID_MAX + 1);
    CHECK(out[strlen(out) - 1] == ']');
    CHECK(!strcmp(muse_np_action_name(MUSE_NP_PREVIOUS), "previous"));
    CHECK(!strcmp(muse_np_art_name(MUSE_NP_ART_UNCHANGED), "unchanged"));
}

static void test_np_art_scale(void)
{
    CHECK(muse_np_art_scale(448, 448, 368, 448) == 0);
    CHECK(muse_np_art_scale(1000, 1000, 368, 448) == 1);   /* 500 still covers, 250 doesn't */
    CHECK(muse_np_art_scale(1800, 1800, 368, 448) == 2);   /* 450 */
    CHECK(muse_np_art_scale(4000, 4000, 368, 448) == 3);   /* 500 at 1/8, the decoder's smallest */
    CHECK(muse_np_art_scale(300, 300, 368, 448) == 0);     /* too small: scaled up after */
    CHECK(muse_np_art_scale(1500, 600, 368, 448) == 0);    /* halving loses the height */
}

/* The decoded pixel the tile's (x, y) shows. */
static int fit_x(muse_np_fit_t f, int x)
{
    return (int)((f.ox + (uint32_t)x * f.step) >> 16);
}

static int fit_y(muse_np_fit_t f, int y)
{
    return (int)((f.oy + (uint32_t)y * f.step) >> 16);
}

static void test_np_art_fit(void)
{
    /* A 448 square on the 368x448 tile: 1:1, cropped 40 px each side. */
    muse_np_fit_t f = muse_np_art_fit(448, 448, 368, 448);
    CHECK(f.step == 65536 && fit_x(f, 0) == 40 && fit_x(f, 367) == 407 && fit_y(f, 0) == 0 && fit_y(f, 447) == 447);
    /* 500x500 (a 1000 px cover at 1/2): scaled down to the tile's height, so
     * only the sides are cropped (review fix: 1:1 cut a quarter of a cover off). */
    f = muse_np_art_fit(500, 500, 368, 448);
    CHECK(f.step > 65536 && fit_y(f, 0) == 0 && fit_y(f, 447) >= 498 && fit_x(f, 0) == 44);
    /* Music Assistant's 512 px covers: the whole height shows, 420 of 512 across. */
    f = muse_np_art_fit(512, 512, 368, 448);
    CHECK(fit_y(f, 0) == 0 && fit_y(f, 447) >= 510 && fit_x(f, 0) == 45 && fit_x(f, 367) == 465);
    /* A 300 square: scaled up to fill the height, cropped at the sides. */
    f = muse_np_art_fit(300, 300, 368, 448);
    CHECK(f.step < 65536 && fit_y(f, 0) == 0 && fit_y(f, 447) == 299);
    CHECK(fit_x(f, 0) == 26 && fit_x(f, 367) == 272);
    /* Review Focus 4: never past the decoded image's edge, whatever its size. */
    static const int sizes[][2] = { { 1, 1 }, { 368, 448 }, { 369, 449 }, { 640, 360 },
                                    { 4096, 4096 }, { 4096, 1 }, { 1, 4096 } };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        f = muse_np_art_fit(sizes[i][0], sizes[i][1], 368, 448);
        CHECK(fit_x(f, 367) < sizes[i][0] && fit_y(f, 447) < sizes[i][1]);
    }
}

/* Joshua: a tap in the lower third (the buttons' row) never refreshes. */
static void test_np_tap_refreshes(void)
{
    CHECK(muse_np_tap_refreshes(0, 448));
    CHECK(muse_np_tap_refreshes(297, 448));
    CHECK(!muse_np_tap_refreshes(299, 448));
    CHECK(!muse_np_tap_refreshes(384, 448));   /* the buttons' centre */
    CHECK(!muse_np_tap_refreshes(447, 448));
    CHECK(muse_np_tap_refreshes(-1, 448) == false && !muse_np_tap_refreshes(448, 448));   /* off the tile */
}

static void test_np_dim(void)
{
    CHECK(muse_np_dim(0, 448) == 128);     /* 50% at the top */
    CHECK(muse_np_dim(179, 448) == 128);   /* until 40% of the way down */
    CHECK(muse_np_dim(447, 448) == 64);    /* 25% at the bottom */
    CHECK(muse_np_dim(313, 448) > 64 && muse_np_dim(313, 448) < 128);
    for (int y = 1; y < 448; y++) {
        CHECK(muse_np_dim(y, 448) <= muse_np_dim(y - 1, 448));   /* never brighter going down */
    }
    CHECK(muse_np_rgb565(255, 255, 255) == 0xFFFF);
    CHECK(muse_np_rgb565(255, 0, 0) == 0xF800 && muse_np_rgb565(0, 0, 255) == 0x001F);
    CHECK(muse_np_dim565(0xFFFF, 256) == 0xFFFF);
    CHECK(muse_np_dim565(0xFFFF, 128) == (uint16_t)((15 << 11) | (31 << 5) | 15));
    CHECK(muse_np_dim565(0xF800, 0) == 0);
}

/* parse_media on parsed JSON. The fields point into *p, which the caller deletes. */
static bool media(const char *json, cJSON **p, muse_np_fields_t *f, char *err)
{
    *p = NULL;
    if (json) {
        *p = cJSON_Parse(json);
        if (!*p) {
            fprintf(stderr, "bad test JSON: %s\n", json);
            exit(2);
        }
    }
    err[0] = '\0';
    return parse_media(*p, f, err, 160);
}

static void test_parse_media(void)
{
    muse_np_fields_t f;
    cJSON *p;
    char err[160];
    CHECK(media("{\"title\":\"So What\",\"artist\":\"Miles Davis\",\"state\":\"playing\",\"player_id\":\"ap12ab\","
                "\"art_url\":\"http://192.168.7.132:8095/imageproxy?path=x&size=448&fmt=jpeg\"}", &p, &f, err) &&
          !strcmp(f.title, "So What") && !strcmp(f.artist, "Miles Davis") && !strcmp(f.state, "playing") &&
          !strcmp(f.player_id, "ap12ab") && !strncmp(f.art_url, "http://", 7) && !f.album && !f.player);
    cJSON_Delete(p);
    /* Review Focus 1: an older skill's call, and none at all. */
    CHECK(media("{\"player\":\"Kitchen\",\"title\":\"x\"}", &p, &f, err) && !f.player_id && !f.art_url);
    cJSON_Delete(p);
    CHECK(media("{}", &p, &f, err) && !f.title);
    cJSON_Delete(p);
    CHECK(media(NULL, &p, &f, err) && !f.state);
    CHECK(media("{\"art_url\":\"\"}", &p, &f, err) && f.art_url && !f.art_url[0]);   /* clears the art */
    cJSON_Delete(p);
    CHECK(media("{\"art_url\":\"https://i.scdn.co/image/ab67616d\"}", &p, &f, err));
    cJSON_Delete(p);
    CHECK(media("{\"state\":\"idle\",\"title\":\"\"}", &p, &f, err) && !strcmp(f.state, "idle"));
    cJSON_Delete(p);
    CHECK(media(json_of("art_url", "http://", 512), &p, &f, err));
    cJSON_Delete(p);
    CHECK(media(json_of("player_id", "", 64), &p, &f, err));
    cJSON_Delete(p);
    /* Refused, saying why. */
    CHECK(!media("{\"state\":\"stopped\"}", &p, &f, err) && strstr(err, "playing, paused or idle"));
    cJSON_Delete(p);
    CHECK(!media("{\"title\":5}", &p, &f, err) && strstr(err, "title"));
    cJSON_Delete(p);
    CHECK(!media(json_of("art_url", "http://", 513), &p, &f, err) && strstr(err, "512"));
    cJSON_Delete(p);
    CHECK(!media("{\"art_url\":\"ftp://host/a.jpg\"}", &p, &f, err) && strstr(err, "art_url"));
    cJSON_Delete(p);
    CHECK(!media("{\"art_url\":\"http://host/a b.jpg\"}", &p, &f, err));
    cJSON_Delete(p);
    CHECK(!media("{\"art_url\":\"http://\"}", &p, &f, err));
    cJSON_Delete(p);
    CHECK(!media("{\"player_id\":\"\"}", &p, &f, err) && strstr(err, "player_id"));
    cJSON_Delete(p);
    CHECK(!media(json_of("player_id", "", 65), &p, &f, err) && strstr(err, "64"));
    cJSON_Delete(p);
    CHECK(!media("{\"player_id\":\"a\\nb\"}", &p, &f, err));
    cJSON_Delete(p);
    /* A misspelled or unknown parameter is refused, not ignored. */
    CHECK(!media("{\"title\":\"x\",\"duration\":200}", &p, &f, err) && strstr(err, "duration"));
    cJSON_Delete(p);
    /* Review fix: the bridge gives null for what a track lacks (a radio stream's
     * album, a local file's cover): not sent, except art_url, where null clears. */
    CHECK(media("{\"title\":\"Radio 1\",\"artist\":null,\"album\":null,\"state\":null,\"art_url\":null}", &p, &f,
                err) &&
          !strcmp(f.title, "Radio 1") && !f.artist && !f.album && !f.state && f.art_url && !f.art_url[0]);
    cJSON_Delete(p);
    CHECK(media("{\"player_id\":null}", &p, &f, err) && !f.player_id);
    cJSON_Delete(p);
    /* audio.play_url still takes https only. */
    gadget_sound_t s;
    CHECK(!sound("audio.play_url", "{\"url\":\"http://example.com/a.mp3\"}", &s, err));
}

static void test_media_result(void)
{
    static const muse_np_art_t arts[] = { MUSE_NP_ART_NONE, MUSE_NP_ART_LOADING, MUSE_NP_ART_UNCHANGED,
                                          MUSE_NP_ART_CLEARED };
    static const char *const names[] = { "none", "loading", "unchanged", "cleared" };
    for (int i = 0; i < 4; i++) {
        cJSON *r = media_result(arts[i]);
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
        cJSON *art = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "payload"), "art");
        CHECK(cJSON_IsString(art) && !strcmp(art->valuestring, names[i]));
        cJSON_Delete(r);
    }
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
    test_parse_react();
    test_reaction_names();
    test_reaction_amount();
    test_react_result();
    test_parse_options();
    test_options_step();
    test_options_message();
    test_np_message();
    test_np_art_scale();
    test_np_art_fit();
    test_np_dim();
    test_np_tap_refreshes();
    test_parse_media();
    test_media_result();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
