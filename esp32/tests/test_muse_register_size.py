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

"""link.register on the s318 build: the gadget commands and the WebAssembly apps' together.

Runs build_register_json() through test_link_device_description's harness, with
muse_gadget_add_commands() linked in. A registration over 8 KB never registers.
"""

import json
import os
import subprocess
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path

from tests import test_link_device_description as meta

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "components/muse/muse_gadget_cmds.c"
LIMIT = meta.REGISTER_LIMIT
BOARD = ["-DCONFIG_HOMEHUB_TUNNEL=1", "-DCONFIG_HOMEHUB_LED_BACKEND_MUSE=1", "-DCONFIG_HOMEHUB_DISPLAY_COMMANDS=1",
         '-DCONFIG_MUSE_DEVICE_MODEL_ID="muse-gadget-waveshare-s3-18"',
         '-DCONFIG_MUSE_DEVICE_MODEL_NAME="Waveshare ESP32-S3-Touch-AMOLED-1.8"']


class MuseRegisterSizeTest(unittest.TestCase):
    @contextmanager
    def with_gadget_commands(self, apps_on: bool, elevenlabs_key: str = ""):
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("static cJSON *param(const char *type")
        end = source.index("\n}\n", source.index("void muse_gadget_add_commands(")) + 3
        with tempfile.TemporaryDirectory() as name:
            gadget = Path(name) / "gadget_add.c"
            gadget.write_text(
                '#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n#include "cJSON.h"\n'
                '#include "muse_gadget_sound.h"\n#include "muse_reactions.h"\n'
                f'#define CONFIG_MUSE_TTS_ELEVENLABS_API_KEY "{elevenlabs_key}"\n'
                f"static bool muse_settings_apps_on(void) {{ return {int(apps_on)}; }}\n"
                + source[start:end], encoding="utf-8")
            real_run = meta.subprocess.run

            def run(args, **kwargs):
                if isinstance(args, list) and "-o" in args:
                    harness = next(Path(a) for a in args if str(a).endswith("harness.c"))
                    harness.write_text('#include "cJSON.h"\n#define TAG "link"\n#define ESP_LOGI(...) ((void)0)\n'
                                       "void muse_gadget_add_commands(cJSON *commands);\n" + harness.read_text())
                    if os.name == "nt":   # llvm-mingw has no ASan runtime
                        args = [a for a in args if a != "-fsanitize=address,undefined"]
                    out = args.index("-o")
                    args[out:out] = ["-DCONFIG_MUSE_GADGET_COMMANDS=1", "-DCONFIG_MUSE_HATCH=1",
                                     "-I", str(ROOT / "components/muse"), str(gadget)]
                return real_run(args, **kwargs)

            meta.subprocess.run = run
            try:
                yield
            finally:
                meta.subprocess.run = real_run

    def register(self, apps_on: bool, elevenlabs_key: str = "") -> tuple[int, dict]:
        with self.with_gadget_commands(apps_on, elevenlabs_key):
            # The longest names and an SSID of 32 escaped quotation marks.
            lines = meta.DeviceDescriptionTest("test_register_describes_the_device").registrations(BOARD, "n" * 31, "d" * 31, ssid='\\"' * 32, apps=True, apps_on=apps_on)
        return max(len(line) for line in lines), json.loads(lines[0])["params"]["commands_v2"]

    def test_fits_with_apps_on(self):
        size, commands = self.register(apps_on=True)
        print(f"link.register, s318 with apps: {size} of {LIMIT} bytes")
        # cJSON_PrintPreallocated asks for 5 bytes beyond the output.
        self.assertLessEqual(size + 5, LIMIT)
        for name in ("app.run", "media.update", "avatar.react", "display.options", "display.draw_url"):
            self.assertIn(name, commands)
        self.assertNotIn("display.draw_file", commands)
        self.assertNotIn("voice.say", commands)

    def test_apps_off_keeps_draw_file(self):
        size, commands = self.register(apps_on=False)
        self.assertLessEqual(size + 5, LIMIT)
        self.assertIn("display.draw_file", commands)
        self.assertNotIn("app.run", commands)

    def test_voice_commands_only_with_a_key(self):
        _, commands = self.register(apps_on=False, elevenlabs_key="sk_test")
        self.assertIn("voice.say", commands)
        self.assertIn("voice.select", commands)



if __name__ == "__main__":
    unittest.main()
