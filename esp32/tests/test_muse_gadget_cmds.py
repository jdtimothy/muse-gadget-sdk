# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))
SOURCE = ROOT / "components/muse/muse_gadget_cmds.c"


class MuseGadgetCmdsHarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = SOURCE.read_text()
        start = source.index("/* ---- Pure (host-tested)")
        (out / "gadget_pure.inc").write_text(source[start:source.index("/* ---- Device", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = ["-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE", "-DCJSON_NESTING_LIMIT=16",
                 "-I", str(JSON), "-I", str(out), "-I", str(ROOT / "components/muse")]
        commands = [
            [*cc, "-std=c11", *flags, "-c", str(JSON / "cJSON.c"), "-o", str(out / "cjson.o")],
            [*cc, "-std=c11", *flags, str(ROOT / "tests/muse_gadget_cmds_harness.c"),
             str(out / "cjson.o"), "-lm", "-o", str(out / "gadget")],
        ]
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_harness(self):
        result = subprocess.run([str(self.out / "gadget")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


PLAY = ROOT / "components/muse/muse_gadget_play.c"
MINIMP3 = ROOT / "components/minimp3"


class MuseGadgetPlayHarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = PLAY.read_text()
        start = source.index("/* ---- Pure (host-tested)")
        (out / "play_pure.inc").write_text(source[start:source.index("/* ---- Device", start)])
        cc = shlex.split(os.environ.get("CC", "cc"))
        inc = ["-I", str(out), "-I", str(ROOT / "components/muse"), "-I", str(MINIMP3 / "include")]
        commands = [
            # Third-party: built as the firmware builds it, warnings off.
            [*cc, "-std=c11", "-O2", "-w", *inc, "-c", str(MINIMP3 / "src/minimp3.c"),
             "-o", str(out / "minimp3.o")],
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE", *inc,
             str(ROOT / "tests/muse_gadget_play_harness.c"), str(out / "minimp3.o"), "-lm",
             "-o", str(out / "play")],
        ]
        for cmd in commands:
            compiled = subprocess.run(cmd, capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_harness(self):
        mp3 = ROOT / "components/muse/test_reply.mp3"
        result = subprocess.run([str(self.out / "play"), str(mp3)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class MuseGadgetCmdsSourceTest(unittest.TestCase):
    """Source checks: they need no compiler."""

    def test_option_and_build(self):
        kconfig = (ROOT / "components/muse/Kconfig").read_text()
        self.assertIn("config MUSE_GADGET_COMMANDS", kconfig)
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        self.assertIn('"muse_gadget_cmds.c"', cmake)

    def test_commands_are_advertised_with_the_option(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        hook = re.search(
            r"#if CONFIG_MUSE_GADGET_COMMANDS\s*\n\s*muse_gadget_add_commands\(commands\);\s*\n#endif\s*\n"
            r"\s*cJSON_AddItemToObject\(params, \"commands_v2\", commands\);", noise)
        self.assertIsNotNone(hook, "build_register_json() lacks the muse_gadget_add_commands hook")

    def test_commands_are_dispatched_first_with_the_option(self):
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        body = app[start:app.index("unsupported command", start)]
        first = body.index("{") + 1
        self.assertRegex(
            body[first:first + 400],
            r"^\s*#if CONFIG_MUSE_GADGET_COMMANDS\s*\n\s*cJSON \*gadget = muse_gadget_command\(")

    def test_register_text_stays_small(self):
        # link.register must fit in 8 KB; the skill carries the detail.
        source = SOURCE.read_text()
        start = source.index("void muse_gadget_add_commands(")
        body = source[start:source.index("\n}\n", start)]
        text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
        self.assertLess(len(text), 2000, text)

    def test_player_is_built(self):
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        self.assertIn('"muse_gadget_play.c"', cmake)

    def test_voice_task_plays_sounds_when_idle(self):
        voice = (ROOT / "components/muse/muse_voice.c").read_text()
        self.assertIn('#include "muse_gadget_play.h"', voice)
        self.assertRegex(voice, r"rest = rest && !muse_play_pending\(\);")
        hook = re.search(
            r"#if CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH\s*\n(?:\s*/\*.*\*/\s*\n)*"
            r"\s*if \(muse_play_pending\(\)\) \{\s*\n\s*muse_wifi_power\(MUSE_WIFI_FULL\);\s*\n"
            r"\s*pending_down = muse_play_run\(s_queue\);", voice)
        self.assertIsNotNone(hook, "voice_task() lacks the muse_play_run hook")
        # Before the idle read, so a queued sound plays before the next press is read.
        self.assertLess(voice.index("pending_down = muse_play_run(s_queue);"),
                        voice.index("/* The 20 ms read paces this loop. */"))

    def test_player_logs_no_urls_or_text(self):
        # Review focus 2: a clip URL can carry a token, and speech is private.
        source = PLAY.read_text()
        for call in re.findall(r"ESP_LOG\w\(TAG,\s*\"(?:[^\"\\]|\\.)*\"(.*?)\);", source, re.S):
            self.assertNotRegex(call, r"\b(url|text|arg)\b", call)

    def test_clips_retry_with_google_roots(self):
        # Board fix: hosts behind Google's front end send GTS Root R1 cross-signed
        # by the retired GlobalSign Root CA, which the IDF bundle refuses.
        source = PLAY.read_text()
        clip = source[source.index("static esp_http_client_handle_t connect_clip("):source.index("static void player_task(")]
        self.assertIn("crt_bundle_attach", source)
        self.assertIn("muse_tts_google_roots()", clip)
        self.assertIn("ESP_ERR_HTTP_CONNECT", clip)
        tts = (ROOT / "components/muse/muse_tts_elevenlabs.c").read_text()
        self.assertIn("const char *muse_tts_google_roots(void)", tts)

    def test_tts_start_takes_turns(self):
        tts = (ROOT / "components/muse/muse_tts_elevenlabs.c").read_text()
        start = tts[tts.index("uint32_t muse_tts_start("):tts.index("size_t muse_tts_read(")]
        self.assertIn("atomic_exchange(&s_busy, true)", start)

    def test_elevenlabs_key_is_never_logged(self):
        for path in ("components/muse/muse_tts_elevenlabs.c", "components/muse/muse_gadget_cmds.c",
                     "components/muse/muse_gadget_play.c"):
            source = (ROOT / path).read_text()
            for call in re.findall(r"ESP_LOG\w\(.*?\);", source, re.S):
                self.assertNotIn("API_KEY", call, f"{path}: {call}")

    def test_options_are_advertised_dispatched_and_built(self):
        source = SOURCE.read_text()
        add = source[source.index("void muse_gadget_add_commands("):]
        block = add[add.index("#if CONFIG_MUSE_HATCH"):]
        self.assertIn('"display.options"', block[:block.index("#endif")])
        body = source[source.index("cJSON *muse_gadget_command("):]
        self.assertIn("options_command(params)", body[:body.index("\n}\n")])
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        self.assertIn('"muse_gadget_options.c"', cmake)

    def test_now_playing_is_advertised_dispatched_and_built(self):
        source = SOURCE.read_text()
        add = source[source.index("void muse_gadget_add_commands("):]
        block = add[add.index("#if CONFIG_MUSE_HATCH"):]
        self.assertIn('"media.update"', block[:block.index("#endif")])
        body = source[source.index("cJSON *muse_gadget_command("):]
        self.assertIn("now_playing_command(params)", body[:body.index("\n}\n")])
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        # LVGL's tile indexes are uint8_t: Now Playing is column 0, the face 1.
        self.assertIn("s_np = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT)", ui)
        self.assertIn("s_face = lv_tileview_add_tile(s_tv, 1, 0,", ui)
        self.assertIn("lv_tileview_set_tile(s_tv, s_face, LV_ANIM_OFF)", ui)
        self.assertNotIn("lv_tileview_add_tile(s_tv, -1,", ui)
        self.assertIn("void muse_ui_now_playing(", ui)
        self.assertIn("static lv_obj_t *s_dots[3];", ui)

    def test_buttons_draw_showable_text_and_send_the_original(self):
        # Review fix: unscii-16 has no curly quotes; a tap still sends Muse its own words.
        opts = (ROOT / "components/muse/muse_gadget_options.c").read_text()
        build = opts[opts.index("static void build("):]
        self.assertIn("muse_text_showable(", build[:build.index("\n}\n")])
        tap = opts[opts.index("static void on_tap("):]
        tap = tap[:tap.index("\n}\n")]
        self.assertIn("s_shown_labels[i]", tap)
        self.assertNotIn("lv_label_get_text", tap)
        frame = opts[opts.index("bool muse_options_frame("):]
        self.assertIn("muse_options_step(", frame)

    def test_ui_shows_options(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIn('#include "muse_gadget_options.h"', ui)
        self.assertRegex(ui, r"if \(muse_options_frame\(lv_obj_get_parent\(s_reply_lbl\), opt->top, opt->h, s_w - 32\)\) \{\s*\n\s*answer = ANSWER_READ;")
        click = ui[ui.index("static void on_canvas_clicked("):]
        self.assertIn("muse_options_showing()", click[:click.index("\n}\n")])

    def test_tap_turn_in_the_chat_session(self):
        chat_h = (ROOT / "components/muse/muse_chat.h").read_text()
        self.assertIn("bool muse_hatch_tap_turn(const char *message, const char *caption);", chat_h)
        session = (ROOT / "components/muse/muse_chat_session.cpp").read_text()
        self.assertIn("CMD_TAP", session)
        tap = session[session.index("static void tap_begin("):]
        tap = tap[:tap.index("\n}\n")]
        self.assertIn("turn_start(gen, false)", tap)        # spoken, like a voice turn
        self.assertIn("emit(MUSE_HATCH_EV_HEARD, caption)", tap)
        self.assertIn('send_chat(message, "text")', tap)
        self.assertIn("if (!s_turn.tap) {", session)          # no "note sent" caption for a tap

    def test_art_is_built_and_logs_no_urls(self):
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        self.assertIn('"muse_gadget_art.c"', cmake)
        art = (ROOT / "components/muse/muse_gadget_art.c").read_text()
        self.assertIn("#if CONFIG_MUSE_GADGET_COMMANDS && CONFIG_MUSE_HATCH", art)
        self.assertIn("muse_tts_google_roots()", art)        # as clips: GTS cross-signed roots
        self.assertIn("rom/tjpgd.h", art)
        for call in re.findall(r"ESP_LOG\w\(.*?\);", art, re.S):
            self.assertNotIn("url", call.split(",", 1)[-1], call)   # the host only, never the URL
        # Review Focus 4: the buffer to fill is chosen, and a finished one handed over, under the lock.
        decode = art[art.index("static void decode("):]
        decode = decode[:decode.index("\n}\n")]
        self.assertRegex(decode, r"taskENTER_CRITICAL\(&s_lock\);\s*\n\s*int back = s_front == 0 \? 1 : 0;")
        take = art[art.index("const uint16_t *muse_art_take("):]
        self.assertIn("taskENTER_CRITICAL(&s_lock);", take[:take.index("\n}\n")])

    def test_quiet_turn_in_the_chat_session(self):
        # Review Focus 3: a Now Playing press is a typed turn that reports nowhere.
        chat_h = (ROOT / "components/muse/muse_chat.h").read_text()
        self.assertIn("bool muse_hatch_quiet_turn(const char *text);", chat_h)
        self.assertIn("bool muse_hatch_quiet_active(void);", chat_h)
        session = (ROOT / "components/muse/muse_chat_session.cpp").read_text()
        self.assertIn("CMD_QUIET", session)
        quiet = session[session.index("static void quiet_begin("):]
        quiet = quiet[:quiet.index("\n}\n")]
        self.assertIn("turn_start(0, true)", quiet)            # typed: no events reach the voice task
        self.assertIn("s_quiet_next = true;", quiet)
        self.assertIn('send_chat(text, "text")', quiet)
        start = session[session.index("static bool turn_start("):]
        self.assertIn("s_turn.quiet = s_quiet_next;", start[:start.index("\n}\n")])
        finish = session[session.index("static void turn_finish("):]
        self.assertIn("s_quiet_open = false;", finish[:finish.index("\n}\n")])
        # Every console report of a typed turn's progress skips quiet ones.
        self.assertEqual(session.count("if (s_turn.text && !s_turn.quiet) {"), 2)   # turn_fail, turn_done
        self.assertIn("if (s_turn.text && !s_turn.quiet && s_turn.agent_busy != was) {", session)
        self.assertRegex(session, r"if \(!s_turn\.quiet\) \{\s*\n\s*if \(final_text && final_text\[0\]")
        self.assertRegex(session, r'if \(!s_turn\.quiet\) \{\s*\n\s*muse_hatch_console\("text", text')

    def test_voice_task_takes_taps(self):
        voice = (ROOT / "components/muse/muse_voice.c").read_text()
        self.assertIn('#include "muse_gadget_options.h"', voice)
        self.assertRegex(voice, r"static bool tap_turn\(const char \*message, const char \*caption\)")
        hook = re.search(r"if \(muse_options_take_tap\(message, sizeof\(message\), label, sizeof\(label\)\)\) \{\s*\n"
                         r"\s*muse_wifi_power\(MUSE_WIFI_FULL\);\s*\n\s*pending_down = tap_turn\(message, label\);", voice)
        self.assertIsNotNone(hook, "voice_task() lacks the tap hook")
        self.assertLess(voice.index("pending_down = tap_turn(message, label);"),
                        voice.index("/* The 20 ms read paces this loop. */"))

    def test_react_is_advertised_and_dispatched_without_spoken_replies(self):
        source = SOURCE.read_text()
        add = source[source.index("void muse_gadget_add_commands("):]
        add = add[:add.index("#if CONFIG_MUSE_HATCH")]
        self.assertIn('"avatar.react"', add)
        body = source[source.index("cJSON *muse_gadget_command("):]
        body = body[:body.index("#if CONFIG_MUSE_HATCH")]
        self.assertIn('"avatar.react"', body)
        self.assertIn("react_command(params)", body)

    def test_voice_and_sound_commands_need_spoken_replies(self):
        source = SOURCE.read_text()
        add = source[source.index("void muse_gadget_add_commands("):]
        block = add[add.index("#if CONFIG_MUSE_HATCH"):]
        block = block[:block.index("#endif")]
        for name in ("voice.select", "voice.say", "audio.play_url", "audio.chime"):
            self.assertIn(f'"{name}"', block)

    def test_sound_commands_are_dispatched(self):
        source = SOURCE.read_text()
        body = source[source.index("cJSON *muse_gadget_command("):]
        body = body[:body.index("\n}\n")]
        self.assertIn("sound_command(command, params)", body)
        for name in ("voice.say", "audio.play_url", "audio.chime"):
            self.assertIn(f'"{name}"', body)

    def test_new_voice_says_hello(self):
        source = SOURCE.read_text()
        select = source[source.index("static cJSON *voice_select("):source.index("static void voice_task(")]
        self.assertIn("hello_text(", select)
        self.assertIn("muse_play_enqueue(MUSE_SOUND_SAY", select)

    def test_voices_come_from_the_saved_voice(self):
        tts = (ROOT / "components/muse/muse_tts_elevenlabs.c").read_text()
        fetch = tts[tts.index("static bool fetch(uint32_t job)"):tts.index("static void tts_task(")]
        self.assertIn("muse_tts_voice(", fetch)
        self.assertNotIn("CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID", fetch)


if __name__ == "__main__":
    unittest.main()
