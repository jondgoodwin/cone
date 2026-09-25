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
import shutil
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
IS_WINDOWS = congo.IS_WINDOWS


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
        header = self.scan('mod @c("SDL_") sdl;\nimport a;')
        self.assertEqual((header.mod, header.c), ("sdl", True))
        self.assertEqual([i.name for i in header.imports], ["a"])
        self.assertTrue(self.scan("mod @c(system) win;").c)
        self.assertTrue(self.scan("mod @c k32;").c)
        header = self.scan("pub mod lexer;\nimport tokens;")
        self.assertEqual((header.mod, header.c), ("lexer", False))

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
        # core and stdio each compiled alone, as libraries, and each compile
        # generated its include file beside its object
        for name in ("core", "stdio"):
            generated = (out / f"{name}.cone").read_text()
            self.assertTrue(generated.startswith(
                f"// Generated by conec: the include file of package {name}"), name)
        self.assertIn("pub extern mut print IOStream;", (out / "stdio.cone").read_text())
        # stdio's description lists stdio's source, and names core's generated
        # include file, where the prelude is loaded from
        stdio_desc = (out / "stdio.conebuild").read_text()
        self.assertIn("output: library", stdio_desc)
        self.assertIn("packages/stdio/src/stdio.cone", stdio_desc)
        self.assertRegex(stdio_desc, r'import core: ".*/hello/build/debug/core\.cone"')
        # core's own description names no prelude: core is the prelude
        self.assertNotIn("import core", (out / "core.conebuild").read_text())
        # The program is compiled against the generated include files, core's
        # and stdio's, never against a file at a package's root
        hello_desc = (out / "hello.conebuild").read_text()
        self.assertIn("output: executable", hello_desc)
        self.assertRegex(hello_desc, r'\nimport core: ".*/hello/build/debug/core\.cone"\n')
        self.assertRegex(hello_desc, r'\nimport stdio: ".*/hello/build/debug/stdio\.cone"\n')
        self.assertRegex(hello_desc[hello_desc.index("hello: {"):],
                         r'import stdio: ".*/hello/build/debug/stdio\.cone"')
        self.assertNotIn("packages/stdio/stdio.cone", hello_desc)
        self.assertNotIn("warning", run.stderr)
        # From a subfolder too: the manifest is found by walking up
        run = self.congo("run", cwd=pkg / "src")
        self.assertEqual(self.program_output(run), "Hello, world!\n")
        # clean removes the build folder, the generated include files with it
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
        # The folder's name is not the package's: the manifest names it, and
        # names the include file greet's compile generates
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
        # An include file written by hand at the package's root, out of date: it
        # is not read, and Congo says so
        write(packages / "greeting" / "greet.cone", """
            mod greet;

            pub extern fn gone();
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
        self.assertIn("greeting", run.stderr)
        self.assertIn("greet.cone is not used", run.stderr)
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        self.assertIn("pub extern fn hello(n i64);", (out / "greet.cone").read_text())

    def test_an_include_file_imports_another_package(self):
        # Three packages: b defines Counter and its method; a imports b and
        # takes and returns b's Counter; the program imports a alone. a's
        # generated include file copies a's import of b, since an include file
        # is a module file like any other, and the program's description lists
        # b among its package lines for it to find [Jon 25 Sep]
        packages = self.root / "chain"
        self.registry(packages)
        for name in ("a", "b"):
            write(packages / name / "congo.toml",
                  f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n')
        write(packages / "b" / "src" / "b.cone", """
            mod b;

            pub struct Counter {
              pub count i64;
              pub step i64;

              pub fn next(self) i64 {
                count + step;
              }
            }
            """)
        write(packages / "a" / "src" / "a.cone", """
            mod a;

            import b;

            pub fn start(n i64) b.Counter {
              b.Counter[n, 5i64];
            }

            pub fn advance(c b.Counter) b.Counter {
              b.Counter[c.next(), c.step];
            }
            """)
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import a;

            fn main() i32 {
              imm c = a.start(30i64);
              stdio.print <- c.next();
              stdio.print <- "\\n";
              imm d = a.advance(a.advance(c));
              stdio.print <- d.count;
              stdio.print <- "\\n";
              stdio.print <- d.step;
              stdio.print <- "\\n";
              0i32;
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # start(30) is Counter[30, 5], whose next() is 30+5 = 35, from b's
        # object. Advanced twice: [35, 5], then [40, 5], built in a's object
        self.assertEqual(self.program_output(run), "35\n40\n5\n")
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["core", "stdio", "b", "a", "app"])
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        desc = (out / "app.conebuild").read_text()
        # The package lines: the whole closure, b included though the program
        # does not import it, each after what it imports, the prelude first
        tops = [line.split(":")[0] for line in desc.splitlines()
                if line.startswith("import ")]
        self.assertEqual(tops, ["import core", "import stdio", "import b", "import a"])
        self.assertRegex(desc, r'\nimport b: ".*/app-[0-9a-f]+/debug/b\.cone"\n')
        # The program's own module imports only what it writes
        module = desc[desc.index("app: {"):]
        self.assertIn("import a:", module)
        self.assertIn("import stdio:", module)
        self.assertNotIn("import b:", module)
        # a's description lists b both ways: its module imports b, and b's is
        # the one include file its compile loads
        a_desc = (out / "a.conebuild").read_text()
        self.assertRegex(a_desc, r'\nimport b: ".*/app-[0-9a-f]+/debug/b\.cone"\n')
        self.assertRegex(a_desc[a_desc.index("a: {"):], r'import b: ".*/debug/b\.cone"')
        # a's generated include file imports b, as a's source does
        self.assertIn("\nimport b;\n", (out / "a.cone").read_text())

    def test_a_library_with_submodules(self):
        # A library laid out as a root, a folder submodule holding a one-file
        # submodule of its own, and a one-file submodule, its API re-exported at
        # the root. Its generated include file declares what the root reaches in
        # each submodule in a private nested block [Jon 25 Sep], and the program
        # reaches everything through the root
        packages = self.root / "libs"
        self.registry(packages)
        write(packages / "coll" / "congo.toml",
              '[package]\nname = "coll"\nversion = "0.1.0"\noutput = "library"\n')
        write(packages / "coll" / "src" / "coll.cone", """
            mod coll;

            pub use vec Stack;
            pub use kinds.Kind;

            pub fn classify(n i64) kinds.Kind {
              if n < 10i64 {Small[];} else {Large[];}
            }

            pub fn code(k kinds.Kind) i64 {
              if k is Small {1i64;} else {2i64;}
            }

            // Expanded in the program: it names vec's function and vec.cell's
            pub fn total(s Stack) i64 inline {
              vec.sum(s) + vec.cell.bonus();
            }
            """)
        write(packages / "coll" / "src" / "vec" / "vec.cone", """
            pub mod vec;

            pub struct Stack {
              pub a i64;
              pub b i64;

              pub fn swap(self) Stack {
                Stack[b, a];
              }
            }

            pub fn sum(s Stack) i64 {
              s.a + s.b;
            }

            // Reached by nothing outside vec: not in the include file
            pub fn unseen() i64 {
              0i64;
            }
            """)
        write(packages / "coll" / "src" / "vec" / "cell.cone", """
            pub mod cell;

            pub fn bonus() i64 {
              100i64;
            }
            """)
        write(packages / "coll" / "src" / "kinds.cone", """
            pub mod kinds;

            pub enum Kind {
              Small, Large;
            }
            """)
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import coll;

            fn main() i32 {
              imm s = coll.Stack[3i64, 4i64].swap();
              stdio.print <- s.a;
              stdio.print <- "\\n";
              stdio.print <- coll.total(s);
              stdio.print <- "\\n";
              stdio.print <- coll.code(coll.classify(20i64));
              stdio.print <- "\\n";
              0i32;
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # Stack[3, 4] swapped is [4, 3]: a is 4; total is 4 + 3 from vec's
        # object, plus 100 from vec.cell's; 20 is Large, whose code is 2
        self.assertEqual(self.program_output(run), "4\n107\n2\n")
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        generated = (out / "coll.cone").read_text()
        # The root's blocks are private, whatever their source said; a block
        # inside one keeps its 'pub', which opens it only to the package, so the
        # root reaches vec.cell as its source does
        for block in ("\nmod kinds {", "\nmod vec {", "\npub mod cell {"):
            self.assertIn(block, generated)
        self.assertNotIn("pub mod vec", generated)
        self.assertNotIn("pub mod kinds", generated)
        self.assertIn("pub extern fn sum(s Stack) i64;", generated)
        self.assertIn("pub extern fn bonus() i64;", generated)
        self.assertNotIn("unseen", generated)
        # The program's description names coll's generated include file, and
        # coll's lists its module tree
        self.assertRegex((out / "app.conebuild").read_text(), r'import coll: ".*/debug/coll\.cone"')
        coll_desc = (out / "coll.conebuild").read_text()
        self.assertRegex(coll_desc, r"vec: \{\n.*vec\.cone\"\n\s+cell: \{")

    @unittest.skipUnless(IS_WINDOWS,"shlwapi is a Windows system library")
    def test_a_c_package_links_the_library_it_names(self):
        # A C package: its source is one '@c' module of declarations, its
        # include file is generated as any package's is, and its manifest names
        # the C library.
        # shlwapi is part of every Windows SDK and, unlike kernel32, is not
        # linked unless named, so the link fails until [link] names it
        packages = self.root / "cpkgs"
        self.registry(packages)
        write(packages / "winstr" / "congo.toml",
              '[package]\nname = "winstr"\nversion = "0.1.0"\noutput = "library"\n')
        write(packages / "winstr" / "src" / "winstr.cone", """
            // shlwapi's string functions, the part used here: '@c("Str")' binds
            // ToIntA to the C function StrToIntA
            mod @c("Str") winstr;

            pub extern {
              fn ToIntA(s *u8) i32;
            }
            """)
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import winstr;

            fn main() i32 {
              stdio.print <- winstr.ToIntA(&"1234" as *u8);
              stdio.print <- "\\n";
              stdio.print <- winstr.ToIntA(&"-56" as *u8) + 100i32;
              stdio.print <- "\\n";
              0i32;
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn("unresolved external symbol StrToIntA", run.stderr)

        write(packages / "winstr" / "congo.toml",
              '[package]\nname = "winstr"\nversion = "0.1.0"\noutput = "library"\n'
              '\n[link]\nlibraries = ["shlwapi"]\n')
        run = self.congo("run", "app.cone", cwd=self.root)
        # StrToIntA("1234") is 1234; StrToIntA("-56") is -56, and -56 + 100 = 44
        self.assertEqual(self.program_output(run), "1234\n44\n")
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        # The program is compiled against the C package's generated include
        # file, which keeps its '@c' line, and the C package's object, which
        # defines nothing, is linked like any other
        self.assertRegex((out / "app.conebuild").read_text(),
                         r'import winstr: ".*/debug/winstr\.cone"')
        self.assertIn('mod @c("Str") winstr;', (out / "winstr.cone").read_text())
        self.assertTrue((out / f"winstr{congo.OBJ_EXT}").is_file())

        # So a C package may hold more than the one file: a helper of its own,
        # which its object defines and nothing outside it reaches
        write(packages / "winstr" / "src" / "more.cone", "fn helper() {}\n")
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "1234\n44\n")
        self.assertNotIn("helper", (out / "winstr.cone").read_text())

    def test_link_paths_find_a_c_library_in_the_package(self):
        # A C library built here, into a folder of the package that only [link]
        # paths names: 'triple' multiplies by three
        packages = self.root / "cpkgs"
        self.registry(packages)
        pkg = packages / "tri"
        write(pkg / "csrc" / "triple.c", "int triple(int n) { return 3 * n; }\n")
        (pkg / "clib").mkdir()
        if congo.IS_WINDOWS:
            env = congo.Linker.vs_environment()
            for tool, *args in (["cl", "/nologo", "/c", "/Fo:triple.obj", "triple.c"],
                                ["lib", "/nologo", "triple.obj", "/OUT:../clib/tri3.lib"]):
                found = shutil.which(tool, path=env["PATH"])
                subprocess.run([found, *args], cwd=pkg / "csrc", env=env, check=True,
                               capture_output=True)
        else:
            for command in (["cc", "-c", "-fPIC", "-o", "triple.o", "triple.c"],
                            ["ar", "rcs", "../clib/libtri3.a", "triple.o"]):
                subprocess.run(command, cwd=pkg / "csrc", check=True, capture_output=True)
        write(pkg / "congo.toml",
              '[package]\nname = "tri"\nversion = "0.1.0"\noutput = "library"\n'
              '\n[link]\nlibraries = ["tri3"]\npaths = ["clib"]\n')
        write(pkg / "src" / "tri.cone", "mod @c tri;\n\npub extern fn triple(n i32) i32;\n")
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import tri;

            fn main() i32 {
              stdio.print <- tri.triple(14i32);
              stdio.print <- "\\n";
              0i32;
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # 3 * 14 = 42
        self.assertEqual(self.program_output(run), "42\n")

    def test_an_import_loop_between_packages_is_refused(self):
        packages = self.root / "loop"
        self.registry(packages)
        for name, other in (("ping", "pong"), ("pong", "ping")):
            write(packages / name / "congo.toml",
                  f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n')
            write(packages / name / "src" / f"{name}.cone",
                  f"mod {name};\n\nimport {other};\n\npub fn f() {{}}\n")
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
        # No include file is written by hand: the build generates it
        self.assertFalse((pkg / "lib1.cone").exists())
        self.congo("build", cwd=pkg)
        self.assertTrue((pkg / "build" / "debug" / f"lib1{congo.OBJ_EXT}").is_file())
        self.assertIn("pub extern fn answer() i64;",
                      (pkg / "build" / "debug" / "lib1.cone").read_text())
        run = self.congo("run", cwd=pkg, ok=False)
        self.assertIn("is a library", run.stderr)
        write(pkg / "congo.toml", '[package]\nname = "lib1"\nversion = "1.0"\n'
                                  'output = "library"\n')
        self.assertIn("MAJOR.MINOR.PATCH", self.congo("build", cwd=pkg, ok=False).stderr)
        write(pkg / "congo.toml", '[package]\nname = "lib1"\nversion = "1.0.0"\n'
                                  'output = "library"\n[dependencies]\nstdio = "1"\n')
        self.assertIn("no dependencies section",
                      self.congo("build", cwd=pkg, ok=False).stderr)
        # [link] names a library by its bare name, and has two keys
        head = '[package]\nname = "lib1"\nversion = "1.0.0"\noutput = "library"\n'
        write(pkg / "congo.toml", head + '[link]\nlibraries = ["SDL2.lib"]\n')
        self.assertIn("bare name", self.congo("build", cwd=pkg, ok=False).stderr)
        write(pkg / "congo.toml", head + '[link]\nlibs = ["SDL2"]\n')
        self.assertIn("'libs' is not a [link] key",
                      self.congo("build", cwd=pkg, ok=False).stderr)
        write(pkg / "congo.toml", head + '[link]\nlibraries = ["SDL2"]\npaths = ["x"]\n')
        self.congo("build", cwd=pkg)     # a library links nothing, so names are only read


if __name__ == "__main__":
    unittest.main(verbosity=2)
