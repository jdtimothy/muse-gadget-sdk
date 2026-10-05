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

/* Host harness for the pure section of components/muse/muse_gadget_play.c.
 * argv[1]: an MP3 to decode (components/muse/test_reply.mp3). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "play_pure.inc"

static int failures;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_resample(void)
{
    static const int rates[] = { 8000, 16000, 22050, 24000, 44100 };
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        play_resampler_t rs;
        resampler_init(&rs, rates[r], PLAY_RATE);
        int16_t in[1000], out[2100];
        size_t total = 0;
        long last = -1;
        bool rising = true;
        for (int call = 0; call < 10; call++) {
            for (int i = 0; i < 1000; i++) {
                in[i] = (int16_t)(call * 1000 + i);   /* one ramp across the calls */
            }
            size_t n = resample(&rs, in, 1000, out);
            for (size_t i = 0; i < n; i++) {
                if (last >= 0 && (out[i] < last || out[i] - last > rates[r] / PLAY_RATE + 1)) {
                    rising = false;
                }
                last = out[i];
            }
            total += n;
        }
        long expect = 10000L * PLAY_RATE / rates[r];
        CHECK(labs((long)total - expect) <= 2);
        CHECK(rising);   /* continuous across calls: no jump back or skip */
    }
}

static void test_chimes(void)
{
    for (int i = 0; i < MUSE_CHIME_COUNT; i++) {
        const muse_chime_t *c = &MUSE_CHIMES[i];
        size_t total = chime_frames(c);
        CHECK(total >= PLAY_RATE * 3 / 10 && total <= PLAY_RATE * 3);
        int16_t *whole = malloc(total * sizeof(int16_t));
        int16_t *pieces = malloc(total * sizeof(int16_t));
        chime_render(c, 0, whole, total);
        for (size_t at = 0; at < total; at += 333) {
            chime_render(c, at, pieces + at, total - at < 333 ? total - at : 333);
        }
        CHECK(!memcmp(whole, pieces, total * sizeof(int16_t)));
        int peak = 0;
        for (size_t k = 0; k < total; k++) {
            peak = abs(whole[k]) > peak ? abs(whole[k]) : peak;
        }
        CHECK(peak > 3000 && peak < 32000);   /* heard, and never clipped */
        CHECK(abs(whole[total - 1]) < 300);   /* ends quiet, no click */
        free(whole);
        free(pieces);
    }
}

/* Review focus 2: logs get the host alone. */
static void test_url_host(void)
{
    char h[64];
    url_host("https://example.com/a.mp3?token=abc", h, sizeof(h));
    CHECK(!strcmp(h, "example.com"));
    url_host("https://user:secret@cdn.example.com:8443/x.mp3", h, sizeof(h));
    CHECK(!strcmp(h, "cdn.example.com:8443"));
    url_host("https://example.com?sig=1", h, sizeof(h));
    CHECK(!strcmp(h, "example.com"));
    url_host("https://example.com#t=1", h, sizeof(h));
    CHECK(!strcmp(h, "example.com"));
    url_host("https://", h, sizeof(h));
    CHECK(!strcmp(h, ""));
    char small[8];
    url_host("https://a-very-long-host.example.com/", small, sizeof(small));
    CHECK(strlen(small) == 7);
}

static void test_speech_pos(void)
{
    CHECK(speech_pos(0, 0, 100) == 0);
    CHECK(speech_pos(PLAY_RATE, 0, 140) == 14);   /* 14 characters a second until the length is known */
    CHECK(speech_pos(8000, 16000, 100) == 50);
    CHECK(speech_pos(99999, 16000, 100) == 99);
    CHECK(speech_pos(5, 0, 0) == 0);
}

/* Feeds mp3 to a stream in pieces of 1-997 bytes (seed 0: as much as fits) and
 * collects the PCM. Exits if the stream never says it's done. */
