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
import re
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


class NotNames(unittest.TestCase):
    def test_the_words_that_cannot_name_a_module_are_the_compilers(self):
        # Every keyAdd in the lexer but an attribute's, which no file or folder
        # name can spell; and every permission corelib makes
        lexer = (congo.REPO / "compiler" / "c" / "parser" / "lexer.c").read_text()
        added = re.findall(r'keyAdd\("([a-z0-9_]+)", *(\w+)\)', lexer)
        self.assertEqual(congo.KEYWORDS, {w for w, tok in added if tok != "ReservedToken"})
        self.assertEqual(congo.RESERVED, {w for w, tok in added if tok == "ReservedToken"})
        corelib = (congo.REPO / "compiler" / "c" / "corelib" / "corelib.c").read_text()
        self.assertEqual(congo.PERMISSIONS,
                         set(re.findall(r'newPermNodeStr\("(\w+)"', corelib)))
        self.assertIsNone(congo.name_fault("usecheck"))
        self.assertIn("keyword", congo.name_fault("use"))
        self.assertIn("reserved", congo.name_fault("yield"))
        self.assertIn("permission", congo.name_fault("mut"))
        self.assertIn("not a Cone name", congo.name_fault("two-words"))


@unittest.skipUnless(IS_WINDOWS, "an inherited Visual Studio environment is Windows'")
class InheritedLinker(unittest.TestCase):
    """Which inherited link.exe links for x64, and what LIB keeps when Congo
    takes vcvars64.bat's environment instead. The folders are as Visual Studio
    2022's Developer Command Prompt sets them."""
    MSVC = r"C:\VS\VC\Tools\MSVC\14.42.34433"

    def test_the_linkers_folder_names_its_target(self):
        target = congo.msvc_target
        self.assertEqual(target(self.MSVC + r"\bin\HostX86\x86\link.exe", {}), "x86")
        self.assertEqual(target(self.MSVC + r"\bin\Hostx64\x64\link.exe", {}), "x64")
        self.assertEqual(target(self.MSVC + r"\bin\HostX86\x64\link.exe", {}), "x64")
        # The folder is the linker that would run, whatever the variable says
        self.assertEqual(target(self.MSVC + r"\bin\HostX86\x86\link.exe",
                                {"VSCMD_ARG_TGT_ARCH": "x64"}), "x86")

    def test_else_the_environment_names_it(self):
        self.assertEqual(congo.msvc_target(r"C:\tools\link.exe",
                                           {"VSCMD_ARG_TGT_ARCH": "x86"}), "x86")
        self.assertIsNone(congo.msvc_target(r"C:\tools\link.exe", {}))

    def test_lib_keeps_the_users_folders_and_drops_visual_studios(self):
        kits = r"C:\Program Files (x86)\Windows Kits\10"
        env = {"VSINSTALLDIR": "C:\\VS\\", "WINDOWSSDKDIR": kits + "\\",
               "LIB": ";".join([r"C:\libs\SDL2\lib\x64", self.MSVC + r"\ATLMFC\lib\x86",
                                self.MSVC + r"\lib\x86", kits + r"\lib\10.0.22621.0\ucrt\x86",
                                kits + r"\\lib\10.0.22621.0\\um\x86", r"C:\mine", ""]),
               "PATH": self.MSVC + r"\bin\HostX86\x86"}
        cleaned = congo.without_vs_libs(env)
        self.assertEqual(cleaned["LIB"], r"C:\libs\SDL2\lib\x64;C:\mine")
        self.assertEqual(cleaned["PATH"], env["PATH"])
        # A folder merely beginning with Visual Studio's name is the user's
        self.assertEqual(congo.without_vs_libs({"VSINSTALLDIR": r"C:\VS",
                                                "LIB": r"C:\VS2;C:\VS\lib"})["LIB"],
                         r"C:\VS2")

    def test_set_output_replaces_a_variable_in_any_spelling(self):
        # A Command Prompt's set prints Path, where os.environ spells it PATH
        env = congo.with_set_output({"PATH": r"C:\old", "LIB": r"C:\mine"},
                                    "Path=C:\\new;C:\\old\nVSCMD_VER=17.12.0\n")
        self.assertEqual(env, {"PATH": r"C:\new;C:\old", "LIB": r"C:\mine",
                               "VSCMD_VER": "17.12.0"})

    def test_no_linker_says_what_ran_and_where_it_looked(self):
        text = congo.Linker.no_linker(
            r"C:\Git\usr\bin\link.exe", r"C:\a;C:\Git\usr\bin",
            (r"C:\VS\vcvars64.bat", "banner\n[ERROR:vcvars.bat] Toolset directory not found\n"))
        self.assertEqual(text.splitlines()[1:], [
            r"the link.exe found, C:\Git\usr\bin\link.exe, is not Microsoft's",
            r"ran C:\VS\vcvars64.bat, which printed (the end of it):",
            "    banner", "    [ERROR:vcvars.bat] Toolset directory not found",
            "searched for link.exe in PATH's folders:", r"    C:\a", r"    C:\Git\usr\bin"])

    def test_vcvars64_keeps_what_lib_lists_behind_its_own(self):
        mine = str(Path(tempfile.gettempdir()) / "congo-own-libs")
        base = {k: v for k, v in os.environ.items() if k.upper() != "LIB"}
        lib = congo.env_value(congo.Linker.vs_environment({**base, "LIB": mine}), "LIB")
        entries = [e for e in lib.split(";") if e]
        self.assertEqual(entries[-1], mine)
        self.assertTrue(entries[0].lower().endswith("x64"), entries[0])


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
        # prints 40. Built in order: libc (core's import), core, stdio (greet's
        # import), greet, app
        self.assertEqual(self.program_output(run), "greet 7\ngreet 40\n")
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["libc", "core", "stdio", "greet", "app"])
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
        self.assertEqual(compiled, ["libc", "core", "stdio", "b", "a", "app"])
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        desc = (out / "app.conebuild").read_text()
        # The package lines: the whole closure, b included though the program
        # does not import it, each after what it imports, the prelude first
        # after libc, which the prelude imports
        tops = [line.split(":")[0] for line in desc.splitlines()
                if line.startswith("import ")]
        self.assertEqual(tops, ["import libc", "import core", "import stdio", "import b", "import a"])
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

    def test_an_indirect_package_runs_its_init_once(self):
        # A diamond: the program imports a and c, each of which imports b, and
        # b holds a submodule. The program never imports b, yet b and its
        # submodule are modules of its compile, through a's and c's include
        # files, so the program's initAll() runs their 'init's and finalAll()
        # their 'final's
        packages = self.root / "diamond"
        self.registry(packages)
        for name in ("a", "b", "c"):
            write(packages / name / "congo.toml",
                  f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n')
        write(packages / "b" / "src" / "b.cone", """
            mod b;

            import stdio;

            pub imm value i64;

            fn @initpure init() {
              value = sub.base + 7i64;
              stdio.print <- "b init ";
              stdio.print <- value;
              stdio.print <- "\\n";
            }

            fn final() {
              stdio.print <- "b final\\n";
            }

            pub fn get() i64 {
              value;
            }
            """)
        write(packages / "b" / "src" / "sub.cone", """
            pub mod sub;

            import stdio;

            pub imm base i64;

            fn @initpure init() {
              base = 100i64;
              stdio.print <- "b.sub init\\n";
            }

            fn final() {
              stdio.print <- "b.sub final\\n";
            }
            """)
        write(packages / "a" / "src" / "a.cone", """
            mod a;

            import stdio;
            import b;

            fn @initpure init() {
              stdio.print <- "a init ";
              stdio.print <- b.get();
              stdio.print <- "\\n";
            }

            fn final() {
              stdio.print <- "a final\\n";
            }

            pub fn readB() i64 {
              b.value;
            }
            """)
        write(packages / "c" / "src" / "c.cone", """
            mod c;

            import stdio;
            import b;

            fn @initpure init() {
              stdio.print <- "c init\\n";
            }

            fn final() {
              stdio.print <- "c final\\n";
            }

            pub fn twiceB() i64 {
              b.get() * 2i64;
            }
            """)
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import a;
            import c;

            fn main() i32 {
              initAll();
              stdio.print <- "main ";
              stdio.print <- a.readB();
              stdio.print <- " ";
              stdio.print <- c.twiceB();
              stdio.print <- "\\n";
              finalAll();
              0i32;
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # b's submodule first, since b contains it: base is 100. Then b, once
        # though a and c both import it: value is 100+7 = 107, which a's init
        # reads back. Then c. main reads 107 through a and 2*107 = 214 through
        # c. Every final in exactly the reverse, each once
        self.assertEqual(self.program_output(run),
                         "b.sub init\nb init 107\na init 107\nc init\n"
                         "main 107 214\n"
                         "c final\na final\nb final\nb.sub final\n")
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["libc", "core", "stdio", "b", "a", "c", "app"])
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        # b's generated include file declares b's 'init' and 'final' and its
        # submodule's, which is what the program's stitched pair calls
        generated = (out / "b.cone").read_text()
        self.assertEqual(generated.count("extern fn @initpure init();"), 2)
        self.assertEqual(generated.count("extern fn final();"), 2)
        self.assertIn("\nmod sub {", generated)
        # The program's own module imports a and c alone
        desc = (out / "app.conebuild").read_text()
        self.assertNotIn("import b:", desc[desc.index("app: {"):])

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

    def test_the_prelude_rests_on_libc(self):
        # core imports libc, a C package the compiler gives no prelude, so libc
        # is compiled first, with no package line for core, whose include file
        # does not exist yet; every later description names libc before core
        self.congo("new", "hello", cwd=self.root)
        pkg = self.root / "hello"
        run = self.congo("run", cwd=pkg)
        self.assertEqual(self.program_output(run), "Hello, world!\n")
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["libc", "core", "stdio", "hello"])
        out = pkg / "build" / "debug"
        self.assertNotIn("import", (out / "libc.conebuild").read_text())
        self.assertIn("import libc pub use malloc;", (out / "core.cone").read_text())
        self.assertIn('mod @c libc;', (out / "libc.cone").read_text())
        core_desc = (out / "core.conebuild").read_text()
        self.assertRegex(core_desc, r'\nimport libc: ".*/hello/build/debug/libc\.cone"\n')
        self.assertNotIn("import core", core_desc)
        hello_desc = (out / "hello.conebuild").read_text()
        self.assertRegex(hello_desc, r'\nimport libc: ".*/libc\.cone"\nimport core: ".*/core\.cone"\n')

        # libc built on its own needs no core at all: it is the whole build.
        # Asked of the build order, so nothing is written into the repository
        registry = congo.Registry([congo.REPO_PACKAGES])
        libc = registry.find("libc")
        self.assertEqual([u.pkg.name for u in congo.build_order(libc, registry)], ["libc"])
        core = registry.find("core")
        self.assertEqual([u.pkg.name for u in congo.build_order(core, registry)],
                         ["libc", "core"])

    @unittest.skipUnless(IS_WINDOWS, "libc and posix bind the Windows C runtime")
    def test_the_os_layer_sample(self):
        # samples/oslayer, copied here and run: libc and posix from the
        # repository's packages, posix importing libc. Each step prints 'ok' when
        # what it checked held; the program exits with the count that did not
        sample = self.root / "oslayer"
        shutil.copytree(congo.REPO / "samples" / "oslayer", sample,
                        ignore=shutil.ignore_patterns("build"))
        run = self.congo("run", cwd=sample)
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["libc", "core", "stdio", "posix", "oslayer"])
        self.assertEqual(self.program_output(run), textwrap.dedent("""\
            memory
              ok    100 squares kept through 6 reallocs sum to 328350
              ok    calloc gives zeroed memory
            files
              ok    wrote 23 bytes to notes.txt
              ok    read the same 23 bytes back
              ok    stat: notes.txt exists, a regular file of 23 bytes
              ok    stat: missing.txt does not exist
            directories
              ok    mkdir made sub
              ok    chdir went into sub
              ok    getcwd there is getcwd here with \\sub after it
              ok    chdir came back out
              ok    realpath of sub/../notes.txt is here with \\notes.txt after it
              ok    listing found two entries: notes.txt (23 bytes) and sub (a directory)
            processes and the environment
              ok    setenv set OSLAYER_GREETING
              ok    getenv reads it back
              ok    popen captured what the child printed, the variable included
                    the child said: hello from a child
              ok    system returns the command's exit status
            cleaning up
              ok    removed notes.txt
              ok    removed sub
              ok    left and removed oslayer-tour
            """))
        # posix's include file imports libc, answered by the program's package
        # line for libc; the program imports both
        out = sample / "build" / "debug"
        self.assertIn("import libc;", (out / "posix.cone").read_text())
        self.assertRegex((out / "posix.conebuild").read_text(),
                         r'\nimport libc: ".*/libc\.cone"\nimport core: ".*/core\.cone"\n')

    @unittest.skipUnless(IS_WINDOWS, "libc binds the Windows C runtime")
    def test_the_math3d_example(self):
        # packages/math3d/examples/tour.cone, run where it stands as a lone
        # file: math3d from the registry, compiled alone as a library over libc,
        # and the example linked against its object. Its build is in the home,
        # so nothing is written into the repository
        example = congo.REPO_PACKAGES / "math3d" / "examples" / "tour.cone"
        run = self.congo("run", str(example), cwd=self.root)
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["libc", "core", "stdio", "math3d", "tour"])
        self.assertEqual(self.program_output(run), textwrap.dedent("""\
            perspective, 60 degrees, 16:9:
                 0.9743   0.0000   0.0000   0.0000
                 0.0000   1.7321   0.0000   0.0000
                 0.0000   0.0000  -1.0020  -0.2002
                 0.0000   0.0000  -1.0000   0.0000
            view from (0, 2, 5) looking at the origin:
                 1.0000   0.0000   0.0000   0.0000
                 0.0000   0.9285  -0.3714   0.0000
                 0.0000   0.3714   0.9285  -5.3852
                 0.0000   0.0000   0.0000   1.0000
            the origin, seen by the camera: (0.0000, 0.0000, -5.3852)
            its distance from the eye: 5.3852

            a quarter turn about z: (0.0000, 0.0000, 0.7071, 0.7071)
            x turned by it: (0.0000, 1.0000, 0.0000)
            halfway there, by slerp: (0.0000, 0.0000, 0.3827, 0.9239)
            an eighth turn, made directly: (0.0000, 0.0000, 0.3827, 0.9239)
            x turned by the halfway orientation: (0.7071, 0.7071, 0.0000)

            placed at (1, 2, 3), turned, scaled by (2, 3, 4):
                 0.0000  -3.0000   0.0000   1.0000
                 2.0000   0.0000   0.0000   2.0000
                 0.0000   0.0000   4.0000   3.0000
                 0.0000   0.0000   0.0000   1.0000
            its corner (1, 1, 1), placed: (-2.0000, 4.0000, 7.0000)
            and brought back by the inverse: (1.0000, 1.0000, 1.0000)
            the inverse times the placement:
                 1.0000   0.0000   0.0000   0.0000
                 0.0000   1.0000   0.0000   0.0000
                 0.0000   0.0000   1.0000   0.0000
                 0.0000   0.0000   0.0000   1.0000

            orange: 1.0000 0.5000 0.0000 alpha 1.0000
            screen: 1280 by 720
            """))
        # math3d's include file imports libc, and the example's description
        # finds math3d at its own compile's output
        out = next((self.root / "home" / "lone").glob("tour-*")) / "debug"
        self.assertIn("import libc;", (out / "math3d.cone").read_text())
        self.assertRegex((out / "tour.conebuild").read_text(),
                         r'\nimport math3d: ".*/math3d\.cone"\n')
        self.assertFalse((example.parent / "build").exists())

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

    def test_a_keyword_cannot_name_a_module(self):
        # Wherever Congo takes a module's name from a file or a folder, a word
        # the compiler never reads as a name is refused, naming the file, before
        # a build description is written that the compiler could not read
        main = "fn main() i32 {\n  0i32;\n}\n"
        write(self.root / "use.cone", main)
        run = self.congo("run", "use.cone", cwd=self.root, ok=False)
        self.assertIn("use.cone: with no 'mod' line it is named by its file: 'use' is a"
                      " Cone keyword", run.stderr)
        self.assertIn("give the file a 'mod' line", run.stderr)
        # The language refuses the word on a 'mod' line too, and so does Congo
        write(self.root / "use.cone", "mod use;\n\n" + main)
        run = self.congo("run", "use.cone", cwd=self.root, ok=False)
        self.assertIn("use.cone: 'mod use': 'use' is a Cone keyword", run.stderr)
        self.assertNotIn("Error 1129", run.stdout + run.stderr)
        # Named on its 'mod' line, the file's own name does not matter
        write(self.root / "use.cone", "mod usecheck;\n\n" + main)
        self.congo("run", "use.cone", cwd=self.root)

        self.congo("new", "shelf", "--lib", cwd=self.root)
        pkg = self.root / "shelf"
        write(pkg / "src" / "mut.cone", "mod mut;\n")
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertIn("mut.cone: a one-file module is named by its file: 'mut' is a Cone"
                      " permission", run.stderr)
        (pkg / "src" / "mut.cone").unlink()
        write(pkg / "src" / "yield" / "yield.cone", "mod yield;\n")
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertIn("yield.cone: a module folder names its module: 'yield' is reserved",
                      run.stderr)
        self.assertFalse((pkg / "build" / "debug" / "shelf.conebuild").exists())
        shutil.rmtree(pkg / "src" / "yield")
        write(pkg / "congo.toml", '[package]\nname = "match"\nversion = "0.1.0"\n'
                                  'output = "library"\n')
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertIn("[package] name 'match' is a Cone keyword", run.stderr)
        run = self.congo("new", "if", cwd=self.root, ok=False)
        self.assertIn("'if' is a Cone keyword", run.stderr)
        self.assertFalse((self.root / "if").exists())


