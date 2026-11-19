"""Tests of package_platforms.py: python tools/platforms/test_package_platforms.py"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "package_platforms.py"


def make(root: Path, name: str, os_line: str = "", link: str = "", src: str = "", imports=()):
    folder = root / name
    (folder / "src").mkdir(parents=True)
    (folder / "congo.toml").write_text(
        f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n{os_line}\n{link}',
        encoding="utf-8")
    body = "".join(f"import {i} use *;\n" for i in imports) + src
    (folder / "src" / f"{name}.cone").write_text(f"mod {name};\n{body}", encoding="utf-8")


def run(root: Path, *args):
    return subprocess.run([sys.executable, "-I", str(TOOL), *args, "--packages", str(root)],
                          capture_output=True, text=True)


class Platforms(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_list_run_skipped_and_unverifiable(self):
        make(self.root, "plain")
        make(self.root, "winonly", '# kernel32 calls\nos = ["windows"]')
        make(self.root, "sdlish", link='[link]\nlibraries = ["SDL3"]\n')
        make(self.root, "usessdl", imports=["sdlish"])
        lacks = self.root / "lacks.txt"
        lacks.write_text("# the box\nSDL3\n", encoding="utf-8")
        out = run(self.root, "list", "--lacks", str(lacks)).stdout.splitlines()
        self.assertIn("plain run", out)
        self.assertIn("winonly skipped kernel32 calls", out)
        self.assertIn("sdlish unverifiable needs SDL3, which the box lacks", out)
        self.assertIn("usessdl unverifiable needs SDL3, which the box lacks", out)
        self.assertEqual(run(self.root, "run", "--lacks", str(lacks)).stdout.split(), ["plain"])
        # no lacks file: nothing is unverifiable
        self.assertEqual(sorted(run(self.root, "run").stdout.split()), ["plain", "sdlish", "usessdl"])
        # a skipped package without a comment gives its declaration as the reason
        make(self.root, "bare", 'os = ["windows"]')
        self.assertIn("bare skipped declared windows only", run(self.root, "list").stdout)

    def test_static_check_flags_unguarded_system_externs_and_windows_imports(self):
        extern = "extern { fn @c(system) Sleep(ms: u32); }\n"
        make(self.root, "bad", src=extern)
        make(self.root, "guarded", src="#if #os == .windows { " + extern + "}\n")
        make(self.root, "declared", 'os = ["windows"]', src=extern)
        make(self.root, "importer", imports=["declared"])
        run1 = run(self.root, "static")
        self.assertEqual(run1.returncode, 1)
        lines = run1.stdout.splitlines()
        self.assertEqual(len(lines), 2, lines)
        self.assertTrue(lines[0].startswith("bad: claims linux, but src/bad.cone has @c(system)"))
        self.assertEqual(lines[1], "importer: claims linux, but imports declared, which does not")

    def test_the_repo_passes_its_own_static_check(self):
        out = subprocess.run([sys.executable, "-I", str(TOOL), "static"], capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout)


if __name__ == "__main__":
    unittest.main()
