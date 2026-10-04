#!/usr/bin/env python3
"""Congo's own checks: python tools/congo/test_congo.py

The header scan is checked on its own. Everything else runs congo.py as a user
would, in a fresh temporary folder with its own Congo home, against the
repository's conec (build it first: python test/run.py --build) and its
packages/. Every expected output below was worked out by hand from the source
it prints from, not copied from a run.
"""

from __future__ import annotations

import hashlib
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

# CONGO_EXE names a Congo executable to drive in place of congo.py: the Cone
# Congo, tools/congo/build/debug/congo.exe (README.md, "Congo in Cone"). Every
# scenario runs against either. The checks of congo.py's own functions run
# either way.
CONGO_EXE = os.environ.get("CONGO_EXE")
CONGO = [CONGO_EXE] if CONGO_EXE else [sys.executable, str(HERE / "congo.py")]
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
            import libc as clib use EDOM;
            fn main() {}
            import late;
            """)
        self.assertEqual(header.mod, "prog")
        self.assertEqual([i.written for i in header.imports],
                         ["stdio", "geometry", '"../q/q"', "libc"])
        self.assertEqual([i.line for i in header.imports], [5, 6, 7, 8])

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
        # The Cone Congo keeps the same three lists, in src/header.cone
        header = (HERE / "src" / "header.cone").read_text(encoding="utf-8")
        lists = {name: set(words.split()) for name, words in
                 re.findall(r'fn (is\w+)\(w &\[\]u8\) Bool \{\s*inWords\(w, "([^"]*)"\)', header)}
        self.assertEqual(lists, {"isKeyword": congo.KEYWORDS, "isReserved": congo.RESERVED,
                                 "isPermission": congo.PERMISSIONS})
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

    def test_an_import_renamed_with_as(self):
        # 'import x as y' names the package x, which is what Congo reads: the
        # rename and the clause after it are the compiler's. A sister imported
        # under another name is found as she is under her own
        self.congo("new", "renamed", cwd=self.root)
        pkg = self.root / "renamed"
        write(pkg / "src" / "renamed.cone", """
            mod renamed;

            import stdio as io use printStr;

            fn main() i32 {
              printStr("sum = ");
              io.printInt(twice.of(21i64));
              printStr("\\n");
              0i32;
            }
            """)
        write(pkg / "src" / "twice" / "twice.cone", """
            mod twice;

            import helper as h;

            pub fn of(n i64) i64 {
              h.add(n, n);
            }
            """)
        write(pkg / "src" / "helper" / "helper.cone", """
            mod helper;

            pub fn add(a i64, b i64) i64 {
              a + b;
            }
            """)
        run = self.congo("run", cwd=pkg)
        self.assertEqual(self.program_output(run), "sum = 42\n")

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
        # Nothing is written beside the file: a lone file's build is in the
        # home, in a folder named for the file and the first ten hex digits of
        # the SHA-1 of its absolute path, lower-cased on Windows
        self.assertFalse((self.root / "build").exists())
        spelled = str((self.root / "lone.cone").resolve())
        digest = hashlib.sha1((spelled.lower() if IS_WINDOWS else spelled).encode()).hexdigest()
        self.assertTrue((self.root / "home" / "lone" / f"lone-{digest[:10]}" / "debug"
                         / f"lone{congo.EXE_EXT}").is_file())
        self.congo("clean", "lone.cone", cwd=self.root)
        self.assertEqual(list((self.root / "home" / "lone").iterdir()), [])

    # TEMPORARY, a provisional mechanism whose final design is open
    def test_define_reaches_the_compile(self):
        write(self.root / "flags.cone", """
            mod flags;

            import stdio use *;

            fn main() i32 {
              if isDefined("FAST") {print <- "fast ";} else {print <- "slow ";}
              print <- definedInt("LEVEL");
              print <- "\\n";
              0i32;
            }
            """)
        run = self.congo("run", "-D", "FAST", "-DLEVEL=7", "flags.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "fast 7\n")
        run = self.congo("run", "flags.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "slow 0\n")
        # conec refuses a bad one, and Congo says which compile failed
        failed = self.congo("run", "--define=LEVEL=high", "flags.cone", cwd=self.root, ok=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("-D LEVEL takes an integer", failed.stdout + failed.stderr)

    def test_a_lone_file_under_a_non_ascii_folder(self):
        # A path beyond ASCII, in the folder and the file's name, reaches conec
        # whole: the source is read, and the build description, the object and
        # the executable are written into a build folder named for the file,
        # which conec and the linker are given
        name = "café日本"
        write(self.root / name / f"{name}.cone", """
            mod lone;

            import stdio use *;

            fn main() i32 {
              print <- "lone ";
              print <- 6i64 * 7i64;
              print <- "\\n";
              0i32;
            }
            """)
        run = self.congo("run", f"{name}/{name}.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "lone 42\n")
        spelled = str((self.root / name / f"{name}.cone").resolve())
        digest = hashlib.sha1((spelled.lower() if IS_WINDOWS else spelled).encode()).hexdigest()
        self.assertTrue((self.root / "home" / "lone" / f"{name}-{digest[:10]}" / "debug"
                         / f"lone{congo.EXE_EXT}").is_file())

    def test_a_compile_error_names_a_non_ascii_path(self):
        # conec prints the path in UTF-8, and Congo reads it as UTF-8 and
        # writes it so: the message names the file as it is spelled
        name = "café日本"
        write(self.root / name / "bad.cone", """
            mod bad;

            fn main() i32 {
              nosuch();
              0i32;
            }
            """)
        run = subprocess.run([*CONGO, "run", f"{name}/bad.cone"], cwd=self.root,
                             env=self.env, capture_output=True)
        out = run.stdout.decode("utf-8").replace("\r\n", "\n")
        err = run.stderr.decode("utf-8").replace("\r\n", "\n")
        self.assertEqual(run.returncode, 1, out + err)
        spelled = (self.root / name / "bad.cone").resolve().as_posix()
        self.assertIn("Error 1019: The name nosuch does not refer to a declared name\n", out)
        self.assertIn(f"^--- {spelled}:4:3\n", out)
        self.assertIn("congo: error: could not compile bad", err)

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
              new b.Counter(n, 5i64);
            }

            pub fn advance(c b.Counter) b.Counter {
              new b.Counter(c.next(), c.step);
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
        # start(30) is new Counter(30, 5), whose next() is 30+5 = 35, from b's
        # object. Advanced twice: (35, 5), then (40, 5), built in a's object
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
                new Stack(b, a);
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
              imm s = new coll.Stack(3i64, 4i64).swap();
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
        # new Stack(3, 4) swapped is (4, 3): a is 4; total is 4 + 3 from vec's
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
        self.assertNotRegex((out / "libc.conebuild").read_text(), r"(?m)^import ")
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
    def test_the_geomath_example(self):
        # packages/geomath/examples/tour.cone, run where it stands as a lone
        # file: geomath from the registry, compiled alone as a library over
        # libc (it needs no collections), and the example linked against its
        # object. Its build is in the home, so nothing is written into the
        # repository
        example = congo.REPO_PACKAGES / "geomath" / "examples" / "tour.cone"
        run = self.congo("run", str(example), cwd=self.root)
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["libc", "core", "stdio", "geomath", "tour"])
        self.assertEqual(self.program_output(run), textwrap.dedent("""\
            perspective, 60 degrees, 16:9:
                 0.9743   0.0000   0.0000   0.0000
                 0.0000   1.7321   0.0000   0.0000
                 0.0000   0.0000  -1.0010  -0.1001
                 0.0000   0.0000  -1.0000   0.0000
            view from (0, 2, 5) looking at the origin:
                 1.0000   0.0000   0.0000   0.0000
                 0.0000   0.9285  -0.3714   0.0000
                 0.0000   0.3714   0.9285  -5.3852
                 0.0000   0.0000   0.0000   1.0000
            the origin, seen by the camera: (0.0000, 0.0000, -5.3852)
            its distance from the eye: 5.3852
            a unit box at the origin is inside
            a ball behind the camera is outside

            a quarter turn about z: (0.0000, 0.0000, 0.7071, 0.7071)
            x turned by it: (0.0000, 1.0000, 0.0000)
            halfway there, by slerp: (0.0000, 0.0000, 0.3827, 0.9239)
            an eighth turn, made directly: (0.0000, 0.0000, 0.3827, 0.9239)
            x turned by the halfway orientation: (0.7071, 0.7071, 0.0000)

            placed at (1, 2, 3), turned, scaled by 2:
                 0.0000  -2.0000   0.0000   1.0000
                 2.0000   0.0000   0.0000   2.0000
                 0.0000   0.0000   2.0000   3.0000
                 0.0000   0.0000   0.0000   1.0000
            its corner (1, 1, 1), placed: (-1.0000, 4.0000, 5.0000)
            and brought back by the inverse: (1.0000, 1.0000, 1.0000)

            the ray from the eye meets the box after 4.3081, at (0.0000, 0.4000, 1.0000)
            the arch's middle: (1.0000, 1.5000)
            which way it runs there: (3.0000, 0.0000)

            orange: 1.0000 0.5000 0.0000 alpha 1.0000
            screen: 1280 by 720
            """))
        # geomath's include file imports libc, and the example's description
        # finds geomath at its own compile's output
        out = next((self.root / "home" / "lone").glob("tour-*")) / "debug"
        self.assertIn("import libc;", (out / "geomath.cone").read_text())
        self.assertRegex((out / "tour.conebuild").read_text(),
                         r'\nimport geomath: ".*/geomath\.cone"\n')
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

    def test_a_keyword_cannot_name_a_lone_file(self):
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

    def test_a_keyword_cannot_name_a_module(self):
        # The same, where a package's file or folder names a module
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
              test.format(name="status", n=4, status="i32.from(counter.twice(1i64) + 1i64)"))
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

    def test_a_package_exports_an_actor(self):
        # A library declaring a public actor and a private one: its include file
        # holds each actor whole, the importer generates their declarations
        # again, and its instances of the actors package's generics reach the
        # functions the library's object exports for them. The two actors'
        # lines are ordered by their messages, not by luck: Hidden is handed
        # the Pinger and pings it after printing, so the Pinger's last message,
        # and so its final, comes after Hidden's line
        pkg = self.root / "pinger"
        write(pkg / "congo.toml",
              '[package]\nname = "pinger"\nversion = "0.1.0"\noutput = "library"\n')
        write(pkg / "src" / "pinger.cone", """
            mod pinger;

            import stdio use *;
            import actors;

            pub actor Pinger {
              count u64;

              pub fn init(self &new, start u64) {
                *self = new Self(count: start);
              }

              pub fn ping(self, n u64) {
                count = count + n;
              }

              fn final(self &uni) {
                printStr("pinger "); printUInt(count); printStr("\\n");
              }
            }

            actor Hidden {
              pub fn hello(self, n u64, p Pinger) {
                printStr("hidden "); printUInt(n); printStr("\\n");
                p.ping(n);
              }
            }

            pub fn useHidden(p Pinger) {
              imm h = new Hidden();
              h.hello(3u64, p);
            }
            """)
        write(pkg / "tests" / "useit.cone", """
            mod useit;

            import pinger;
            import actors;

            fn main() {
              initAll();
              {
                imm p = new pinger.Pinger(5u64);
                p.ping(10u64);
                pinger.useHidden(p);
              }
              finalAll();
            }
            """)
        write(pkg / "tests" / "useit.out", "hidden 3\npinger 18\n")
        run = self.congo("test", cwd=pkg)
        self.assertIn("test useit ... ok", run.stdout)
        include = (pkg / "build" / "debug" / "pinger.cone").read_text()
        self.assertIn("pub actor Pinger {", include)
        self.assertIn("actor Hidden {", include)

    def test_a_non_ascii_diff_to_a_pipe(self):
        # Congo's stdout here is a pipe, which Python would write in the
        # locale's code page (1252 on Windows) unless told otherwise: a diff
        # holding a character outside it raised UnicodeEncodeError. Congo
        # writes UTF-8 whatever the locale, so the diff arrives whole
        pkg = self.counter(self.root)
        write(pkg / "tests" / "accent.cone", """
            mod accent;

            import stdio use *;
            import counter;

            fn main() i32 {
              printStr("caf\\u00e9 \\u65e5\\u672c\\n");
              0i32;
            }
            """)
        write(pkg / "tests" / "accent.out", "cafe 日本\n")
        env = dict(self.env)
        for name in ("PYTHONIOENCODING", "PYTHONUTF8"):
            env.pop(name, None)
        run = subprocess.run([*CONGO, "test", "accent"], cwd=pkg, env=env,
                             capture_output=True)
        out = run.stdout.decode("utf-8").replace("\r\n", "\n")
        err = run.stderr.decode("utf-8").replace("\r\n", "\n")
        self.assertEqual(run.returncode, 1, out + err)
        self.assertNotIn("Traceback", err)
        self.assertIn("test accent ... FAILED", out)
        self.assertRegex(out, "\n\\s*-cafe 日本\n\\s*\\+café 日本\n")
        self.assertIn("counter: test accent", err)

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

    def halver(self, root: Path) -> Path:
        """A library whose one function checks its argument in a debug build
        only; a test of it that passes in either build, and one, marked
        debug-only by its .debug file, that breaks the check."""
        pkg = root / "halver"
        write(pkg / "congo.toml",
              '[package]\nname = "halver"\nversion = "0.1.0"\noutput = "library"\n')
        # The check is on line 4 of the file, which the panic names
        write(pkg / "src" / "halver.cone",
              "mod halver;\n"
              "\n"
              "pub fn half(n i64) i64 {\n"
              '  assertDebugMsg(n % 2i64 == 0i64, "only an even number halves");\n'
              "  n / 2i64;\n"
              "}\n")
        test = """
            mod {name};

            import stdio use *;
            import halver;

            fn main() i32 {{
              printInt(halver.half({n}i64));
              printStr("\\n");
              0i32;
            }}
            """
        write(pkg / "tests" / "even.cone", test.format(name="even", n=8))
        write(pkg / "tests" / "even.out", "4\n")
        write(pkg / "tests" / "odd.cone", test.format(name="odd", n=7))
        write(pkg / "tests" / "odd.debug", "")
        return pkg

    def test_a_debug_only_test(self):
        pkg = self.halver(self.root)
        # Released, it is not built or run, and so --bless writes nothing for
        # it: a release build would not panic, and its status is not the
        # test's. The run still succeeds
        run = self.congo("test", "--release", "--bless", cwd=pkg)
        self.assertIn("test even ... ok", run.stdout)
        self.assertIn("test odd ... skipped (debug only)", run.stdout)
        self.assertIn("halver: 2 tests: 1 passed, 0 failed, 1 skipped (debug only);"
                      " 0 examples: 0 built, 0 failed to build", run.stdout)
        for suffix in (".out", ".exit", ".err"):
            self.assertFalse((pkg / "tests" / f"odd{suffix}").exists(), suffix)
        self.assertFalse((pkg / "build" / "release" / "tests" / "odd").exists())
        # In a debug build it runs as any test does: blessed, it panics, so its
        # status is written beside its output
        run = self.congo("test", "--bless", cwd=pkg)
        self.assertIn("test odd ... ok", run.stdout)
        self.assertIn("blessed", run.stdout)
        self.assertNotIn("skipped", run.stdout)
        self.assertIn("halver: 2 tests: 2 passed, 0 failed; 0 examples", run.stdout)
        self.assertNotEqual((pkg / "tests" / "odd.exit").read_text().strip(), "0")
        # With the panic's line expected on stderr, it passes in debug ...
        write(pkg / "tests" / "odd.err",
              "panic at halver.cone:4: only an even number halves\n")
        run = self.congo("test", cwd=pkg)
        self.assertIn("test odd ... ok", run.stdout)
        # ... and a wrong one fails: the test ran
        write(pkg / "tests" / "odd.err", "panic at halver.cone:4: something else\n")
        run = self.congo("test", "odd", cwd=pkg, ok=False)
        self.assertIn("test odd ... FAILED", run.stdout)
        self.assertIn("stderr differs:", run.stdout)
        # Released, the name filter finds it, and skipping it is not a failure
        run = self.congo("test", "odd", "--release", cwd=pkg)
        self.assertIn("test odd ... skipped (debug only)", run.stdout)
        self.assertIn("halver: 1 test: 0 passed, 0 failed, 1 skipped (debug only);",
                      run.stdout)
        self.assertNotIn("no test or example has", run.stderr)

    def test_a_package_of_debug_only_tests(self):
        # Every test debug-only: in a release test, each is skipped and the
        # run succeeds, for the package and for a folder of packages
        shelf = self.root / "shelf"
        pkg = self.halver(shelf)
        (pkg / "tests" / "even.cone").unlink()
        run = self.congo("test", "--release", cwd=pkg)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("test odd ... skipped (debug only)", run.stdout)
        self.assertIn("halver: 1 test: 0 passed, 0 failed, 1 skipped (debug only);"
                      " 0 examples: 0 built, 0 failed to build", run.stdout)
        self.assertNotIn("failed:", run.stderr)
        self.congo("new", "plain", str(shelf / "plain"), "--lib", cwd=self.root)
        run = self.congo("test", "--release", cwd=shelf)
        self.assertIn("2 packages: 1 test: 0 passed, 0 failed, 1 skipped (debug only);"
                      " 0 examples: 0 built, 0 failed to build", run.stdout)

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


@unittest.skipUnless(IS_WINDOWS, "runtime DLLs are copied on Windows only")
class RuntimeLibraries(unittest.TestCase):
    """[link] runtime: the DLLs a program needs beside it, copied there by
    every build that links an executable, and inherited by whatever imports
    the package that names them."""

    setUp = Scenarios.setUp
    tearDown = Scenarios.tearDown
    congo = Scenarios.congo
    program_output = Scenarios.program_output
    registry = Scenarios.registry

    TRI_HEAD = '[package]\nname = "tri"\nversion = "0.1.0"\noutput = "library"\n'

    def tri(self, packages: Path, runtime: str | None) -> Path:
        """A C package over a real DLL built here, tri3.dll, exporting
        'triple', which multiplies by three, with its import library tri3.lib
        beside it in clib/, which only [link] paths names. The folder is on no
        PATH, so a program runs only if tri3.dll is copied beside it."""
        pkg = packages / "tri"
        write(pkg / "csrc" / "triple.c",
              "__declspec(dllexport) int triple(int n) { return 3 * n; }\n")
        (pkg / "clib").mkdir(parents=True)
        env = congo.Linker.vs_environment()
        found = shutil.which("cl", path=env["PATH"])
        subprocess.run([found, "/nologo", "/LD", "triple.c", "/Fe:../clib/tri3.dll"],
                       cwd=pkg / "csrc", env=env, check=True, capture_output=True)
        self.manifest(pkg, runtime)
        write(pkg / "src" / "tri.cone", "mod @c tri;\n\npub extern fn triple(n i32) i32;\n")
        return pkg

    def manifest(self, pkg: Path, runtime: str | None) -> None:
        link = '\n[link]\nlibraries = ["tri3"]\npaths = ["clib"]\n'
        write(pkg / "congo.toml", self.TRI_HEAD + link
              + (f"runtime = {runtime}\n" if runtime is not None else ""))

    def app(self) -> Path:
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
        return self.root / "app.cone"

    def test_a_runtime_dll_is_copied_beside_the_program(self):
        packages = self.root / "cpkgs"
        self.registry(packages)
        pkg = self.tri(packages, None)
        self.app()
        # Without runtime the program links, against tri3.lib, but cannot start:
        # Windows finds no tri3.dll beside it or on PATH
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertNotEqual(run.returncode, 0)
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        self.assertTrue((out / "app.exe").is_file())
        self.assertFalse((out / "tri3.dll").exists())

        # Named in [link] runtime, it is copied beside the program, which runs:
        # 3 * 14 = 42
        self.manifest(pkg, '["tri3"]')
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "42\n")
        source = pkg / "clib" / "tri3.dll"
        copying = f"Copying {source.relative_to(self.root)}\n"
        self.assertIn(copying, run.stdout)
        self.assertEqual((out / "tri3.dll").read_bytes(), source.read_bytes())

        # Copied again only when it has changed: the copy there is left alone
        os.utime(out / "tri3.dll", (1_000_000_000, 1_000_000_000))
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "42\n")
        self.assertNotIn("Copying", run.stdout)
        self.assertEqual((out / "tri3.dll").stat().st_mtime, 1_000_000_000)
        # A byte added past the end of the DLL, where Windows' loader does not
        # look, changes the file: it is copied again
        source.write_bytes(source.read_bytes() + b"\0")
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "42\n")
        self.assertIn(copying, run.stdout)
        self.assertEqual((out / "tri3.dll").read_bytes(), source.read_bytes())

        # One that is nowhere is an error naming it and the package that asked,
        # and the program is not linked: the one an earlier build left is gone
        self.manifest(pkg, '["tri3", "nosuch"]')
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn("congo: error: tri's [link] runtime names nosuch, but nosuch.dll is in"
                      " none of the folders Congo looks in: [link] paths (", run.stderr)
        self.assertIn(str(pkg / "clib"), run.stderr)
        self.assertIn("then the folders LIB lists, then the folders PATH lists", run.stderr)
        self.assertFalse((out / "app.exe").exists())

        # The list holds bare names, and is a list of strings
        self.manifest(pkg, '["tri3.dll"]')
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertIn("[link] runtime \"tri3.dll\" must be the DLL's bare name", run.stderr)
        self.manifest(pkg, '"tri3"')
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertIn("[link] runtime must be a list of strings", run.stderr)
        write(pkg / "congo.toml", self.TRI_HEAD + '\n[link]\nruntimes = ["tri3"]\n')
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertIn("'runtimes' is not a [link] key; the keys are libraries, paths and"
                      " runtime", run.stderr)

    def test_an_importer_inherits_the_list(self):
        # A program importing a library that imports tri names nothing of
        # tri's, and still gets tri3.dll beside it, for build and run alike
        packages = self.root / "cpkgs"
        self.registry(packages)
        self.tri(packages, '["tri3"]')
        write(packages / "wrap" / "congo.toml",
              '[package]\nname = "wrap"\nversion = "0.1.0"\noutput = "library"\n')
        write(packages / "wrap" / "src" / "wrap.cone", """
            mod wrap;

            import tri;

            pub fn sextuple(n i32) i32 {
              2i32 * tri.triple(n);
            }
            """)
        self.congo("new", "user", cwd=self.root)
        user = self.root / "user"
        write(user / "src" / "user.cone", """
            mod user;

            import stdio;
            import wrap;

            fn main() i32 {
              stdio.print <- wrap.sextuple(7i32);
              stdio.print <- "\\n";
              0i32;
            }
            """)
        self.congo("build", cwd=user)
        self.assertTrue((user / "build" / "debug" / "tri3.dll").is_file())
        # 2 * 3 * 7 = 42
        run = self.congo("run", cwd=user)
        self.assertEqual(self.program_output(run), "42\n")

    def test_where_a_runtime_dll_is_looked_for(self):
        # [link] paths first, then each folder of the fetched-dependencies
        # folder (CONE_DEPS here), then LIB's folders, then PATH's. Each place
        # holds a different file named fake.dll, which the program never loads
        packages = self.root / "cpkgs"
        self.registry(packages)
        pkg = packages / "fake"
        write(pkg / "congo.toml",
              '[package]\nname = "fake"\nversion = "0.1.0"\noutput = "library"\n'
              '\n[link]\npaths = ["clib"]\nruntime = ["fake"]\n')
        write(pkg / "src" / "fake.cone", "mod fake;\n\npub fn one() i32 {\n  1i32;\n}\n")
        write(self.root / "app.cone", """
            mod app;

            import fake;

            fn main() i32 {
              fake.one() - 1i32;
            }
            """)
        deps = self.root / "deps"
        places = {"paths": pkg / "clib", "deps": deps / "b", "lib": self.root / "libdir",
                  "path": self.root / "pathdir"}
        (deps / "a").mkdir(parents=True)        # searched first, but holds no fake.dll
        for name, folder in places.items():
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "fake.dll").write_bytes(name.encode())
        self.env["CONE_DEPS"] = str(deps)
        self.env["LIB"] = f"{places['lib']};{self.env.get('LIB', '')}"
        self.env["PATH"] = f"{places['path']};{self.env['PATH']}"
        for name in ("paths", "deps", "lib", "path"):
            self.congo("run", "app.cone", cwd=self.root)
            copied = next((self.root / "home" / "lone").glob("app-*")) / "debug" / "fake.dll"
            self.assertEqual(copied.read_bytes(), name.encode(), name)
            (places[name] / "fake.dll").unlink()
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertIn("fake's [link] runtime names fake, but fake.dll is in none", run.stderr)
        self.assertIn(f"then each folder in {deps} (python tools/deps/fetch.py fills it)",
                      run.stderr)

    def test_congo_test_copies_beside_each_test_and_example(self):
        # Each test and example is linked into a folder of its own, and the DLL
        # goes beside each; the test passes only if tri3.dll loads
        pkg = self.tri(self.root, '["tri3"]')
        program = """
            mod {name};

            import stdio;
            import tri;

            fn main() i32 {{
              stdio.print <- tri.triple(5i32);
              stdio.print <- "\\n";
              0i32;
            }}
            """
        write(pkg / "tests" / "triples.cone", program.format(name="triples"))
        write(pkg / "tests" / "triples.out", "15\n")
        write(pkg / "examples" / "show.cone", program.format(name="show"))
        run = self.congo("test", cwd=pkg)
        self.assertIn("test triples ... ok", run.stdout)
        self.assertIn("example show ... built", run.stdout)
        source = (pkg / "clib" / "tri3.dll").read_bytes()
        build = pkg / "build" / "debug"
        for folder in (build / "tests" / "triples", build / "examples" / "show"):
            self.assertEqual((folder / "tri3.dll").read_bytes(), source, folder)
        # The test runs print no Copying line
        self.assertNotIn("Copying", run.stdout)


def entry_points(spv: Path) -> list[str]:
    """The names of a SPIR-V module's GLCompute entry points: each OpEntryPoint
    (opcode 15) whose execution model is GLCompute (5), its name the literal
    string after the entry's id, four bytes a word, ending in a zero."""
    data = spv.read_bytes()
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    assert words[0] == 0x07230203, "SPIR-V's magic number"
    names, at = [], 5
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 0xFFFF
        if op == 15 and words[at + 1] == 5:
            raw = b"".join(w.to_bytes(4, "little") for w in words[at + 3:at + count])
            names.append(raw.split(b"\0")[0].decode())
        at += max(count, 1)
    return names