static int16_t *decode_all(const uint8_t *mp3, size_t len, unsigned seed, size_t *frames)
{
    static mp3s_t s;
    static int16_t out[MP3S_OUT_MAX];
    mp3s_init(&s);
    size_t cap = PLAY_RATE * 120, n = 0, off = 0;
    int16_t *pcm = malloc(cap * sizeof(int16_t));
    for (int guard = 0; guard < 1000000; guard++) {
        if (off < len && mp3s_room(&s)) {
            size_t want = len - off;
            if (seed) {
                seed = seed * 1103515245u + 12345u;
                size_t piece = 1 + (seed >> 16) % 997;
                want = want < piece ? want : piece;
            }
            off += mp3s_push(&s, mp3 + off, want);
        }
        if (off == len) {
            mp3s_end(&s);
        }
        bool done;
        size_t got;
        while ((got = mp3s_pull(&s, out, &done))) {
            if (n + got > cap) {
                fprintf(stderr, "more PCM than expected\n");
                exit(2);
            }
            memcpy(pcm + n, out, got * sizeof(int16_t));
            n += got;
        }
        if (done) {
            *frames = n;
            return pcm;
        }
    }
    fprintf(stderr, "the MP3 stream never finished\n");
    exit(2);
}

/* What minimp3 makes of the whole file in one go: samples per channel, and the rate. */
static size_t reference_samples(const uint8_t *mp3, size_t len, int *rate)
{
    static mp3dec_t dec;
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_init(&dec);
    size_t off = 0, total = 0;
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, mp3 + off, (int)(len - off), pcm, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += (size_t)info.frame_bytes;
        if (samples) {
            total += (size_t)samples;
            *rate = info.hz;
        }
    }
    return total;
}

/* Review focus 3 and 4. */
static void test_mp3_stream(const uint8_t *mp3, size_t len)
{
    size_t whole_n, n;
    int16_t *whole = decode_all(mp3, len, 0, &whole_n);
    int rate = 0;
    size_t samples = reference_samples(mp3, len, &rate);
    CHECK(rate > 0 && whole_n > PLAY_RATE / 2);
    CHECK(labs((long)whole_n - (long)(samples * PLAY_RATE / (size_t)rate)) <= 4);

    /* Odd-sized pieces decode to the very same audio. */
    for (unsigned seed = 1; seed <= 5; seed++) {
        int16_t *pcm = decode_all(mp3, len, seed, &n);
        CHECK(n == whole_n && !memcmp(pcm, whole, n * sizeof(int16_t)));
        free(pcm);
    }

    /* A leading ID3 tag full of things that look like MP3 headers is skipped. */
    uint8_t *tagged = malloc(len + 3000);
    memcpy(tagged, "ID3\x04\x00\x00", 6);
    tagged[6] = 0;
    tagged[7] = 0;
    tagged[8] = (2990 >> 7) & 0x7f;
    tagged[9] = 2990 & 0x7f;
    for (size_t i = 10; i < 3000; i += 4) {
        memcpy(tagged + i, "\xFF\xFB\x90\x64", 4);
    }
    memcpy(tagged + 3000, mp3, len);
    int16_t *pcm = decode_all(tagged, len + 3000, 3, &n);
    CHECK(n == whole_n && !memcmp(pcm, whole, n * sizeof(int16_t)));
    free(pcm);
    free(tagged);

    /* Not an MP3 at all: done, with nothing to play. */
    char html[5000];
    for (size_t i = 0; i < sizeof(html); i++) {
        html[i] = "<html><body>Not found</body></html>\n"[i % 36];
    }
    pcm = decode_all((const uint8_t *)html, sizeof(html), 0, &n);
    CHECK(n == 0);
    free(pcm);
    pcm = decode_all((const uint8_t *)"", 0, 0, &n);
    CHECK(n == 0);
    free(pcm);

    /* Cut short partway through: what came plays, then it's done. */
    pcm = decode_all(mp3, len / 2, 7, &n);
    CHECK(n > 0 && n < whole_n && !memcmp(pcm, whole, (n < 1000 ? n : 1000) * sizeof(int16_t)));
    free(pcm);
    free(whole);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s test.mp3\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "can't open %s\n", argv[1]);
        return 2;
    }
    static uint8_t mp3[1 << 20];
    size_t len = fread(mp3, 1, sizeof(mp3), f);
    fclose(f);

    test_resample();
    test_chimes();
    test_url_host();
    test_speech_pos();
    test_mp3_stream(mp3, len);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
