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
        self.assertLess(len(text), 1000, text)

    def test_elevenlabs_key_is_never_logged(self):
        for path in ("components/muse/muse_tts_elevenlabs.c", "components/muse/muse_gadget_cmds.c"):
            source = (ROOT / path).read_text()
            for call in re.findall(r"ESP_LOG\w\(.*?\);", source, re.S):
                self.assertNotIn("API_KEY", call, f"{path}: {call}")

    def test_voice_select_needs_spoken_replies(self):
        source = SOURCE.read_text()
        add = source[source.index("void muse_gadget_add_commands("):]
        block = add[add.index("#if CONFIG_MUSE_HATCH"):]
        self.assertIn('"voice.select"', block[:block.index("#endif")])

    def test_voices_come_from_the_saved_voice(self):
        tts = (ROOT / "components/muse/muse_tts_elevenlabs.c").read_text()
        fetch = tts[tts.index("static bool fetch(uint32_t job)"):tts.index("static void tts_task(")]
        self.assertIn("muse_tts_voice(", fetch)
        self.assertNotIn("CONFIG_MUSE_TTS_ELEVENLABS_VOICE_ID", fetch)


if __name__ == "__main__":
    unittest.main()