class GpuKernels(unittest.TestCase):
    """[package] targets = ["native", "gpu"]: a marked package's kernels
    compiled for the GPU into <name>.spv, with what it imports from their
    source, and copied beside every program that imports it; an import not
    marked refused, naming the chain that pulled it in [Jon 3 Oct 2026]."""

    setUp = Scenarios.setUp
    tearDown = Scenarios.tearDown
    congo = Scenarios.congo
    program_output = Scenarios.program_output
    registry = Scenarios.registry

    KERN = """
        mod kern;

        import geomath use *;

        pub fn half(x f32) f32 {
          x * 0.5;
        }

        // A kernel: one invocation for each point
        pub fn @compute(64) scale(inv Invocation, points &[]Vec3, out &[]mut f32) {
          imm i = usize.from(inv.globalId[0usize]);
          if i < out.len and i < points.len {
            out[i] = half(points[i].length());
          }
        }
        """

    PROGRAM = """
        mod {name};

        import stdio;
        import fs;
        import kern;

        fn main() i32 {{
          if kern.half(8.) == 4. {{
            stdio.print <- "half of 8 is 4\\n";
          }}
          if fs.isFile("kern.spv") {{
            stdio.print <- "kern.spv is here\\n";
          }}
          0i32;
        }}
        """

    def package(self, folder: Path, name: str, source: str, gpu: bool = True,
                output: str = "library") -> Path:
        targets = 'targets = ["native", "gpu"]\n' if gpu else ""
        write(folder / "congo.toml", f'[package]\nname = "{name}"\nversion = "0.1.0"\n'
                                     f'output = "{output}"\n{targets}')
        write(folder / "src" / f"{name}.cone", source)
        return folder

    def test_targets_are_checked(self):
        pkg = self.package(self.root / "lib1", "lib1", "mod lib1;\n", gpu=False)
        head = '[package]\nname = "lib1"\nversion = "0.1.0"\noutput = "library"\n'
        for targets, said in (
                ('"gpu"', '[package] targets must be a list of targets, such as'
                          ' ["native", "gpu"]'),
                ('["native", "cuda"]', '[package] targets names "cuda", which is not a target;'
                                       ' the targets are "native" and "gpu"'),
                ('["gpu"]', '[package] targets must name "native": a package built for the'
                            ' GPU alone is not built yet'),
                ('[]', '[package] targets must name "native"')):
            write(pkg / "congo.toml", head + f"targets = {targets}\n")
            run = self.congo("build", cwd=pkg, ok=False)
            self.assertIn(said, run.stderr, targets)
        write(pkg / "congo.toml", head + 'colour = "red"\n')
        self.assertIn("'colour' is not a [package] key; the keys are name, version, output and"
                      " targets", self.congo("build", cwd=pkg, ok=False).stderr)
        # A program has no kernels to build: they go in a library it imports
        write(pkg / "congo.toml", '[package]\nname = "lib1"\nversion = "0.1.0"\n'
                                  'output = "executable"\ntargets = ["native", "gpu"]\n')
        self.assertIn('[package] targets names "gpu", and an executable is not built for the'
                      ' GPU: put its kernels in its gpu/ folder, or in a library package it imports',
                      self.congo("build", cwd=pkg, ok=False).stderr)
        # ["native"] is what a manifest without the key says: no GPU build
        write(pkg / "congo.toml", head + 'targets = ["native"]\n')
        run = self.congo("build", cwd=pkg)
        self.assertNotIn("for the GPU", run.stdout)

    def test_kernels_are_built_and_copied_beside_each_program(self):
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        kern = self.package(pkgs / "kern", "kern", self.KERN)
        run = self.congo("build", cwd=kern)
        self.assertIn(f"Compiling kern v0.1.0 for the GPU ({kern})", run.stdout)
        out = kern / "build" / "debug"
        self.assertIn(f"Finished debug library object {Path('build/debug/kern.obj')} and"
                      f" kernels {Path('build/debug/kern.spv')}", run.stdout)
        # One module, its kernel named as its function is; geomath, marked
        # but with no kernels of its own, has none built
        self.assertEqual(entry_points(out / "kern.spv"), ["scale"])
        self.assertFalse((out / "geomath.spv").exists())
        self.assertNotIn("geomath v0.1.0 for the GPU", run.stdout)
        # Compiled from a module that imports the package, so that conec finds
        # it and geomath on its search path and compiles both from source
        module = (out / "gpu" / "kern" / "kern_gpu.cone").read_text()
        self.assertIn("mod kern_gpu;\n\nimport kern;\n", module)
        # The release build's, in its own folder
        self.congo("build", "--release", cwd=kern)
        self.assertEqual(entry_points(kern / "build" / "release" / "kern.spv"), ["scale"])

        # A program that imports kern gets kern.spv beside it, where it runs:
        # a lone file's, a package's, and each test's and example's
        write(self.root / "app.cone", self.PROGRAM.format(name="app"))
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertIn("Compiling kern v0.1.0 for the GPU", run.stdout)
        self.assertEqual(self.program_output(run), "half of 8 is 4\n")
        lone = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        self.assertEqual(entry_points(lone / "kern.spv"), ["scale"])
        self.assertTrue((lone / "app.exe").is_file())

        self.congo("new", "user", cwd=self.root)
        user = self.root / "user"
        write(user / "src" / "user.cone", self.PROGRAM.format(name="user"))
        run = self.congo("run", cwd=user)
        self.assertTrue((user / "build" / "debug" / "kern.spv").is_file())
        self.assertNotIn("Copying", run.stdout)

        write(kern / "tests" / "beside.cone", self.PROGRAM.format(name="beside"))
        write(kern / "tests" / "beside.out", "half of 8 is 4\nkern.spv is here\n")
        write(kern / "examples" / "show.cone", self.PROGRAM.format(name="show"))
        run = self.congo("test", cwd=kern)
        self.assertIn("test beside ... ok", run.stdout)
        self.assertIn("example show ... built", run.stdout)
        self.assertEqual(entry_points(out / "examples" / "show" / "kern.spv"), ["scale"])
        self.assertNotIn("Copying", run.stdout)
        # Its kernels validated, counted as a test, where spirv-val is on PATH;
        # only built, and said so, where it is not
        if shutil.which("spirv-val", path=self.env["PATH"]):
            self.assertIn("     kernels kern.spv ... valid\n", run.stdout)
            self.assertIn("kern: 2 tests: 2 passed, 0 failed; 1 example: 1 built", run.stdout)
            folder = Path(shutil.which("spirv-val", path=self.env["PATH"])).parent
            self.env["PATH"] = os.pathsep.join(
                p for p in self.env["PATH"].split(os.pathsep)
                if p and os.path.normcase(os.path.abspath(p)) != os.path.normcase(str(folder)))
            run = self.congo("test", cwd=kern)
        self.assertIn("     kernels kern.spv ... built, not validated: spirv-val, the Vulkan"
                      " SDK's, is not on PATH\n", run.stdout)
        self.assertIn("kern: 1 test: 1 passed, 0 failed; 1 example: 1 built", run.stdout)
        # A filter runs only the tests it names, and not the validation
        run = self.congo("test", "beside", cwd=kern)
        self.assertNotIn("kernels kern.spv", run.stdout)

    def test_kernels_are_found_in_source_only(self):
        # 'fn @compute' in a comment or a string is no kernel; and a package
        # not marked has none built, whatever its source holds
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        quiet = self.package(pkgs / "quiet", "quiet", """
            // fn @compute(64) scale(inv Invocation, out &[]mut f32) is not here
            mod quiet;

            pub fn said() &[]u8 {
              "fn @compute(64)";
            }
            """)
        run = self.congo("build", cwd=quiet)
        self.assertNotIn("for the GPU", run.stdout)
        self.assertFalse((quiet / "build" / "debug" / "quiet.spv").exists())
        plain = self.package(pkgs / "kern", "kern", self.KERN, gpu=False)
        run = self.congo("build", cwd=plain)
        self.assertNotIn("for the GPU", run.stdout)
        self.assertFalse((plain / "build" / "debug" / "kern.spv").exists())

    def test_an_import_not_marked_is_refused(self):
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        self.package(pkgs / "plain", "plain", "mod plain;\n\npub fn one() i32 {\n  1i32;\n}\n",
                     gpu=False)
        self.package(pkgs / "loose", "loose", "mod loose;\n\npub fn two() i32 {\n  2i32;\n}\n",
                     gpu=False)
        mid = self.package(pkgs / "mid", "mid",
                           "mod mid;\n\nimport plain;\n\npub fn three() i32 {\n"
                           "  plain.one() + 2i32;\n}\n")
        kern = self.package(pkgs / "kern", "kern", """
            mod kern;

            import mid;
            import loose;

            pub fn @compute(64) fill(inv Invocation, out &[]mut i32) {
              imm i = usize.from(inv.globalId[0usize]);
              if i < out.len {
                out[i] = mid.three() + loose.two();
              }
            }
            """)
        run = self.congo("build", cwd=kern, ok=False)
        self.assertEqual(run.returncode, 1)
        where = Path("src") / "kern.cone"
        self.assertIn(
            "congo: error: kern is compiled for the GPU, and so is every package it imports,"
            " each of which must be marked for the GPU with targets = [\"native\", \"gpu\"] in"
            " its congo.toml; these are not:\n"
            f"    plain: kern imports mid at {where}:3; mid imports plain at"
            f" {mid / 'src' / 'mid.cone'}:3\n"
            f"    loose: kern imports loose at {where}:4\n", run.stderr)
        # Refused before anything is compiled
        self.assertNotIn("Compiling", run.stdout)
        self.assertFalse(list((kern / "build").rglob(f"*{congo.OBJ_EXT}")))
        # A marked package being built is refused the same way, kernels or none
        run = self.congo("build", cwd=mid, ok=False)
        self.assertIn("mid is compiled for the GPU, and so is every package it imports",
                      run.stderr)
        self.assertIn(f"    plain: mid imports plain at {Path('src') / 'mid.cone'}:3",
                      run.stderr)
        # A program that imports mid builds nothing for the GPU, and so is not
        write(self.root / "app.cone", "mod app;\n\nimport mid;\n\nfn main() i32 {\n"
                                      "  mid.three() - 3i32;\n}\n")
        self.congo("run", "app.cone", cwd=self.root)

    def test_a_package_compiled_for_the_gpu_is_in_a_folder_named_for_it(self):
        # conec finds a package on its search path by its folder's name
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        kern = self.package(pkgs / "kernels", "kern", self.KERN)
        run = self.congo("build", cwd=kern, ok=False)
        self.assertIn(f"congo: error: kern's GPU build compiles kern from its source, which"
                      f" conec finds by its name on its package search path ({pkgs},"
                      f" {congo.REPO_PACKAGES}): there it finds no kern, not"
                      f" {kern / 'src' / 'kern.cone'}; it looks for a package in a folder"
                      f" named for it, and kern is in {kern}", run.stderr)


