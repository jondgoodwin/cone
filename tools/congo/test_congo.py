#!/usr/bin/env python3
"""Congo's own checks: python tools/congo/test_congo.py

The header scan is checked on its own. Everything else runs congo.py as a user
would, in a fresh temporary folder with its own Congo home, against the
repository's conec (build it first: python test/run.py --build) and its
packages/. Every expected output below was worked out by hand from the source
it prints from, not copied from a run.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import congo  # noqa: E402

CONGO = [sys.executable, str(HERE / "congo.py")]


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(text).lstrip("\n"), encoding="utf-8")


class HeaderScan(unittest.TestCase):
    def scan(self, text: str) -> congo.Header:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "f.cone"
            write(path, text)
            return congo.scan_header(path)

    def test_comments_then_mod_then_imports_then_stop(self):
        header = self.scan("""
            // a line comment
            /* a block /* nested */ comment
               over lines */
            mod prog extends base is Shell use {run, stop};
            import stdio use *;
            pub import geometry use Point as P, Line;
            import "../q/q";
            fn main() {}
            import late;
            """)
        self.assertEqual(header.mod, "prog")
        self.assertEqual([i.written for i in header.imports],
                         ["stdio", "geometry", '"../q/q"'])
        self.assertEqual([i.line for i in header.imports], [5, 6, 7])

    def test_extends_is_read_from_the_mod_line(self):
        header = self.scan("mod tower extends plinth is Shell;\nimport stdio;")
        self.assertEqual(header.mod, "tower")
        self.assertEqual((header.extends.name, header.extends.line), ("plinth", 1))
        self.assertEqual([i.name for i in header.imports], ["stdio"])
        self.assertIsNone(self.scan("mod plain use run;").extends)

    def test_c_named_and_pub_mod_lines(self):
        self.assertEqual(self.scan('mod @c("SDL_") sdl;\nimport a;').mod, "sdl")
        self.assertEqual(self.scan("mod @c(system) win;").mod, "win")
        self.assertEqual(self.scan("pub mod lexer;\nimport tokens;").mod, "lexer")

    def test_a_generic_mod_line(self):
        header = self.scan("pub mod stack[T, U] extends base;\nimport seq;")
        self.assertEqual(header.mod, "stack")
        self.assertEqual(header.extends.name, "base")
        self.assertEqual([i.name for i in header.imports], ["seq"])

    def test_a_mod_trait_is_not_a_mod_line(self):
        header = self.scan("mod trait Shell {\n  fn run();\n}\nimport stdio;")
        self.assertIsNone(header.mod)
        self.assertEqual(header.imports, [])

    def test_no_mod_line(self):
        header = self.scan("import stdio use *;\nimport q;\n\nfn main() {}")
        self.assertIsNone(header.mod)
        self.assertEqual([i.name for i in header.imports], ["stdio", "q"])


class Scenarios(unittest.TestCase):
    """Congo run as a command, in a temporary folder."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.env = dict(os.environ, CONGO_HOME=str(self.root / "home"))
        self.env.pop("CONEC", None)

    def tearDown(self):
        self.tmp.cleanup()

    def congo(self, *args: str, cwd: Path, ok: bool = True) -> subprocess.CompletedProcess:
        run = subprocess.run([*CONGO, *args], cwd=cwd, env=self.env,
                             capture_output=True, text=True)
        if ok and run.returncode != 0:
            self.fail(f"congo {' '.join(args)} failed ({run.returncode}):\n"
                      f"{run.stdout}{run.stderr}")
        return run

    def program_output(self, run: subprocess.CompletedProcess) -> str:
        """What the program printed: everything after Congo's 'Running' line."""
        lines = run.stdout.splitlines()
        at = next(i for i, line in enumerate(lines) if line.strip().startswith("Running"))
        return "\n".join(lines[at + 1:]) + "\n"

    def registry(self, *folders: Path) -> None:
        listed = ", ".join(f'"{f.as_posix()}"' for f in folders)
        write(self.root / "home" / "config.toml", f"[registry]\nfolders = [{listed}]\n")

    def test_new_then_run(self):
        self.congo("new", "hello", cwd=self.root)
        pkg = self.root / "hello"
        self.assertTrue((pkg / "congo.toml").is_file())
        self.assertTrue((pkg / "src" / "hello.cone").is_file())
        self.assertTrue((pkg / "tests").is_dir())
        run = self.congo("run", cwd=pkg)
        # The template prints one line, through stdio's own object
        self.assertEqual(self.program_output(run), "Hello, world!\n")
        out = pkg / "build" / "debug"
        for name in ("core", "stdio", "hello"):
            self.assertTrue((out / f"{name}.conebuild").is_file(), name)
            self.assertTrue((out / f"{name}{congo.OBJ_EXT}").is_file(), name)
        # Each package compiled alone: stdio's description lists stdio's source,
        # the program's names stdio's include file
        stdio_desc = (out / "stdio.conebuild").read_text()
        self.assertIn("output: library", stdio_desc)
        self.assertIn("packages/stdio/src/stdio.cone", stdio_desc)
        hello_desc = (out / "hello.conebuild").read_text()
        self.assertIn("output: executable", hello_desc)
        self.assertRegex(hello_desc, r'import stdio: ".*/packages/stdio/stdio\.cone"')
        # From a subfolder too: the manifest is found by walking up
        run = self.congo("run", cwd=pkg / "src")
        self.assertEqual(self.program_output(run), "Hello, world!\n")
        self.congo("clean", cwd=pkg)
        self.assertFalse((pkg / "build").exists())

    def test_a_package_with_submodules_importing_stdio(self):
        self.congo("new", "show", cwd=self.root)
        pkg = self.root / "show"
        write(pkg / "src" / "show.cone", """
            // The root: its own file, one organisational file, a one-file module
            // and a folder module, each of which prints through stdio
            mod show;

            import stdio use *;

            fn main() i32 {
              print <- "sum = ";
              print <- 19i64 + 23i64;
              print <- "\\n";
              imm half = 7.0d / 2.0d;
              print <- "half = ";
              print <- half;
              print <- "\\n";
              print <- "big = ";
              print <- 3000000000u64;
              print <- "\\n";
              printInt(-5i64);
              printStr("\\n");
              fmt.line("twice", twice.of(21i64));
              fmt.line("tripled", tripled(5i64));
              0i32;
            }
            """)
        write(pkg / "src" / "util" / "extra.cone", """
            fn tripled(n i64) i64 {
              n * 3i64;
            }
            """)
        write(pkg / "src" / "twice.cone", """
            mod twice;

            pub fn of(n i64) i64 {
              n + n;
            }
            """)
        write(pkg / "src" / "fmt" / "fmt.cone", """
            mod fmt;

            import stdio;

            pub fn line(label &[]u8, n i64) {
              stdio.printStr(label);
              stdio.print <- " = ";
              stdio.print <- n;
              stdio.print <- "\\n";
            }
            """)
        run = self.congo("run", cwd=pkg)
        # 19+23 = 42; 7/2 = 3.5, printed by %g; 3000000000 fits no i32, printed
        # unsigned; -5 through the C function directly; twice.of(21) = 42;
        # tripled(5) = 15, from the organisational file
        self.assertEqual(self.program_output(run),
                         "sum = 42\nhalf = 3.5\nbig = 3000000000\n-5\n"
                         "twice = 42\ntripled = 15\n")
        desc = (pkg / "build" / "debug" / "show.conebuild").read_text()
        # The organisational file joins the root; each child module has its
        # own import line where it imports
        self.assertIn("src/util/extra.cone", desc)
        self.assertRegex(desc, r"fmt: \{\n.*fmt\.cone\"\n\s+import stdio:")
        self.assertRegex(desc, r"twice: \{\n.*twice\.cone\"\n\s+\}")
        # Release: the same program, optimised
        run = self.congo("run", "--release", cwd=pkg)
        self.assertIn("twice = 42", self.program_output(run))
        self.assertTrue((pkg / "build" / "release" / f"show{congo.EXE_EXT}").is_file())
        self.assertIn("build: release",
                      (pkg / "build" / "release" / "show.conebuild").read_text())

    def test_a_lone_file(self):
        write(self.root / "lone.cone", """
            mod lone;

            import stdio use *;

            fn main() i32 {
              print <- "lone ";
              print <- 6i64 * 7i64;
              print <- "\\n";
              0i32;
            }
            """)
        run = self.congo("run", "lone.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "lone 42\n")
        # Nothing is written beside the file: a lone file's build is in the home
        self.assertFalse((self.root / "build").exists())
        self.congo("clean", "lone.cone", cwd=self.root)
        self.assertEqual(list((self.root / "home" / "lone").iterdir()), [])

    def test_a_package_from_the_registry_importing_stdio(self):
        packages = self.root / "mypackages"
        self.registry(packages)
        # The folder's name is not the package's: the manifest names it
        write(packages / "greeting" / "congo.toml",
              '[package]\nname = "greet"\nversion = "1.2.3"\noutput = "library"\n')
        write(packages / "greeting" / "src" / "greet.cone", """
            mod greet;

            import stdio;

            pub mut count i64 = 3i64;

            pub fn hello(n i64) {
              stdio.print <- "greet ";
              stdio.print <- n;
              stdio.print <- "\\n";
              count = count + 1i64;
            }
            """)
        write(packages / "greeting" / "greet.cone", """
            mod greet;

            pub extern mut count i64;

            pub extern fn hello(n i64);
            """)
        write(self.root / "app.cone", """
            mod app;

            import greet;

            fn main() i32 {
              greet.hello(greet.count + 4i64);
              greet.hello(greet.count * 10i64);
              0i32;
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # count starts at 3: hello(3+4) prints 7 and makes it 4; hello(4*10)
        # prints 40. Built in order: core, stdio (greet's import), greet, app
        self.assertEqual(self.program_output(run), "greet 7\ngreet 40\n")
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["core", "stdio", "greet", "app"])

    def test_an_import_loop_between_packages_is_refused(self):
        packages = self.root / "loop"
        self.registry(packages)
        for name, other in (("ping", "pong"), ("pong", "ping")):
            write(packages / name / "congo.toml",
                  f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n')
            write(packages / name / "src" / f"{name}.cone",
                  f"mod {name};\n\nimport {other};\n\npub fn f() {{}}\n")
            write(packages / name / f"{name}.cone", f"mod {name};\n\npub extern fn f();\n")
        write(self.root / "p.cone", "import ping;\n\nfn main() i32 {\n  0i32;\n}\n")
        run = self.congo("run", "p.cone", cwd=self.root, ok=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn("import loop between packages: ping -> pong -> ping", run.stderr)

    def test_an_import_loop_between_modules_is_refused(self):
        # Two sisters importing each other, and, once that is fixed, a child
        # importing a name of its parent: both loops, found before conec runs
        self.congo("new", "tree", cwd=self.root)
        pkg = self.root / "tree"
        write(pkg / "src" / "tree.cone", """
            mod tree;

            pub fn shared() i64 {
              1i64;
            }

            fn main() i32 {
              0i32;
            }
            """)
        write(pkg / "src" / "lexer.cone", "mod lexer;\n\nimport parser;\n")
        write(pkg / "src" / "parser" / "parser.cone", "mod parser;\n\nimport lexer;\n")
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn("import loop between modules of package tree:"
                      " tree.lexer -> tree.parser -> tree.lexer", run.stderr)
        self.assertIn("tree.parser imports tree.lexer at", run.stderr)
        self.assertFalse((pkg / "build" / "debug" / "tree.conebuild").exists())

        write(pkg / "src" / "parser" / "parser.cone", "mod parser extends lexer;\n")
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertIn("tree.parser extends tree.lexer at", run.stderr)

        write(pkg / "src" / "parser" / "parser.cone", "mod parser;\n\nimport shared;\n")
        run = self.congo("build", cwd=pkg, ok=False)
        # lexer still imports parser, so the walk reaches parser through it
        self.assertIn("import loop between modules of package tree:"
                      " tree -> tree.lexer -> tree.parser -> tree", run.stderr)
        self.assertIn("tree contains tree.lexer; tree.lexer imports tree.parser at",
                      run.stderr)
        self.assertIn("tree.parser imports shared of tree at", run.stderr)
        self.assertIn("move what they share into a sister both import", run.stderr)

        # A sister both import, and the loop is gone
        write(pkg / "src" / "parser" / "parser.cone", "mod parser;\n")
        self.congo("build", cwd=pkg)

    def test_an_unknown_import_is_named(self):
        write(self.root / "p.cone", "import nosuch;\n\nfn main() i32 {\n  0i32;\n}\n")
        run = self.congo("run", "p.cone", cwd=self.root, ok=False)
        self.assertIn("p.cone:1: import nosuch: no package named 'nosuch'", run.stderr)

    def test_a_manifest_is_checked(self):
        self.congo("new", "lib1", "--lib", cwd=self.root)
        pkg = self.root / "lib1"
        self.assertTrue((pkg / "lib1.cone").is_file())      # its include file
        self.congo("build", cwd=pkg)
        self.assertTrue((pkg / "build" / "debug" / f"lib1{congo.OBJ_EXT}").is_file())
        run = self.congo("run", cwd=pkg, ok=False)
        self.assertIn("is a library", run.stderr)
        write(pkg / "congo.toml", '[package]\nname = "lib1"\nversion = "1.0"\n'
                                  'output = "library"\n')
        self.assertIn("MAJOR.MINOR.PATCH", self.congo("build", cwd=pkg, ok=False).stderr)
        write(pkg / "congo.toml", '[package]\nname = "lib1"\nversion = "1.0.0"\n'
                                  'output = "library"\n[dependencies]\nstdio = "1"\n')
        self.assertIn("no dependencies section",
                      self.congo("build", cwd=pkg, ok=False).stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
