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
PIXEL = ROOT / "avatar/muse_pixel.c"


class MuseReactionsHarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmd = [*cc, "-std=c11", "-O1", "-Wall", "-Werror", "-D_DEFAULT_SOURCE",
               "-I", str(ROOT / "components/muse"),
               str(ROOT / "tests/muse_reactions_harness.c"), "-lm", "-o", str(out / "reactions")]
        compiled = subprocess.run(cmd, capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.out = out

    def test_harness(self):
        result = subprocess.run([str(self.out / "reactions")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class MuseReactionsSourceTest(unittest.TestCase):
    """The hooks in Meta's renderer: keep them when rebasing."""

    def test_renderer_hooks(self):
        src = PIXEL.read_text()
        for hook in ('#include "muse_reaction_colors.inc"', "REACT_COLOR_IDS", "REACT_COLOR_VALUES",
                     '#include "muse_reactions.inc"', "update_palette(react_scheme(p, &SCHEMES[mode]), dt);",
                     "react_motion(p, &bob, &lean, &hop);", "react_arms(p, &j, arms);",
                     "react_prop_back(p, &j);", "react_eyes(p, ", "react_brows(p, bl, br, by);",
                     "blush = react_blush(p, blush);", "react_mouth(p, ", "react_prop_front(p, &j);"):
            self.assertIn(hook, src)
        # Reactions come in before the frame code, so they can use its helpers.
        self.assertLess(src.index('#include "muse_reactions.inc"'), src.index("void muse_pixel_render("))

    def test_avatar_files_keep_metas_line(self):
        # AGENTS.md: avatar/ files carry only Meta's copyright line.
        self.assertTrue(PIXEL.read_text().startswith("// Copyright (c) Meta Platforms, Inc. and affiliates."))
        self.assertNotIn("Apache", PIXEL.read_text()[:400])

    def test_ui_passes_the_reaction(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertRegex(ui, r'#if CONFIG_MUSE_GADGET_COMMANDS\s*\n#include "muse_gadget_react.h"\s*\n#endif')
        self.assertRegex(
            ui, r"#if CONFIG_MUSE_GADGET_COMMANDS\s*\n\s*pose\.reaction = muse_react_pose\(&pose\.react_t, "
                r"&pose\.react_amount\);\s*\n#endif\s*\n\s*muse_pixel_render\(&pose\);")

    def test_react_module_is_built(self):
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        self.assertIn('"muse_gadget_react.c"', cmake)

    def test_pose_fields(self):
        header = (ROOT / "components/muse/muse_pixel.h").read_text()
        for field in ("int reaction;", "float react_t;", "float react_amount;"):
            self.assertIn(field, header)


if __name__ == "__main__":
    unittest.main()