def sdl3_on_lib() -> bool:
    """Whether SDL3's import library is in a folder LIB lists, as a program
    over gpu needs to link (and its DLL, beside it, to run)."""
    return any((Path(folder) / "SDL3.lib").is_file()
               for folder in os.environ.get("LIB", "").split(os.pathsep) if folder)


class GpuFolder(unittest.TestCase):
    """A package's gpu/ folder, beside src/ [Jon 3 Oct 2026]: its modules,
    submodules of the package's root, compiled for the CPU as part of the
    package, and for the GPU without src/ into the package's one .spv; the
    folder checked against the GPU's rules as a marked package is, and gpu/
    refused any use of src/."""

    setUp = Scenarios.setUp
    tearDown = Scenarios.tearDown
    congo = Scenarios.congo
    program_output = Scenarios.program_output
    registry = Scenarios.registry

    # A module folder of gpu/: maths, with a file joining it
    MATHS = """
        mod maths;

        import geomath use *;

        pub fn half(x f32) f32 {
          x * 0.5;
        }
        """
    MATHS_MORE = """
        pub fn len(p Vec3) f32 {
          p.length();
        }
        """
    # A module of one file in gpu/, importing its sister there, maths
    KERN = """
        mod kern;

        import geomath use Vec3;
        import maths;

        // A kernel: one invocation for each point
        pub fn @compute(64) scale(inv Invocation, points &[]Vec3, out &[]mut f32) {
          imm i = usize.from(inv.globalId[0usize]);
          if i < out.len and i < points.len {
            out[i] = maths.half(maths.len(points[i]));
          }
        }
        """

    def package(self, folder: Path, name: str, source: str, output: str = "library",
                targets: str = "") -> Path:
        write(folder / "congo.toml", f'[package]\nname = "{name}"\nversion = "0.1.0"\n'
                                     f'output = "{output}"\n{targets}')
        write(folder / "src" / f"{name}.cone", source)
        return folder

    def gpu_modules(self, pkg: Path) -> None:
        write(pkg / "gpu" / "maths" / "maths.cone", self.MATHS)
        write(pkg / "gpu" / "maths" / "more.cone", self.MATHS_MORE)
        write(pkg / "gpu" / "kern.cone", self.KERN)

    def test_a_library_with_a_gpu_folder(self):
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        shapes = self.package(pkgs / "shapes", "shapes", """
            mod shapes;

            // gpu/'s modules are the root's submodules: reached by name, folded
            // by use, re-exported by pub use
            pub use maths half;

            pub fn quarter(x f32) f32 {
              maths.half(user.halved(x));
            }
            """)
        # A module of src/ may import one of gpu/, its sister
        write(shapes / "src" / "user.cone", """
            mod user;

            import maths;

            pub fn halved(x f32) f32 {
              maths.half(x);
            }
            """)
        self.gpu_modules(shapes)
        run = self.congo("build", cwd=shapes)
        self.assertIn(f"Compiling shapes v0.1.0 for the GPU ({shapes / 'gpu'})", run.stdout)
        out = shapes / "build" / "debug"
        self.assertIn(f"Finished debug library object {Path('build/debug/shapes.obj')} and"
                      f" kernels {Path('build/debug/shapes.spv')}", run.stdout)
        self.assertEqual(entry_points(out / "shapes.spv"), ["scale"])
        # For the CPU, gpu/'s modules are in the package's one description,
        # beside src/'s
        desc = (out / "shapes.conebuild").read_text()
        self.assertRegex(desc, r'\n    kern: \{\n        ".*/shapes/gpu/kern\.cone"\n')
        self.assertRegex(desc, r'\n    maths: \{\n        ".*/shapes/gpu/maths/maths\.cone"\n'
                               r'        ".*/shapes/gpu/maths/more\.cone"\n'
                               r'        import geomath: ')
        self.assertRegex(desc, r'\n    user: \{\n        ".*/shapes/src/user\.cone"\n')
        # For the GPU, a module importing each of gpu/'s, maths before kern,
        # which imports it, found where gpu/ leads the search path
        module = (out / "gpu" / "shapes" / "shapes_gpu.cone").read_text()
        self.assertIn("// The GPU build of shapes's gpu/ folder, written by Congo for conec;"
                      " rewritten\n", module)
        self.assertTrue(module.endswith("\nmod shapes_gpu;\n\nimport maths;\nimport kern;\n"),
                        module)
        self.congo("build", "--release", cwd=shapes)
        self.assertEqual(entry_points(shapes / "build" / "release" / "shapes.spv"), ["scale"])

        # A program that imports it calls gpu/'s functions through src/, and
        # gets shapes.spv beside it
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import shapes;

            fn main() i32 {
              if shapes.quarter(8.) == 2. and shapes.half(8.) == 4. {
                stdio.print <- "a quarter of 8 is 2\\n";
              }
              0i32;
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "a quarter of 8 is 2\n")
        lone = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        self.assertEqual(entry_points(lone / "shapes.spv"), ["scale"])

    def test_a_program_with_a_gpu_folder(self):
        # A program needs no targets to have a gpu/ folder, and its main never
        # reaches the GPU build: only gpu/ is compiled for the GPU
        self.congo("new", "app", cwd=self.root)
        app = self.root / "app"
        write(app / "src" / "app.cone", """
            mod app;

            import stdio;

            fn main() i32 {
              if maths.half(8.) == 4. {
                stdio.print <- "half of 8 is 4\\n";
              }
              0i32;
            }
            """)
        self.gpu_modules(app)
        run = self.congo("run", cwd=app)
        self.assertIn(f"Compiling app v0.1.0 for the GPU ({app / 'gpu'})", run.stdout)
        self.assertEqual(self.program_output(run), "half of 8 is 4\n")
        self.assertEqual(entry_points(app / "build" / "debug" / "app.spv"), ["scale"])
        self.assertNotIn("import app;", (app / "build" / "debug" / "gpu" / "app" /
                                         "app_gpu.cone").read_text())
        # congo test validates its kernels, as a library's
        run = self.congo("test", cwd=app)
        self.assertIn("     kernels app.spv ... ", run.stdout)

    def test_gpu_may_not_use_src(self):
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        lib = self.package(pkgs / "lib", "lib", """
            mod lib;

            pub fn answer() i32 {
              42i32;
            }
            """)
        write(lib / "src" / "helpers.cone", "mod helpers;\n\npub fn one() i32 {\n  1i32;\n}\n")
        tail = (", and gpu/ may not use src/: gpu/ is compiled for the GPU without src/. Move"
                " what both need into gpu/, which src/ may use")
        kern = Path("gpu") / "kern.cone"
        helpers = Path("src") / "helpers.cone"
        for source, said in (
                ("mod kern;\n\nimport helpers;\n",
                 f"{kern}:3: import helpers: helpers is a module of lib's src/ ({helpers})"),
                ("mod kern;\n\nimport answer;\n",
                 f"{kern}:3: import answer: answer is no package, so it would be a name of"
                 f" lib's root module, in src/"),
                ("mod kern extends helpers;\n",
                 f"{kern}:1: extends helpers: helpers is a module of lib's src/ ({helpers})")):
            write(lib / "gpu" / "kern.cone", source)
            run = self.congo("build", cwd=lib, ok=False)
            self.assertEqual(run.returncode, 1)
            self.assertIn(f"congo: error: {said}{tail}\n", run.stderr)
            self.assertNotIn("Compiling", run.stdout)

    def test_what_gpu_may_not_hold_is_refused(self):
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        # src/ may print; gpu/ may not, since stdio is not marked for the GPU
        lib = self.package(pkgs / "lib", "lib", "mod lib;\n\nimport stdio;\n")
        write(lib / "gpu" / "kern.cone", """
            mod kern;

            import geomath;
            import stdio;

            pub fn @compute(64) fill(inv Invocation, out &[]mut f32) {
              imm i = usize.from(inv.globalId[0usize]);
              if i < out.len {
                out[i] = 1.;
              }
            }
            """)
        run = self.congo("build", cwd=lib, ok=False)
        self.assertIn(
            "congo: error: lib's gpu/ folder is compiled for the GPU, and so is every package it"
            " imports, each of which must be marked for the GPU with targets = [\"native\","
            " \"gpu\"] in its congo.toml; these are not:\n"
            f"    stdio: lib imports stdio at {Path('gpu') / 'kern.cone'}:4\n", run.stderr)
        self.assertNotIn("Compiling", run.stdout)
        # What a GPU cannot do is the compiler's to refuse, anywhere in gpu/,
        # called by a kernel or not: here a function calling itself
        write(lib / "gpu" / "kern.cone", """
            mod kern;

            pub fn down(n u32) u32 {
              if n == 0u32 {0u32;} else {down(n - 1u32);};
            }

            pub fn @compute(64) fill(inv Invocation, out &[]mut f32) {
              imm i = usize.from(inv.globalId[0usize]);
              if i < out.len {
                out[i] = 1.;
              }
            }
            """)
        run = self.congo("build", cwd=lib, ok=False)
        self.assertIn("Error 1256: On a GPU target a function may not call itself", run.stdout)
        self.assertIn(f"congo: error: could not compile lib for the GPU"
                      f" ({lib / 'build' / 'debug' / 'gpu' / 'lib' / 'lib_gpu.cone'})",
                      run.stderr)

    def test_a_gpu_folder_without_entry_points(self):
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        lib = self.package(pkgs / "lib", "lib", "mod lib;\n\npub use maths half;\n")
        write(lib / "gpu" / "maths" / "maths.cone", self.MATHS)
        run = self.congo("build", cwd=lib)
        self.assertNotIn("for the GPU", run.stdout)
        self.assertFalse((lib / "build" / "debug" / "lib.spv").exists())
        self.assertIn(f"Finished debug library object {Path('build/debug/lib.obj')}\n",
                      run.stdout)
        # Its imports are checked all the same
        write(lib / "gpu" / "maths" / "maths.cone", "mod maths;\n\nimport stdio;\n")
        run = self.congo("build", cwd=lib, ok=False)
        self.assertIn("lib's gpu/ folder is compiled for the GPU", run.stderr)

    def test_the_gpu_folder_is_laid_out_as_src_is(self):
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        lib = self.package(pkgs / "lib", "lib", "mod lib;\n")
        loose = lib / "gpu" / "loose.cone"
        write(loose, "pub fn one() i32 {\n  1i32;\n}\n")
        self.assertIn(f"congo: error: {loose}: a file in gpu/ opens with its own 'mod' line, as"
                      f" a module of one file named for the file: gpu/ holds the package's GPU"
                      f" modules, and a file with no 'mod' line would join none\n",
                      self.congo("build", cwd=lib, ok=False).stderr)
        loose.unlink()
        bare = lib / "gpu" / "bare"
        write(bare / "one.cone", "pub fn one() i32 {\n  1i32;\n}\n")
        self.assertIn(f"congo: error: {bare}: a folder in gpu/ is a module folder, holding its"
                      f" designated file, bare.cone: gpu/ holds the package's GPU modules, and"
                      f" a folder with none would be part of no module\n",
                      self.congo("build", cwd=lib, ok=False).stderr)
        shutil.rmtree(bare)
        write(lib / "src" / "twice.cone", "mod twice;\n")
        write(lib / "gpu" / "twice.cone", "mod twice;\n")
        self.assertIn(f"congo: error: {lib / 'gpu' / 'twice.cone'}: lib has two submodules"
                      f" named 'twice', this one in gpu/ and {lib / 'src' / 'twice.cone'} in"
                      f" src/; rename one\n", self.congo("build", cwd=lib, ok=False).stderr)
        (lib / "src" / "twice.cone").unlink()
        self.congo("build", cwd=lib)
        # gpu/ leads the GPU build's search path, so a module of it may not
        # take the name of a package that build compiles from source
        write(lib / "gpu" / "libc.cone", "mod libc;\n")
        write(lib / "gpu" / "kern.cone", "mod kern;\n\npub fn @compute(64) fill(inv Invocation,"
                                         " out &[]mut f32) {\n}\n")
        self.assertIn(f"congo: error: lib's GPU build compiles libc from its source, which conec"
                      f" finds by its name on its package search path ({lib / 'gpu'},"
                      f" {congo.REPO_PACKAGES}): there it finds {lib / 'gpu' / 'libc.cone'}, not"
                      f" {congo.REPO_PACKAGES / 'libc' / 'src' / 'libc.cone'}\n",
                      self.congo("build", cwd=lib, ok=False).stderr)
        (lib / "gpu" / "libc.cone").unlink()
        self.assertEqual(entry_points(lib / "build" / "debug" / "lib.spv")
                         if self.congo("build", cwd=lib) else [], ["fill"])
        # A package marked for the GPU is compiled for it whole: a gpu/ folder
        # there is refused
        write(lib / "congo.toml", '[package]\nname = "lib"\nversion = "0.1.0"\n'
                                  'output = "library"\ntargets = ["native", "gpu"]\n')
        self.assertIn(f"congo: error: {lib / 'gpu'}: lib is marked for the GPU (targets ="
                      f" [\"native\", \"gpu\"]), so all of it is compiled for the GPU already;"
                      f" a gpu/ folder is for a package that is not: move gpu/'s modules into"
                      f" src/, or take \"gpu\" out of targets\n",
                      self.congo("build", cwd=lib, ok=False).stderr)

    @unittest.skipUnless(IS_WINDOWS and sdl3_on_lib(),
                         "the sample needs SDL3.lib on LIB, a GPU driver with Vulkan 1.3, and"
                         " Windows")
    def test_the_sample_program_with_a_gpu_folder(self):
        # packages/gpupart: a program whose main dispatches gpu/'s kernel
        # through gpuwork and calls the same function on the CPU, every word
        # identical, bit for bit. Run from a copy, so the repository's
        # package folder gets no build/
        sample = self.root / "gpupart"
        shutil.copytree(congo.REPO_PACKAGES / "gpupart", sample,
                        ignore=shutil.ignore_patterns("build"))
        run = self.congo("run", cwd=sample)
        self.assertEqual(self.program_output(run),
                         "12288 values compared, 12288 identical\n14 checks passed\n")
        self.assertEqual(entry_points(sample / "build" / "debug" / "gpupart.spv"), ["fill"])

    @unittest.skipUnless(IS_WINDOWS and sdl3_on_lib(),
                         "the starship needs SDL3.lib on LIB, a GPU driver with Vulkan 1.3, and"
                         " Windows")
    def test_the_starship_meshed_on_the_gpu_as_on_the_cpu(self):
        # packages/starship: a part whose gpu/ holds its distance field and
        # the kernels that sample it and relax its mesh. In coarse cells, and
        # cut into slabs so that their joining is exercised, every level of
        # the frame and the membranes meshed on the GPU is the CPU's mesh:
        # the same counts and triangles, index for index, no sample of a
        # different sign, positions within a hundredth of a cell. The
        # program exits 0 only if each comparison held. Run from a copy, so
        # the repository's package folder gets no build/
        ship = self.root / "starship"
        shutil.copytree(congo.REPO_PACKAGES / "starship", ship,
                        ignore=shutil.ignore_patterns("build"))
        run = self.congo("run", "--release", "--", "--cell", "0.16", "--slab", "12", "--compare",
                         "--no-shots", cwd=ship)
        said = self.program_output(run)
        self.assertEqual(said.count("triangles: 0 indices different, in 0 quads"), 4, said)
        self.assertEqual(said.count(" 0 of a different sign"), 4, said)
        self.assertEqual(said.count(" 0 more than a hundredth of a cell"), 4, said)
        self.assertTrue(said.endswith("OK\n"), said)
        self.assertEqual(entry_points(ship / "build" / "release" / "starship.spv"),
                         ["blocks", "sample", "probe", "move", "shade", "recut", "layerEnd"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