class Testing(unittest.TestCase):
    """congo test: a package's tests/ programs built against its generated
    include file, run, and compared; its examples/ programs built."""

    setUp = Scenarios.setUp
    tearDown = Scenarios.tearDown
    congo = Scenarios.congo

    def counter(self, root: Path) -> Path:
        """A library in no registry, with one function; a test of it that
        passes, one whose expected output is wrong, and one that ends with the
        status its .exit file names; an example that builds and one that does
        not."""
        pkg = root / "counter"
        write(pkg / "congo.toml",
              '[package]\nname = "counter"\nversion = "0.1.0"\noutput = "library"\n')
        write(pkg / "src" / "counter.cone", """
            mod counter;

            pub fn twice(n i64) i64 {
              n * 2i64;
            }
            """)
        test = """
            mod {name};

            import stdio use *;
            import counter;

            fn main() i32 {{
              printInt(counter.twice({n}i64));
              printStr("\\n");
              {status};
            }}
            """
        write(pkg / "tests" / "doubles.cone", test.format(name="doubles", n=21, status="0i32"))
        write(pkg / "tests" / "doubles.out", "42\n")
        write(pkg / "tests" / "wrong.cone", test.format(name="wrong", n=2, status="0i32"))
        write(pkg / "tests" / "wrong.out", "5\n")
        # twice(1) + 1 is 3, the status main returns
        write(pkg / "tests" / "status.cone",
              test.format(name="status", n=4, status="i32[counter.twice(1i64) + 1i64]"))
        write(pkg / "tests" / "status.out", "8\n")
        write(pkg / "tests" / "status.exit", "3\n")
        write(pkg / "examples" / "show.cone", test.format(name="show", n=5, status="0i32"))
        write(pkg / "examples" / "broken.cone", """
            mod broken;

            import counter;

            fn main() i32 {
              counter.thrice(1i64);
              0i32;
            }
            """)
        return pkg

    def test_tests_run_against_the_include_file(self):
        pkg = self.counter(self.root)
        run = self.congo("test", cwd=pkg, ok=False)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        out = run.stdout
        self.assertIn("test doubles ... ok", out)
        self.assertIn("test status ... ok", out)
        self.assertIn("test wrong ... FAILED", out)
        # The diff of the wrong one: 5 expected, twice(2) printed
        self.assertIn("output differs:", out)
        self.assertRegex(out, r"\n\s*-5\n\s*\+4\n")
        self.assertIn("example show ... built", out)
        self.assertIn("example broken ... FAILED to build", out)
        self.assertIn("counter: 3 tests: 2 passed, 1 failed; 2 examples: 1 built,"
                      " 1 failed to build", out)
        self.assertIn("counter: test wrong", run.stderr)
        self.assertIn("counter: example broken", run.stderr)
        # counter is in no registry: the tests found it as the package under
        # test, compiled on its own as a library, and each was compiled against
        # the include file that compile generated, then linked with its object
        build = pkg / "build" / "debug"
        self.assertIn("pub extern fn twice(n i64) i64;", (build / "counter.cone").read_text())
        desc = (build / "tests" / "doubles" / "doubles.conebuild").read_text()
        self.assertRegex(desc, r'import counter: ".*/build/debug/counter\.cone"')
        self.assertTrue((build / "tests" / "doubles" / f"doubles{congo.EXE_EXT}").is_file())
        self.assertFalse((build / "examples" / "broken" / f"broken{congo.EXE_EXT}").exists())

    def test_a_filter_and_an_exit_status(self):
        pkg = self.counter(self.root)
        run = self.congo("test", "doubles", cwd=pkg)
        self.assertIn("test doubles ... ok", run.stdout)
        self.assertNotIn("wrong", run.stdout)
        self.assertNotIn("example show", run.stdout)
        self.assertIn("1 test: 1 passed, 0 failed; 0 examples", run.stdout)
        # Without its .exit file, status's 3 is a failure
        (pkg / "tests" / "status.exit").unlink()
        run = self.congo("test", "status", cwd=pkg, ok=False)
        self.assertIn("test status ... FAILED", run.stdout)
        self.assertIn("exited 3, expected 0", run.stdout)
        self.assertNotIn("output differs", run.stdout)
        run = self.congo("test", "nosuch", cwd=pkg, ok=False)
        self.assertIn("no test or example has 'nosuch' in its name", run.stderr)

    def test_a_test_named_for_a_keyword(self):
        # Its 'mod' line names it for its file, as a test's does, and the word
        # is a keyword: Congo says so, naming the file, and writes no build
        # description the compiler could not read
        pkg = self.counter(self.root)
        write(pkg / "tests" / "use.cone", "mod use;\n\nfn main() i32 {\n  0i32;\n}\n")
        write(pkg / "tests" / "use.out", "\n")
        run = self.congo("test", "use", cwd=pkg, ok=False)
        self.assertIn("test use ... FAILED", run.stdout)
        self.assertIn("use.cone: 'mod use': 'use' is a Cone keyword, and a keyword cannot"
                      " name a package or a module; rename the module", run.stdout)
        self.assertNotIn("Error 1129", run.stdout)
        self.assertFalse((pkg / "build" / "debug" / "tests" / "use" / "use.conebuild").exists())

    def test_bless_writes_only_what_is_missing(self):
        pkg = self.counter(self.root)
        (pkg / "tests" / "doubles.out").unlink()
        run = self.congo("test", "doubles", cwd=pkg, ok=False)
        self.assertIn("no expected output", run.stdout)
        run = self.congo("test", "--bless", cwd=pkg, ok=False)
        self.assertIn("blessed", run.stdout)
        self.assertIn("check it by hand against the source", run.stdout)
        self.assertEqual((pkg / "tests" / "doubles.out").read_bytes(), b"42\n")
        # An expected file already there is compared, never rewritten
        self.assertIn("test wrong ... FAILED", run.stdout)
        self.assertEqual((pkg / "tests" / "wrong.out").read_text(), "5\n")
        self.congo("test", "doubles", cwd=pkg)

    def test_no_tests_and_a_folder_of_packages(self):
        shelf = self.root / "shelf"
        self.congo("new", "plain", str(shelf / "plain"), "--lib", cwd=self.root)
        # congo new makes tests/ empty; with it gone the package says it has none
        (shelf / "plain" / "tests").rmdir()
        run = self.congo("test", cwd=shelf / "plain")
        self.assertIn("none: plain has no tests folder", run.stdout)
        self.assertIn("plain: 0 tests: 0 passed, 0 failed; 0 examples", run.stdout)
        # At a folder of packages, each package's tests run
        pkg = self.counter(shelf)
        for name in ("wrong", "status"):
            (pkg / "tests" / f"{name}.cone").unlink()
        (pkg / "examples" / "broken.cone").unlink()
        run = self.congo("test", cwd=shelf)
        self.assertIn("Testing counter", run.stdout)
        self.assertIn("Testing plain", run.stdout)
        self.assertIn("2 packages: 1 test: 1 passed, 0 failed; 1 example: 1 built,"
                      " 0 failed to build", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
