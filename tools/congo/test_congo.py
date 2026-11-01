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
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
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
                 re.findall(r'fn (is\w+)\(w: &str\) bool \{\s*inWords\(w, "([^"]*)"\)', header)}
        self.assertEqual(lists, {"isKeyword": congo.KEYWORDS, "isReserved": congo.RESERVED,
                                 "isPermission": congo.PERMISSIONS})
        self.assertIsNone(congo.name_fault("usecheck"))
        self.assertIn("keyword", congo.name_fault("use"))
        self.assertIn("reserved", congo.name_fault("wait"))
        self.assertIn("permission", congo.name_fault("mut"))
        self.assertIn("keyword", congo.name_fault("let"))
        self.assertIn("keyword", congo.name_fault("var"))
        self.assertIn("not a Cone name", congo.name_fault("two-words"))


@unittest.skipUnless(IS_WINDOWS, "the loader's dialogs are Windows'")
class ErrorMode(unittest.TestCase):
    """A program that cannot find a DLL ends with a status; it never stops at
    the loader's modal dialog on the desktop. Only the error mode is checked
    here, never a program that is missing a DLL, which would show the dialog
    were the mode wrong (RuntimeLibraries runs one, once the mode is shown to
    be set)."""

    SEM_FAILCRITICALERRORS, SEM_NOOPENFILEERRORBOX = 0x1, 0x8000
    PRINT_MODE = "import ctypes; print(ctypes.WinDLL('kernel32').GetErrorMode())"

    def test_congo_sets_the_error_mode_its_children_inherit(self):
        wanted = self.SEM_FAILCRITICALERRORS | self.SEM_NOOPENFILEERRORBOX
        # Congo's main, started from a parent whose mode is nothing (as a
        # plain console's is, whatever the machine running this test has set),
        # then what a child it starts reports of its own mode
        code = ("import sys, ctypes; sys.path.insert(0, sys.argv[1]); import congo, subprocess\n"
                "ctypes.WinDLL('kernel32').SetErrorMode(0)\n"
                "try: congo.main(['new', '--help'])\n"
                "except SystemExit: pass\n"
                "print(subprocess.run([sys.executable, '-c', sys.argv[2]],"
                " capture_output=True, text=True).stdout.strip())\n")
        ran = subprocess.run([sys.executable, "-c", code, str(HERE), self.PRINT_MODE],
                             capture_output=True, text=True, env=dict(os.environ, PYTHONPATH=""))
        inherited = int(ran.stdout.strip().splitlines()[-1])
        self.assertEqual(inherited & wanted, wanted, ran.stdout + ran.stderr)

    def test_an_exit_status_of_a_missing_dll_is_named(self):
        exe = Path(tempfile.gettempdir()) / "prog.exe"
        self.assertEqual(congo.dll_note(0xC0000135, exe),
                         f"\n{exe} could not start: a DLL it needs was not found (0xC0000135)")
        self.assertEqual(congo.dll_note(3221225781, exe), congo.dll_note(0xC0000135, exe))
        self.assertEqual(congo.dll_note(0, exe), "")
        self.assertEqual(congo.dll_note(None, exe), "")
        self.assertEqual(congo.dll_note(0xC0000409, exe), "")


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
        self.assertIn("pub extern let print: IOStream;", (out / "stdio.cone").read_text())
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
              0i32
            }
            """)
        write(pkg / "src" / "twice" / "twice.cone", """
            mod twice;

            import helper as h;

            pub fn of(n: i64) i64 {
              h.add(n, n)
            }
            """)
        write(pkg / "src" / "helper" / "helper.cone", """
            mod helper;

            pub fn add(a: i64, b: i64) i64 {
              a + b
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
              let half = 7.0 / 2.0;
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
              0i32
            }
            """)
        write(pkg / "src" / "util" / "extra.cone", """
            fn tripled(n: i64) i64 {
              n * 3i64
            }
            """)
        write(pkg / "src" / "twice.cone", """
            mod twice;

            pub fn of(n: i64) i64 {
              n + n
            }
            """)
        write(pkg / "src" / "fmt" / "fmt.cone", """
            mod fmt;

            import stdio;

            pub fn line(label: &str, n: i64) {
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

    def test_release_symbols_and_strip(self):
        """A release build carries debug info, so a profiler can name its
        functions: on Windows its link writes a PDB beside the program, naming
        the program's functions; '--strip' leaves the info out of every compile
        and writes no PDB; a debug build writes one too. On Linux the
        info stays in the ELF, which '--strip' makes smaller."""
        self.congo("new", "sym", cwd=self.root)
        pkg = self.root / "sym"
        write(pkg / "src" / "sym.cone", """
            mod sym;

            import stdio use *;

            // Recursive, so optimisation leaves it a function of its own
            fn fibonacciNumber(n: i32) i32 {
              if n < 2i32 { n } else { fibonacciNumber(n - 1i32) + fibonacciNumber(n - 2i32) }
            }

            fn main() i32 {
              print <- fibonacciNumber(20i32);
              print <- "\\n";
              0i32
            }
            """)
        exe = pkg / "build" / "release" / f"sym{congo.EXE_EXT}"
        pdb = exe.with_suffix(".pdb")
        self.congo("build", "--release", cwd=pkg)
        self.assertTrue(exe.is_file())
        symbolled = exe.stat().st_size
        if IS_WINDOWS:
            self.assertTrue(pdb.is_file(), "a release link writes a PDB")
            self.assertTrue(b"fibonacciNumber" in pdb.read_bytes(),
                            "the PDB names the program's functions")
        # Stripped: the same exe path, no PDB left from the build before
        self.congo("build", "--release", "--strip", cwd=pkg)
        self.assertTrue(exe.is_file())
        self.assertFalse(pdb.exists(), "--strip writes no PDB, and removes the last one")
        if not IS_WINDOWS:
            self.assertLess(exe.stat().st_size, symbolled)
        # A debug build has its own folder, and writes a PDB unless stripped
        self.congo("build", cwd=pkg)
        if IS_WINDOWS:
            self.assertTrue((pkg / "build" / "debug" / "sym.pdb").is_file(), "a debug link writes a PDB")
        self.congo("build", "--strip", cwd=pkg)
        self.assertFalse((pkg / "build" / "debug" / "sym.pdb").exists(), "--strip writes no PDB in debug either")

    def test_a_lone_file(self):
        write(self.root / "lone.cone", """
            mod lone;

            import stdio use *;

            fn main() i32 {
              print <- "lone ";
              print <- 6i64 * 7i64;
              print <- "\\n";
              0i32
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
              0i32
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

    def test_the_hash_seed_is_drawn_once_per_process(self):
        # core's Hasher.seeded() starts from the process's seed, 64 random bits
        # from the OS that conestd draws on first use (processSeed): the same
        # for every ask within a run, never 0, and different on another run, so
        # a table keyed by it visits its entries in another order each time
        write(self.root / "seed.cone", """
            mod seed;

            import stdio use *;

            fn main() i32 {
              print <- processSeed();
              print <- " ";
              print <- processSeed();
              print <- " ";
              if Hasher.seeded().finish() == Hasher.withSeed(processSeed()).finish() {print <- "same";}
              else {print <- "different";}
              print <- "\\n";
              0i32
            }
            """)
        seeds = []
        for _ in range(3):
            run = self.congo("run", "seed.cone", cwd=self.root)
            first, again, same = self.program_output(run).split()
            self.assertEqual(first, again)
            self.assertNotEqual(first, "0")
            self.assertEqual(same, "same")
            seeds.append(first)
        self.assertEqual(len(set(seeds)), 3, "three runs, three seeds")

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
              0i32
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
              0i32
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

            pub var count: i64 = 3i64;

            pub fn hello(n: i64) {
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
              0i32
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
        self.assertIn("pub extern fn hello(n: i64);", (out / "greet.cone").read_text())

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
              pub count: i64;
              pub step: i64;

              pub fn next(self) i64 {
                count + step
              }
            }
            """)
        write(packages / "a" / "src" / "a.cone", """
            mod a;

            import b;

            pub fn start(n: i64) b.Counter {
              b.Counter(n, 5i64)
            }

            pub fn advance(c: b.Counter) b.Counter {
              b.Counter(c.next(), c.step)
            }
            """)
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import a;

            fn main() i32 {
              let c = a.start(30i64);
              stdio.print <- c.next();
              stdio.print <- "\\n";
              let d = a.advance(a.advance(c));
              stdio.print <- d.count;
              stdio.print <- "\\n";
              stdio.print <- d.step;
              stdio.print <- "\\n";
              0i32
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # start(30) is Counter(30, 5), whose next() is 30+5 = 35, from b's
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

    def test_an_include_file_records_the_types_that_change_shape(self):
        # A struct wrapping a List is shape-changing though it declares nothing,
        # found from its methods; a gauge that lends a number it bumps is not.
        # The methods are cut out of the package's include file, so the include
        # file says which: 'ShapeChanging' is written into the type's 'is' list,
        # and an importer takes the type as the file says
        packages = self.root / "shapes"
        self.registry(packages)
        write(packages / "shapes" / "congo.toml",
              '[package]\nname = "shapes"\nversion = "0.1.0"\noutput = "library"\n')
        write(packages / "shapes" / "src" / "shapes.cone", """
            mod shapes;

            import collections use List;

            pub struct Pt { pub x: i64; pub y: i64; }

            // Wraps a List: found shape-changing, with no 'is' list to add to
            pub struct Bag {
              items: List[Pt];

              pub fn empty() Bag { Bag(List[Pt].empty()) }
              pub fn first(self: &) &Pt { &items[0usize] }
              pub fn add(self: &mut, p: Pt) { items.push(p); }
            }

            // The same through a type that has an 'is' list already
            pub struct Shelf is Sendable {
              items: List[Pt];

              pub fn empty() Shelf { Shelf(List[Pt].empty()) }
              pub fn first(self: &) &Pt { &items[0usize] }
              pub fn add(self: &mut, p: Pt) { items.push(p); }
            }

            // Lends a number it writes: not found
            pub struct Gauge {
              n: i64;

              pub fn start(n: i64) Gauge { Gauge(n) }
              pub fn peek(self: &) &i64 { &n }
              pub fn bump(self: &mut) { n = n + 1; }
            }
            """)
        write(self.root / "good.cone", """
            mod good;

            import stdio;
            import shapes;

            fn viaGauge(g: &mut shapes.Gauge) i64 {
              let e = g.peek();
              g.bump();
              *e
            }

            fn usedFirst(bag: &mut shapes.Bag, p: shapes.Pt) i64 {
              let x = bag.first().x;
              bag.add(p);
              x
            }

            fn main() i32 {
              var g = shapes.Gauge.start(41i64);
              stdio.print <- viaGauge(&mut g);
              stdio.print <- "\\n";
              var bag = shapes.Bag.empty();
              bag.add(shapes.Pt(7i64, 0i64));
              stdio.print <- usedFirst(&mut bag, shapes.Pt(8i64, 0i64));
              stdio.print <- "\\n";
              0i32
            }
            """)
        run = self.congo("run", "good.cone", cwd=self.root)
        # The gauge's borrow reads the number it bumped; the bag's first is read before the push
        self.assertEqual(self.program_output(run), "42\n7\n")
        out = next((self.root / "home" / "lone").glob("good-*")) / "debug"
        included = (out / "shapes.cone").read_text()
        self.assertIn("pub struct Bag is ShapeChanging {", included)
        self.assertIn("pub struct Shelf is Sendable, ShapeChanging {", included)
        # Not shape-changing, and its methods reshape nothing a caller borrows: recorded as
        # keeping the shape. Bag and Shelf push to a list, so they are not
        self.assertIn("pub struct Gauge is ShapeKeeping {", included)
        self.assertNotIn("Gauge is ShapeChanging", included)
        self.assertNotIn("ShapeKeeping, ShapeChanging", included)
        self.assertNotIn("ShapeChanging, ShapeKeeping", included)
        self.assertNotIn("Pt is", included)
        for name, spelling in (("bag", "shapes.Bag"), ("shelf", "shapes.Shelf")):
            write(self.root / f"{name}.cone", f"""
                mod {name};

                import shapes;

                fn viaShape(w: &mut {spelling}, p: shapes.Pt) i64 {{
                  let e = w.first();
                  w.add(p);
                  e.x
                }}

                fn main() i32 {{ 0i32 }}
                """)
            run = self.congo("run", f"{name}.cone", cwd=self.root, ok=False)
            self.assertNotEqual(run.returncode, 0, name)
            self.assertIn("could change the shape of a", run.stdout + run.stderr, name)

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

            pub let value: i64;

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
              value
            }
            """)
        write(packages / "b" / "src" / "sub.cone", """
            pub mod sub;

            import stdio;

            pub let base: i64;

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
              b.value
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
              b.get() * 2i64
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
              0i32
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

            pub fn classify(n: i64) kinds.Kind {
              if n < 10i64 {Small} else {Large}
            }

            pub fn code(k: kinds.Kind) i64 {
              if k is Small {1i64} else {2i64}
            }

            // Expanded in the program: it names vec's function and vec.cell's
            pub fn total(s: Stack) i64 inline {
              vec.sum(s) + vec.cell.bonus()
            }
            """)
        write(packages / "coll" / "src" / "vec" / "vec.cone", """
            pub mod vec;

            pub struct Stack {
              pub a: i64;
              pub b: i64;

              pub fn swap(self) Stack {
                Stack(b, a)
              }
            }

            pub fn sum(s: Stack) i64 {
              s.a + s.b
            }

            // Reached by nothing outside vec: not in the include file
            pub fn unseen() i64 {
              0i64
            }
            """)
        write(packages / "coll" / "src" / "vec" / "cell.cone", """
            pub mod cell;

            pub fn bonus() i64 {
              100i64
            }
            """)
        write(packages / "coll" / "src" / "kinds.cone", """
            pub mod kinds;

            pub enum Kind {
              Small, Large
            }
            """)
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import coll;

            fn main() i32 {
              let s = coll.Stack(3i64, 4i64).swap();
              stdio.print <- s.a;
              stdio.print <- "\\n";
              stdio.print <- coll.total(s);
              stdio.print <- "\\n";
              stdio.print <- coll.code(coll.classify(20i64));
              stdio.print <- "\\n";
              0i32
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # Stack(3, 4) swapped is (4, 3): a is 4; total is 4 + 3 from vec's
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
        self.assertIn("pub extern fn sum(s: Stack) i64;", generated)
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
              fn ToIntA(s: *u8) i32;
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
              0i32
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
        write(pkg / "src" / "tri.cone", "mod @c tri;\n\npub extern fn triple(n: i32) i32;\n")
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import tri;

            fn main() i32 {
              stdio.print <- tri.triple(14i32);
              stdio.print <- "\\n";
              0i32
            }
            """)
        run = self.congo("run", "app.cone", cwd=self.root)
        # 3 * 14 = 42
        self.assertEqual(self.program_output(run), "42\n")

    def tri_package(self, packages: Path, link: str) -> Path:
        """The C package 'tri' over tri3, a C library built here into its clib/
        folder ('triple' multiplies by three), with the [link] text link, and an
        app.cone calling triple(14)."""
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
              '[package]\nname = "tri"\nversion = "0.1.0"\noutput = "library"\n' + link)
        write(pkg / "src" / "tri.cone", "mod @c tri;\n\npub extern fn triple(n: i32) i32;\n")
        write(self.root / "app.cone", """
            mod app;

            import stdio;
            import tri;

            fn main() i32 {
              stdio.print <- tri.triple(14i32);
              stdio.print <- "\\n";
              0i32
            }
            """)
        return pkg

    def test_a_link_section_adds_to_the_plain_link_on_its_system(self):
        # The plain [link] applies on every system, and this system's section
        # adds to it. A section for another system is ignored: naming a library
        # that is nowhere would fail the link, if it were used.
        packages = self.root / "cpkgs"
        self.registry(packages)
        here = congo.HOST_OS
        others = [s for s in ("windows", "linux", "macos") if s != here]
        elsewhere = "".join(f'\n[link.{s}]\nlibraries = ["nosuchlib_{s}"]\n' for s in others)
        # The library is in the section, the folder it is in is the plain table's
        pkg = self.tri_package(packages, f'\n[link]\npaths = ["clib"]\n\n[link.{here}]\n'
                               f'libraries = ["tri3"]\n{elsewhere}')
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "42\n")        # 3 * 14
        # The reverse: the library in the plain table, the folder in the section
        head = '[package]\nname = "tri"\nversion = "0.1.0"\noutput = "library"\n'
        write(pkg / "congo.toml", head + f'\n[link]\nlibraries = ["tri3"]\n\n[link.{here}]\n'
              f'paths = ["clib"]\n{elsewhere}')
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "42\n")
        # And no plain [link] at all
        write(pkg / "congo.toml", head + f'\n[link.{here}]\nlibraries = ["tri3"]\n'
              f'paths = ["clib"]\n{elsewhere}')
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "42\n")

    def test_a_systems_libraries_follow_the_plain_ones(self):
        # Lists are concatenated, the plain table's first: the note after a
        # failed link names the libraries in that order, though the section is
        # written first, and another system's section adds none
        packages = self.root / "cpkgs"
        self.registry(packages)
        here = congo.HOST_OS
        other = "linux" if here != "linux" else "windows"
        self.tri_package(packages, f'\n[link.{here}]\nlibraries = ["nosuchlib_b", "nosuchlib_c"]\n'
                         f'\n[link]\nlibraries = ["nosuchlib_a"]\n\n[link.{other}]\n'
                         'libraries = ["nosuchlib_x"]\n')
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn("tri names nosuchlib_a, nosuchlib_b, nosuchlib_c", run.stderr)
        self.assertNotIn("nosuchlib_x", run.stderr)
        if congo.IS_WINDOWS:
            # The Windows SDK's libraries a package calls are named by that
            # package's [link.windows]; Congo's own link line has the C runtime's only
            line = next(l for l in run.stderr.splitlines() if "/SUBSYSTEM:CONSOLE" in l)
            self.assertIn("msvcrt.lib legacy_stdio_definitions.lib", line)
            for lib in ("synchronization", "shell32", "ws2_32", "ntdll"):
                self.assertNotIn(f"{lib}.lib", line)

    def test_link_sections_are_checked_on_every_system(self):
        # A section is read, whether or not it is this system's, so a mistake
        # in it is found on any machine; a library only reads its manifest
        self.congo("new", "lib1", "--lib", cwd=self.root)
        pkg = self.root / "lib1"
        head = '[package]\nname = "lib1"\nversion = "1.0.0"\noutput = "library"\n'

        def refused(link: str, want: str) -> None:
            write(pkg / "congo.toml", head + link)
            run = self.congo("build", cwd=pkg, ok=False)
            self.assertEqual(run.returncode, 1, link)
            self.assertIn(want, run.stderr, link)

        for system in ("windows", "linux", "macos"):
            refused(f'[link.{system}]\nlibraries = ["SDL2.lib"]\n',
                    f'[link.{system}] library "SDL2.lib" must be the library\'s bare name')
            refused(f'[link.{system}]\nlibs = ["SDL2"]\n',
                    f"'libs' is not a [link.{system}] key; the keys are libraries, paths"
                    " and runtime")
            refused(f'[link.{system}]\nruntime = "SDL2"\n',
                    f"[link.{system}] runtime must be a list of strings")
            refused(f'[link.{system}]\npaths = "clib"\n',
                    f"[link.{system}] paths must be a list of strings")
            refused(f'[link]\nlibraries = ["a"]\n[link.{system}]\nruntime = ["x.dll"]\n',
                    f'[link.{system}] runtime "x.dll" must be the DLL\'s bare name')
            refused(f'[link]\n{system} = ["x"]\n',
                    f"[link] {system} must be a table: write it as [link.{system}]")
        sections = ("[link.freebsd] is not a link section; the sections are [link.windows],"
                    " [link.linux] and [link.macos]")
        refused('[link.freebsd]\nlibraries = ["a"]\n', sections)
        refused('[link]\nlibraries = ["a"]\n[link.freebsd]\nlibraries = ["a"]\n', sections)
        refused('[link.windows]\nlibraries = ["a"]\n[link.windows.x]\nlibraries = ["a"]\n',
                "[link.windows.x] is not a link section; the sections are")
        # The plain table's own errors are as they were
        refused('[link]\nlibraries = ["SDL2.lib"]\n[link.linux]\nlibraries = ["a"]\n',
                '[link] library "SDL2.lib" must be the library\'s bare name')
        refused('[link]\nlibs = ["SDL2"]\n', "'libs' is not a [link] key")
        # All three at once, with the same keys as [link], is fine (a library
        # links nothing, so the names are only read)
        write(pkg / "congo.toml", head + '\n[link]\nlibraries = ["z"]\n'
              '\n[link.windows]\nlibraries = ["user32"]\npaths = ["w"]\nruntime = ["w3"]\n'
              '\n[link.linux]\nlibraries = ["ssl", "crypto"]\npaths = ["l"]\n'
              '\n[link.macos]\nlibraries = ["objc"]\n')
        self.congo("build", cwd=pkg)

    def test_a_dotted_table_is_part_of_the_table_it_starts_with(self):
        # [link.linux] does not make an unknown table of the manifest a known
        # one, and [package.x] is a [package] key that is not one
        self.congo("new", "lib1", "--lib", cwd=self.root)
        pkg = self.root / "lib1"
        head = '[package]\nname = "lib1"\nversion = "1.0.0"\noutput = "library"\n'
        write(pkg / "congo.toml", head + '[link.linux]\nlibraries = ["a"]\n[extra.linux]\n')
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertIn("[extra] is not part of a Congo manifest, which holds [package] and"
                      " [link].", run.stderr)
        write(pkg / "congo.toml", head + '[package.x]\nk = 1\n')
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertIn("'x' is not a [package] key; the keys are name, version, output and"
                      " targets", run.stderr)

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

    def test_the_os_layer_sample(self):
        # samples/oslayer, copied here and run: libc and posix from the
        # repository's packages, posix importing libc. Each step prints 'ok' when
        # what it checked held; the program exits with the count that did not.
        # The same program and the same output on Windows and on Linux
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
              ok    getcwd there is getcwd here with a separator and sub after it
              ok    chdir came back out
              ok    realpath of sub/../notes.txt is here with a separator and notes.txt after it
              ok    listing found two entries: notes.txt (a file) and sub (a directory)
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

    def test_every_sample_compiles(self):
        # Every program under samples/ is compiled for its own target, so none
        # can rot unseen (webgl did, from #393 until 016a0e83). A sample with a
        # congo.toml is a package: copied here and built by Congo. Any other is
        # a lone program, its designated file named for its folder, compiled by
        # conec alone; one holding wasm.syms (the browser's imports) is for
        # WebAssembly. A folder with no .cone file at all (a retired sample's
        # leftover build folder) is passed over
        samples = congo.REPO / "samples"
        seen = []
        for folder in sorted(p for p in samples.iterdir() if p.is_dir()):
            if not any(folder.rglob("*.cone")):
                continue
            seen.append(folder.name)
            with self.subTest(sample=folder.name):
                if (folder / "congo.toml").is_file():
                    copy = self.root / "samples" / folder.name
                    shutil.copytree(folder, copy, ignore=shutil.ignore_patterns("build"))
                    self.congo("build", cwd=copy)
                    continue
                main = folder / f"{folder.name}.cone"
                self.assertTrue(main.is_file(), f"{folder.name} has no {main.name}")
                target = ["--wasm"] if (folder / "wasm.syms").is_file() else []
                out = self.root / "samples-out" / folder.name
                out.mkdir(parents=True)
                run = subprocess.run([str(congo.REPO_CONEC), *target, "-o", str(out), main.name],
                                     cwd=folder, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertNotRegex(run.stdout + run.stderr, r"(?m)^Error \d+",
                                    f"{folder.name}:\n{run.stdout}{run.stderr}")
        self.assertIn("oslayer", seen)
        self.assertIn("webgl", seen)

    def test_conec_reports_a_sane_compile_time(self):
        # conec's closing line gives the time the compile took. On Linux the
        # timer once read only the nanoseconds within the current second, so a
        # stage that crossed a second boundary wrapped the unsigned total to
        # 2^64 ns ("1.84467e+10 sec"). Only a boundary falling inside a timed
        # stage wrapped (3 compiles of this program in 60 on the VM), so the
        # compiles go on for three seconds, each a small positive time: the old
        # timer failed this most runs, the fixed one never does
        src = self.root / "timed.cone"
        write(src, "mod timed;\nfn main() {}\n")
        out = self.root / "timed-out"
        out.mkdir()
        until = time.monotonic() + 3.0
        while time.monotonic() < until:
            run = subprocess.run([str(congo.REPO_CONEC), "-o", str(out), src.name],
                                 cwd=self.root, capture_output=True, text=True)
            said = re.search(r"Compile finished in (\S+) sec", run.stdout + run.stderr)
            self.assertIsNotNone(said, run.stdout + run.stderr)
            secs = float(said.group(1))
            self.assertTrue(0.0 <= secs < 60.0, f"conec says the compile took {secs} sec")

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
        write(self.root / "p.cone", "import ping;\n\nfn main() i32 {\n  0i32\n}\n")
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
              1i64
            }

            fn main() i32 {
              0i32
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
        write(self.root / "p.cone", "import nosuch;\n\nfn main() i32 {\n  0i32\n}\n")
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
        main = "fn main() i32 {\n  0i32\n}\n"
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
        write(pkg / "src" / "wait" / "wait.cone", "mod wait;\n")
        run = self.congo("build", cwd=pkg, ok=False)
        self.assertIn("wait.cone: a module folder names its module: 'wait' is reserved",
                      run.stderr)
        self.assertFalse((pkg / "build" / "debug" / "shelf.conebuild").exists())
        shutil.rmtree(pkg / "src" / "wait")
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

            pub fn twice(n: i64) i64 {
              n * 2i64
            }
            """)
        test = """
            mod {name};

            import stdio use *;
            import counter;

            fn main() i32 {{
              printInt(counter.twice({n}i64));
              printStr("\\n");
              {status}
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
              0i32
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
        self.assertIn("pub extern fn twice(n: i64) i64;", (build / "counter.cone").read_text())
        desc = (build / "tests" / "doubles" / "doubles.conebuild").read_text()
        self.assertRegex(desc, r'import counter: ".*/build/debug/counter\.cone"')
        self.assertTrue((build / "tests" / "doubles" / f"doubles{congo.EXE_EXT}").is_file())
        self.assertFalse((build / "examples" / "broken" / f"broken{congo.EXE_EXT}").exists())

    def test_a_test_has_expected_files_of_its_own_for_a_host(self):
        # tests/<name>.<host>.out (and .err) is compared in place of
        # tests/<name>.out where it exists for the running host, as the
        # compiler suite's run.py does; another host's file is not looked at
        pkg = self.root / "perhost"
        write(pkg / "congo.toml",
              '[package]\nname = "perhost"\nversion = "0.1.0"\noutput = "library"\n')
        write(pkg / "src" / "perhost.cone", """
            mod perhost;

            pub fn one() i64 {
              1i64
            }
            """)
        write(pkg / "tests" / "says.cone", """
            mod says;

            import stdio use *;
            import perhost;

            fn main() i32 {
              printInt(perhost.one());
              printStr(" on this host\\n");
              eprintStr("note\\n");
              0i32
            }
            """)
        other = "macos" if congo.HOST_OS != "macos" else "windows"
        write(pkg / "tests" / "says.out", "plain file, wrong here\n")
        write(pkg / "tests" / f"says.{congo.HOST_OS}.out", "1 on this host\n")
        write(pkg / "tests" / f"says.{other}.out", "another host's file\n")
        write(pkg / "tests" / "says.err", "plain file, wrong here\n")
        write(pkg / "tests" / f"says.{congo.HOST_OS}.err", "note\n")
        run = self.congo("test", cwd=pkg, ok=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("test says ... ok", run.stdout)
        # With the host's files gone, the plain ones are compared, and fail
        (pkg / "tests" / f"says.{congo.HOST_OS}.out").unlink()
        (pkg / "tests" / f"says.{congo.HOST_OS}.err").unlink()
        run = self.congo("test", cwd=pkg, ok=False)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn("test says ... FAILED", run.stdout)
        self.assertIn("plain file, wrong here", run.stdout)

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
              count: u64;

              pub fn init(self: &new, start: u64) {
                *self = Self(count: start);
              }

              pub async fn ping(self, n: u64) {
                count = count + n;
              }

              fn final(self: &uni) {
                printStr("pinger "); printUInt(count); printStr("\\n");
              }
            }

            actor Hidden {
              pub async fn hello(self, n: u64, p: Pinger) {
                printStr("hidden "); printUInt(n); printStr("\\n");
                p.ping(n);
              }
            }

            pub fn useHidden(p: Pinger) {
              let h = Hidden();
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
                let p = pinger.Pinger(5u64);
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

    def test_a_public_actor_keeps_its_states_private_names_private(self):
        # The include file holds a public actor whole, state and bodies, so the
        # private names those name go in beside it, private still: a struct
        # (with a private method) and an enum its state holds, a struct only
        # its state's field names, and a function its behaviour calls. The importer generates the actor again and
        # links against the library's object for the private ones. By hand:
        # add(1) keeps Entry(101), weight 202; add(2) keeps Entry(102), weight
        # 204; the final prints 202 + 204 = 406
        pkg = self.root / "tally"
        write(pkg / "congo.toml",
              '[package]\nname = "tally"\nversion = "0.1.0"\noutput = "library"\n')
        write(pkg / "src" / "tally.cone", """
            mod tally;

            import stdio use *;
            import actors;
            import collections use *;

            enum Kind {
              Small;
              Large;
            }

            struct Entry {
              n: u64;
              kind: Kind;

              fn weight(self: &) u64 {
                n * 2u64
              }
            }

            fn scale(n: u64) u64 {
              n + 100u64
            }

            pub actor Tally {
              entries: List[Entry];
              total: u64;

              pub fn init(self: &new) {
                *self = Self(entries: List[Entry].empty(), total: 0u64);
              }

              pub async fn add(self, n: u64) {
                entries.push(Entry(scale(n), Kind.Large));
                total = total + entries[entries.len() - 1usize].weight();
              }

              fn final(self: &uni) {
                printStr("tally "); printUInt(total); printStr("\\n");
              }
            }

            struct Mark {
              at: u64;
            }

            // Mark is named only by the state's field: the implicit initializer
            // is the module's own, so a function makes the actor
            pub actor Stamp {
              last: Mark;

              pub async fn touch(self) {
                last = Mark(last.at + 1u64);
              }
            }

            pub fn stamp() Stamp {
              Stamp(Mark(0u64))
            }
            """)
        write(pkg / "tests" / "useit.cone", """
            mod useit;

            import tally;
            import actors;

            fn main() {
              initAll();
              {
                let t = tally.Tally();
                t.add(1u64);
                t.add(2u64);
                let s = tally.stamp();
                s.touch();
              }
              finalAll();
            }
            """)
        write(pkg / "tests" / "useit.out", "tally 406\n")
        run = self.congo("test", cwd=pkg)
        self.assertIn("test useit ... ok", run.stdout)
        include = (pkg / "build" / "debug" / "tally.cone").read_text()
        self.assertIn("pub actor Tally {", include)
        self.assertIn("\nstruct Entry {", include)
        self.assertIn("\nenum Kind {", include)
        self.assertIn("\nstruct Mark {", include)
        self.assertNotIn("pub struct Entry", include)
        self.assertNotIn("pub struct Mark", include)

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
              0i32
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
        write(pkg / "tests" / "use.cone", "mod use;\n\nfn main() i32 {\n  0i32\n}\n")
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

    def test_bless_makes_the_exit_file_match_the_run(self):
        pkg = self.counter(self.root)
        tests = pkg / "tests"
        # A test that once ended with 1 and now ends with 0: blessing it again
        # (its .out deleted) removes the stale .exit, or the next run would fail
        (tests / "doubles.exit").write_text("1\n")
        (tests / "doubles.out").unlink()
        run = self.congo("test", "doubles", "--bless", cwd=pkg)
        self.assertIn("blessed", run.stdout)
        self.assertIn("removed the stale", run.stdout)
        self.assertEqual((tests / "doubles.out").read_bytes(), b"42\n")
        self.assertFalse((tests / "doubles.exit").exists())
        self.congo("test", "doubles", cwd=pkg)
        # One that ends with a status gets its .exit written, over a stale one
        (tests / "status.exit").write_text("9\n")
        (tests / "status.out").unlink()
        run = self.congo("test", "status", "--bless", cwd=pkg)
        self.assertIn("status.exit", run.stdout)
        self.assertNotIn("removed the stale", run.stdout)
        self.assertEqual((tests / "status.exit").read_text(), "3\n")
        self.congo("test", "status", cwd=pkg)

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
              "pub fn half(n: i64) i64 {\n"
              '  assertDebugMsg(n % 2i64 == 0i64, "only an even number halves");\n'
              "  n / 2i64\n"
              "}\n")
        test = """
            mod {name};

            import stdio use *;
            import halver;

            fn main() i32 {{
              printInt(halver.half({n}i64));
              printStr("\\n");
              0i32
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

    def test_a_test_that_skips_itself(self):
        # A test that exits with 77 (testing.skip) ran no checks: it is reported
        # as skipped with the last line it printed, nothing is compared or
        # blessed, and the run succeeds. One whose .exit holds 77 is compared
        # as any other, so a test of skip itself still checks its output
        pkg = self.root / "needy"
        write(pkg / "congo.toml",
              '[package]\nname = "needy"\nversion = "0.1.0"\noutput = "library"\n')
        write(pkg / "src" / "needy.cone", "mod needy;\n\npub fn one() i32 { 1i32 }\n")
        test = """
            mod {name};

            import stdio use *;
            import needy;

            fn main() i32 {{
              printStr("{said}\\n");
              {status}i32
            }}
            """
        write(pkg / "tests" / "plain.cone", test.format(name="plain", said="1", status=0))
        write(pkg / "tests" / "plain.out", "1\n")
        write(pkg / "tests" / "needs.cone",
              test.format(name="needs", said="skipped: the data file is absent", status=77))
        run = self.congo("test", "--bless", cwd=pkg)
        self.assertIn("test plain ... ok", run.stdout)
        self.assertIn("test needs ... skipped (the data file is absent)", run.stdout)
        self.assertIn("needy: 2 tests: 1 passed, 0 failed, 1 skipped (the data file is absent);"
                      " 0 examples: 0 built, 0 failed to build", run.stdout)
        for suffix in (".out", ".exit"):
            self.assertFalse((pkg / "tests" / f"needs{suffix}").exists(), suffix)
        # The name filter finds it, and skipping is not a failure
        run = self.congo("test", "needs", cwd=pkg)
        self.assertIn("needy: 1 test: 0 passed, 0 failed, 1 skipped (the data file is absent);",
                      run.stdout)
        # Expected to exit 77, it is compared: a wrong output fails
        write(pkg / "tests" / "needs.exit", "77\n")
        write(pkg / "tests" / "needs.out", "skipped: the data file is absent\n")
        run = self.congo("test", "needs", cwd=pkg)
        self.assertIn("test needs ... ok", run.stdout)
        write(pkg / "tests" / "needs.out", "skipped: something else\n")
        run = self.congo("test", "needs", cwd=pkg, ok=False)
        self.assertIn("test needs ... FAILED", run.stdout)
        self.assertIn("output differs:", run.stdout)

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
        write(pkg / "src" / "tri.cone", "mod @c tri;\n\npub extern fn triple(n: i32) i32;\n")
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
              0i32
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
        # ... and says so, by the program's full path, the loader's dialog
        # suppressed (ErrorMode)
        self.assertIn(f"{(out / 'app.exe').resolve()} could not start: a DLL it needs was"
                      f" not found (0xC0000135)", run.stderr)
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

    def test_a_runtime_dll_may_be_named_by_the_windows_section(self):
        # [link.windows] holds the same keys as [link]: its library, folder and
        # DLL are added to the plain table's (here, none), and a mistake in
        # its runtime list is named by the section
        packages = self.root / "cpkgs"
        self.registry(packages)
        pkg = self.tri(packages, None)
        self.app()
        write(pkg / "congo.toml", self.TRI_HEAD + '\n[link.windows]\nlibraries = ["tri3"]\n'
              'paths = ["clib"]\nruntime = ["tri3"]\n\n[link.linux]\nruntime = ["nosuch"]\n')
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "42\n")
        out = next((self.root / "home" / "lone").glob("app-*")) / "debug"
        source = pkg / "clib" / "tri3.dll"
        self.assertEqual((out / "tri3.dll").read_bytes(), source.read_bytes())
        # A DLL that is nowhere is named with the package, whichever table
        # held it
        write(pkg / "congo.toml", self.TRI_HEAD + '\n[link]\nruntime = ["tri3"]\n'
              '\n[link.windows]\nlibraries = ["tri3"]\npaths = ["clib"]\nruntime = ["nosuch"]\n')
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertIn("congo: error: tri's [link] runtime names nosuch, but nosuch.dll is in"
                      " none of the folders Congo looks in: [link] paths (", run.stderr)
        write(pkg / "congo.toml", self.TRI_HEAD + '\n[link.windows]\nruntime = ["tri3.dll"]\n')
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertIn("[link.windows] runtime \"tri3.dll\" must be the DLL's bare name",
                      run.stderr)

    def test_a_test_missing_its_dll_names_the_program(self):
        # 'congo test' runs the program as a debuggee; with no dialog, the
        # failure says which program could not start
        packages = self.root / "cpkgs"
        self.registry(packages)
        self.tri(packages, None)
        user = self.root / "user"
        write(user / "congo.toml",
              '[package]\nname = "user"\nversion = "0.1.0"\noutput = "library"\n')
        write(user / "src" / "user.cone", "mod user;\n\nimport tri;\n")
        write(user / "tests" / "triple.cone", """
            mod triple;

            import stdio;
            import tri;

            fn main() i32 {
              stdio.print <- tri.triple(14i32);
              0i32
            }
            """)
        write(user / "tests" / "triple.out", "42")
        run = self.congo("test", cwd=user, ok=False)
        exe = (user / "build" / "debug" / "tests" / "triple" / "triple.exe").resolve()
        self.assertIn(f"{exe} could not start: a DLL it needs was not found (0xC0000135)",
                      run.stdout + run.stderr)

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

            pub fn sextuple(n: i32) i32 {
              2i32 * tri.triple(n)
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
              0i32
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
        write(pkg / "src" / "fake.cone", "mod fake;\n\npub fn one() i32 {\n  1i32\n}\n")
        write(self.root / "app.cone", """
            mod app;

            import fake;

            fn main() i32 {
              fake.one() - 1i32
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
              0i32
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

        pub fn half(x: f32) f32 {
          x * 0.5
        }

        // A kernel: one invocation for each point
        pub fn @compute(64) scale(inv: Invocation, points: &Array[Vec3], out: &mut Array[f32]) {
          let i = usize.from(inv.globalId[0usize]);
          if i < out.len && i < points.len {
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
          0i32
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
            // fn @compute(64) scale(inv: Invocation, out: &mut Array[f32]) is not here
            mod quiet;

            pub fn said() &Array[u8] {
              "fn @compute(64)"
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
        self.package(pkgs / "plain", "plain", "mod plain;\n\npub fn one() i32 {\n  1i32\n}\n",
                     gpu=False)
        self.package(pkgs / "loose", "loose", "mod loose;\n\npub fn two() i32 {\n  2i32\n}\n",
                     gpu=False)
        mid = self.package(pkgs / "mid", "mid",
                           "mod mid;\n\nimport plain;\n\npub fn three() i32 {\n"
                           "  plain.one() + 2i32\n}\n")
        kern = self.package(pkgs / "kern", "kern", """
            mod kern;

            import mid;
            import loose;

            pub fn @compute(64) fill(inv: Invocation, out: &mut Array[i32]) {
              let i = usize.from(inv.globalId[0usize]);
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
                                      "  mid.three() - 3i32\n}\n")
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

        pub fn half(x: f32) f32 {
          x * 0.5
        }
        """
    MATHS_MORE = """
        pub fn len(p: Vec3) f32 {
          p.length()
        }
        """
    # A module of one file in gpu/, importing its sister there, maths
    KERN = """
        mod kern;

        import geomath use Vec3;
        import maths;

        // A kernel: one invocation for each point
        pub fn @compute(64) scale(inv: Invocation, points: &Array[Vec3], out: &mut Array[f32]) {
          let i = usize.from(inv.globalId[0usize]);
          if i < out.len && i < points.len {
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

            pub fn quarter(x: f32) f32 {
              maths.half(user.halved(x))
            }
            """)
        # A module of src/ may import one of gpu/, its sister
        write(shapes / "src" / "user.cone", """
            mod user;

            import maths;

            pub fn halved(x: f32) f32 {
              maths.half(x)
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
              if shapes.quarter(8.) == 2. && shapes.half(8.) == 4. {
                stdio.print <- "a quarter of 8 is 2\\n";
              }
              0i32
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
              0i32
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
              42i32
            }
            """)
        write(lib / "src" / "helpers.cone", "mod helpers;\n\npub fn one() i32 {\n  1i32\n}\n")
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

            pub fn @compute(64) fill(inv: Invocation, out: &mut Array[f32]) {
              let i = usize.from(inv.globalId[0usize]);
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

            pub fn down(n: u32) u32 {
              if n == 0u32 {0u32} else {down(n - 1u32)}
            }

            pub fn @compute(64) fill(inv: Invocation, out: &mut Array[f32]) {
              let i = usize.from(inv.globalId[0usize]);
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
        write(loose, "pub fn one() i32 {\n  1i32\n}\n")
        self.assertIn(f"congo: error: {loose}: a file in gpu/ opens with its own 'mod' line, as"
                      f" a module of one file named for the file: gpu/ holds the package's GPU"
                      f" modules, and a file with no 'mod' line would join none\n",
                      self.congo("build", cwd=lib, ok=False).stderr)
        loose.unlink()
        bare = lib / "gpu" / "bare"
        write(bare / "one.cone", "pub fn one() i32 {\n  1i32\n}\n")
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
        write(lib / "gpu" / "kern.cone", "mod kern;\n\npub fn @compute(64) fill(inv: Invocation,"
                                         " out: &mut Array[f32]) {\n}\n")
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


def compiled(run: subprocess.CompletedProcess) -> list[str]:
    """The packages a build compiled, in order, from its 'Compiling' lines; a
    package's kernels as '<name> for the GPU'."""
    names = []
    for line in run.stdout.splitlines():
        words = line.split()
        if words[:1] == ["Compiling"]:
            names.append(words[1] + (" for the GPU" if "for the GPU" in line else ""))
    return names


class BuildCache(unittest.TestCase):
    """A compile is skipped where its key is unchanged since the last compile
    that succeeded in that build folder and its outputs are the files that
    compile wrote (README.md, "What a build does"). A stale object is the
    failure that matters: each scenario changes one thing a compile reads and
    checks that exactly the compiles that read it are done again, and that the
    program then does what its source now says."""

    setUp = Scenarios.setUp
    tearDown = Scenarios.tearDown
    congo = Scenarios.congo
    program_output = Scenarios.program_output
    registry = Scenarios.registry

    LEAF = """
        mod leaf;

        pub fn value() i64 {{
          {value}i64
        }}
        {more}"""

    ALL = ["libc", "core", "stdio", "leaf", "mid", "app"]

    def chain(self) -> Path:
        """leaf, a library; mid, a library importing leaf; app, a program
        importing mid, which prints mid.twice()."""
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        for name in ("leaf", "mid"):
            write(pkgs / name / "congo.toml",
                  f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n')
        self.set_leaf(10)
        write(pkgs / "mid" / "src" / "mid.cone", """
            mod mid;

            import leaf;

            pub fn twice() i64 {
              leaf.value() * 2i64
            }
            """)
        app = self.root / "app"
        write(app / "congo.toml",
              '[package]\nname = "app"\nversion = "0.1.0"\noutput = "executable"\n')
        self.set_app("")
        return app

    def set_leaf(self, value: int, more: str = "") -> None:
        write(self.root / "pkgs" / "leaf" / "src" / "leaf.cone",
              self.LEAF.format(value=value, more=more))

    def set_app(self, plus: str) -> None:
        write(self.root / "app" / "src" / "app.cone", f"""
            mod app;

            import stdio;
            import mid;

            fn main() i32 {{
              stdio.print <- mid.twice(){plus};
              stdio.print <- "\\n";
              0i32
            }}
            """)

    def run_app(self, app: Path, *args: str) -> tuple[list[str], str]:
        run = self.congo("run", *args, cwd=app)
        return compiled(run), self.program_output(run)

    def test_only_what_reads_a_change_is_compiled_again(self):
        app = self.chain()
        out = app / "build" / "debug"
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        # Nothing changed: nothing compiled, the program linked and run
        self.assertEqual(self.run_app(app), ([], "20\n"))
        for name in self.ALL:
            key = (out / f"{name}.key").read_text(encoding="utf-8")
            self.assertTrue(key.startswith("congo build key 1\nkey "), name)
            self.assertIn(f"output {congo.content_hash(out / f'{name}{congo.OBJ_EXT}')} ", key)
        # The key's inputs: conec and conestd by content, the command line,
        # the environment conec reads, the manifest, the description, every
        # source and every include file the description names
        mid_key = (out / "mid.key").read_text(encoding="utf-8")
        for kind in ("conec", "conestd", "command", "env CONE_PACKAGES", "manifest",
                     "description", "source", "include"):
            self.assertRegex(mid_key, rf"(?m)^{kind}[ =]", kind)
        self.assertIn(str(out / "leaf.cone"), mid_key)
        # One line of the program: the program alone
        self.set_app(" + 1i64")
        self.assertEqual(self.run_app(app), (["app"], "21\n"))
        # A body of an import changed, its include file not: the import alone,
        # and the program links its new object
        before = (out / "leaf.cone").read_bytes()
        self.set_leaf(11)
        self.assertEqual(self.run_app(app), (["leaf"], "23\n"))
        self.assertEqual((out / "leaf.cone").read_bytes(), before)
        # An interface changed: the import, and everything whose description
        # names its include file, mid directly and app among its package lines
        self.set_leaf(11, "\npub fn extra() i64 {\n  1i64\n}\n")
        self.assertEqual(self.run_app(app), (["leaf", "mid", "app"], "23\n"))
        self.assertNotEqual((out / "leaf.cone").read_bytes(), before)
        # An object or an include file gone, or not the file its compile wrote:
        # that package. An include file regenerated the same as before
        # compiles nothing after it
        (out / f"mid{congo.OBJ_EXT}").unlink()
        self.assertEqual(self.run_app(app), (["mid"], "23\n"))
        (out / "leaf.cone").unlink()
        self.assertEqual(self.run_app(app), (["leaf"], "23\n"))
        stdio_obj = out / f"stdio{congo.OBJ_EXT}"
        stdio_obj.write_bytes(stdio_obj.read_bytes() + b"\0")
        self.assertEqual(self.run_app(app), (["stdio"], "23\n"))
        # A key file gone, or not one this Congo reads
        (out / "core.key").unlink()
        (out / "libc.key").write_text("congo build key 0\n", encoding="utf-8")
        self.assertEqual(self.run_app(app), (["libc", "core"], "23\n"))
        # The manifest is part of the key, though this change compiles the same
        write(self.root / "pkgs" / "mid" / "congo.toml",
              '[package]\nname = "mid"\nversion = "0.1.0"\noutput = "library"\n'
              'targets = ["native"]\n')
        self.assertEqual(self.run_app(app), (["mid"], "23\n"))
        # A file added to a package changes its description: that package;
        # and its include file, whose banner names the files it was made
        # from, so what reads that
        write(self.root / "pkgs" / "mid" / "src" / "more.cone",
              "fn helper() i64 {\n  3i64\n}\n")
        self.assertEqual(self.run_app(app), (["mid", "app"], "23\n"))
        self.assertIn("from mid.cone and more.cone", (out / "mid.cone").read_text())
        # --no-cache compiles everything, and records keys that stand
        self.assertEqual(self.run_app(app, "--no-cache"), (self.ALL, "23\n"))
        self.assertEqual(self.run_app(app), ([], "23\n"))
        # congo clean deletes the keys with the build folder
        self.congo("clean", cwd=app)
        self.assertEqual(self.run_app(app), (self.ALL, "23\n"))

    def test_each_mode_and_each_set_of_defines(self):
        app = self.chain()
        write(app / "src" / "app.cone", """
            mod app;

            import stdio;
            import mid;

            fn main() i32 {
              stdio.print <- mid.twice() + definedInt("N");
              stdio.print <- "\\n";
              0i32
            }
            """)
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        # Release builds into a folder of its own, its own cache
        self.assertEqual(self.run_app(app, "--release"), (self.ALL, "20\n"))
        self.assertEqual(self.run_app(app), ([], "20\n"))
        self.assertEqual(self.run_app(app, "--release"), ([], "20\n"))
        # The defines are on every compile's command line, so in every key: a
        # build with other defines compiles everything again, in the same folder
        self.assertEqual(self.run_app(app, "-D", "N=5"), (self.ALL, "25\n"))
        self.assertEqual(self.run_app(app, "-D", "N=5"), ([], "25\n"))
        self.assertEqual(self.run_app(app, "-D", "N=6"), (self.ALL, "26\n"))
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        self.assertEqual(self.run_app(app, "--release"), ([], "20\n"))

    def test_an_environment_variable_conec_reads_recompiles(self):
        # CONE_NO_NOALIAS changes the code conec makes without changing any
        # file or command line, so it is in every key
        app = self.chain()
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        self.assertEqual(self.run_app(app), ([], "20\n"))
        self.env["CONE_NO_NOALIAS"] = "1"
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        self.assertEqual(self.run_app(app), ([], "20\n"))
        del self.env["CONE_NO_NOALIAS"]
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))

    def test_another_conec_or_conestd_compiles_everything(self):
        app = self.chain()
        tools = self.root / "tools"
        tools.mkdir()
        conec = tools / congo.REPO_CONEC.name
        shutil.copyfile(congo.REPO_CONEC, conec)
        conestd = tools / congo.conestd_path(congo.REPO_CONEC).name
        shutil.copyfile(congo.conestd_path(congo.REPO_CONEC), conestd)
        self.env["CONEC"] = str(conec)
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        self.assertEqual(self.run_app(app), ([], "20\n"))
        # conec rebuilt where it stands: another file by its content, whatever
        # its time. A byte after the end of an executable changes nothing it does
        with conec.open("ab") as file:
            file.write(b"\0")
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        self.assertEqual(self.run_app(app), ([], "20\n"))
        # conestd likewise. Built as a library, mid is not linked with it
        mid = self.root / "pkgs" / "mid"
        self.assertEqual(compiled(self.congo("build", cwd=mid)), ["libc", "core", "leaf", "mid"])
        self.assertEqual(compiled(self.congo("build", cwd=mid)), [])
        with conestd.open("ab") as file:
            file.write(b"\0")
        self.assertEqual(compiled(self.congo("build", cwd=mid)), ["libc", "core", "leaf", "mid"])
        # Another conec, at another path
        del self.env["CONEC"]
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))

    def wrapper(self) -> Path:
        """A conec that runs the repository's, and then, where
        CONGO_TEST_CLOBBER names a file, writes over it as a compile killed
        half-way would leave it and ends with a failure; and where
        CONGO_TEST_HANG names a folder, says so there first and waits for
        'release' to appear in it, so that Congo can be killed while it runs."""
        tools = self.root / "wrap"
        if IS_WINDOWS:
            script = tools / "conec.bat"
            write(script, f"""
                @echo off
                "{congo.REPO_CONEC}" %*
                if not defined CONGO_TEST_CLOBBER exit /b %errorlevel%
                echo half-written> "%CONGO_TEST_CLOBBER%"
                if not defined CONGO_TEST_HANG exit /b 1
                echo started> "%CONGO_TEST_HANG%\\started"
                :wait
                if exist "%CONGO_TEST_HANG%\\release" goto done
                ping -n 2 127.0.0.1 >nul
                goto wait
                :done
                echo done> "%CONGO_TEST_HANG%\\done"
                exit /b 1
                """)
        else:
            script = tools / "conec"
            write(script, f"""
                #!/bin/sh
                "{congo.REPO_CONEC}" "$@"
                status=$?
                [ -z "$CONGO_TEST_CLOBBER" ] && exit $status
                echo half-written > "$CONGO_TEST_CLOBBER"
                [ -z "$CONGO_TEST_HANG" ] && exit 1
                echo started > "$CONGO_TEST_HANG/started"
                while [ ! -f "$CONGO_TEST_HANG/release" ]; do sleep 0.2; done
                echo done > "$CONGO_TEST_HANG/done"
                exit 1
                """)
            script.chmod(0o755)
        return script

    def test_a_compile_that_fails_or_is_killed_leaves_no_key(self):
        app = self.chain()
        out = app / "build" / "debug"
        self.env["CONEC"] = str(self.wrapper())
        self.env["CONESTD"] = str(congo.conestd_path(congo.REPO_CONEC))
        self.assertEqual(self.run_app(app), (self.ALL, "20\n"))
        self.assertTrue((out / "leaf.key").is_file())
        # conec ends with a failure, its object half-written: no key. With the
        # source put back as it was, its key is the one recorded before, and
        # leaf is compiled again all the same
        self.set_leaf(11)
        self.env["CONGO_TEST_CLOBBER"] = str(out / f"leaf{congo.OBJ_EXT}")
        run = self.congo("run", cwd=app, ok=False)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn("could not compile leaf", run.stderr)
        self.assertFalse((out / "leaf.key").exists())
        del self.env["CONGO_TEST_CLOBBER"]
        self.set_leaf(10)
        self.assertEqual(self.run_app(app), (["leaf"], "20\n"))
        # Congo itself killed while conec runs: the key was removed before
        # conec started
        self.set_leaf(12)
        hang = self.root / "hang"
        hang.mkdir()
        self.env["CONGO_TEST_CLOBBER"] = str(out / f"leaf{congo.OBJ_EXT}")
        self.env["CONGO_TEST_HANG"] = str(hang)
        killed = subprocess.Popen([*CONGO, "run"], cwd=app, env=self.env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            until = time.monotonic() + 600
            while not (hang / "started").exists():
                self.assertIsNone(killed.poll(), "congo ended before conec was killed")
                self.assertLess(time.monotonic(), until, "conec never started")
                time.sleep(0.2)
            self.assertFalse((out / "leaf.key").exists())
        finally:
            killed.kill()
            killed.wait()
            (hang / "release").write_text("go\n")
        until = time.monotonic() + 60
        while not (hang / "done").exists() and time.monotonic() < until:
            time.sleep(0.2)
        del self.env["CONGO_TEST_CLOBBER"], self.env["CONGO_TEST_HANG"]
        self.assertFalse((out / "leaf.key").exists())
        self.set_leaf(10)
        self.assertEqual(self.run_app(app), (["leaf"], "20\n"))
        self.assertEqual(self.run_app(app), ([], "20\n"))

    def test_a_test_program_is_compiled_only_when_it_changes(self):
        pkg = self.root / "counter"
        write(pkg / "congo.toml",
              '[package]\nname = "counter"\nversion = "0.1.0"\noutput = "library"\n')
        write(pkg / "src" / "counter.cone",
              "mod counter;\n\npub fn twice(n: i64) i64 {\n  n * 2i64\n}\n")
        test = ("mod {name};\n\nimport stdio;\nimport counter;\n\nfn main() i32 {{\n"
                "  stdio.print <- counter.twice({n}i64);\n  stdio.print <- \"\\n\";\n"
                "  0i32\n}}\n")
        write(pkg / "tests" / "one.cone", test.format(name="one", n=1))
        write(pkg / "tests" / "one.out", "2\n")
        write(pkg / "tests" / "two.cone", test.format(name="two", n=2))
        write(pkg / "tests" / "two.out", "4\n")
        out = pkg / "build" / "debug"

        def stamps() -> dict[str, int]:
            """When each object was written: only to see whether it was again."""
            return {name: (out / sub / f"{name}{congo.OBJ_EXT}").stat().st_mtime_ns
                    for name, sub in (("counter", "."), ("one", "tests/one"),
                                      ("two", "tests/two"))}

        first = self.congo("test", cwd=pkg)
        # stdio, which the tests import and the package does not, after it
        self.assertEqual(compiled(first), ["libc", "core", "counter", "stdio"])
        self.assertIn("counter: 2 tests: 2 passed, 0 failed", first.stdout)
        before = stamps()
        second = self.congo("test", cwd=pkg)
        self.assertEqual(compiled(second), [])
        self.assertIn("counter: 2 tests: 2 passed, 0 failed", second.stdout)
        self.assertEqual(stamps(), before)
        # One test changed: that test's program alone
        write(pkg / "tests" / "two.cone", test.format(name="two", n=3))
        write(pkg / "tests" / "two.out", "6\n")
        third = self.congo("test", cwd=pkg)
        self.assertIn("counter: 2 tests: 2 passed, 0 failed", third.stdout)
        after = stamps()
        self.assertEqual((after["counter"], after["one"]), (before["counter"], before["one"]))
        self.assertNotEqual(after["two"], before["two"])

    def test_kernels_are_compiled_when_what_they_compile_from_changes(self):
        # kern's kernel calls scale.factor(): kern's object is compiled
        # against scale's include file, but its kernels from scale's source
        pkgs = self.root / "pkgs"
        self.registry(pkgs)
        for name in ("scale", "kern"):
            write(pkgs / name / "congo.toml",
                  f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n'
                  f'targets = ["native", "gpu"]\n')
        factor = "mod scale;\n\npub fn factor() f32 {{\n  {f}\n}}\n"
        write(pkgs / "scale" / "src" / "scale.cone", factor.format(f="0.5"))
        write(pkgs / "kern" / "src" / "kern.cone", """
            mod kern;

            import scale;

            pub fn @compute(64) halve(inv: Invocation, out: &mut Array[f32]) {
              let i = usize.from(inv.globalId[0usize]);
              if i < out.len {
                out[i] = out[i] * scale.factor();
              }
            }
            """)
        kern = pkgs / "kern"
        spv = kern / "build" / "debug" / "kern.spv"
        run = self.congo("build", cwd=kern)
        self.assertEqual(compiled(run), ["libc", "core", "scale", "kern", "kern for the GPU"])
        first = spv.read_bytes()
        self.assertEqual(compiled(self.congo("build", cwd=kern)), [])
        self.assertEqual(entry_points(spv), ["halve"])
        # A body of scale: scale's object, and kern's kernels, which inline it.
        # kern's own object reads scale's include file alone, which is the same
        write(pkgs / "scale" / "src" / "scale.cone", factor.format(f="0.25"))
        self.assertEqual(compiled(self.congo("build", cwd=kern)), ["scale", "kern for the GPU"])
        self.assertNotEqual(spv.read_bytes(), first)
        # The kernels gone, or not the file their compile wrote: they alone
        spv.unlink()
        self.assertEqual(compiled(self.congo("build", cwd=kern)), ["kern for the GPU"])
        spv.write_bytes(spv.read_bytes() + b"\0")
        self.assertEqual(compiled(self.congo("build", cwd=kern)), ["kern for the GPU"])
        self.assertEqual(entry_points(spv), ["halve"])
        self.assertEqual(compiled(self.congo("build", "--no-cache", cwd=kern)),
                         ["libc", "core", "scale", "kern", "kern for the GPU"])


def clean_environment() -> dict[str, str]:
    """This process's environment as a plain console's would be: no Visual
    Studio environment (a test may run in one) and no link.exe on PATH, so
    that Congo takes the environment from vcvars64.bat, or its cache of it.
    PATH holds Windows' own folders and Python's."""
    keep = ("SystemRoot", "SystemDrive", "windir", "ComSpec", "PATHEXT", "TEMP", "TMP",
            "USERPROFILE", "USERNAME", "HOMEDRIVE", "HOMEPATH", "APPDATA", "LOCALAPPDATA",
            "ProgramData", "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432",
            "CommonProgramFiles", "CommonProgramFiles(x86)", "NUMBER_OF_PROCESSORS",
            "PROCESSOR_ARCHITECTURE", "OS")
    env = {k: os.environ[k] for k in keep if k in os.environ}
    root = env.get("SystemRoot", r"C:\Windows")
    env["PATH"] = ";".join([str(Path(sys.executable).parent), rf"{root}\System32", root,
                            rf"{root}\System32\Wbem",
                            rf"{root}\System32\WindowsPowerShell\v1.0"])
    return env


@unittest.skipUnless(IS_WINDOWS, "vcvars64.bat is Visual Studio's, on Windows")
class MsvcEnvironmentCache(unittest.TestCase):
    """What vcvars64.bat sets, captured once into <Congo home>/msvc-env.json
    and applied to each link's environment with no batch file run; captured
    again for a Visual Studio update, a folder gone, no link.exe, a link that
    fails the way a stale environment does, and --no-cache."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.home = self.root / "home"
        self.saved = os.environ.get("CONGO_HOME")
        os.environ["CONGO_HOME"] = str(self.home)
        self.runs = 0
        real = congo.Linker.run_vcvars

        def counted(base, vcvars=None):
            self.runs += 1
            return real(base, vcvars)

        self.real = real
        congo.Linker.run_vcvars = staticmethod(counted)

    def tearDown(self):
        congo.Linker.run_vcvars = staticmethod(self.real)
        congo.USE_CACHE[0] = True
        if self.saved is None:
            os.environ.pop("CONGO_HOME", None)
        else:
            os.environ["CONGO_HOME"] = self.saved
        self.tmp.cleanup()

    def test_a_template_keeps_the_environment_around_it(self):
        templates = {"LIB": "A;B;<congo:base>", "PATH": "V;<congo:base>;W", "VSCMD_VER": "17"}
        env = congo.MsvcEnvironment.apply(templates, {"Lib": r"C:\sdl3", "Path": "P", "X": "1"})
        # In the base's spelling of each name, which Windows does not tell apart
        self.assertEqual(env, {"Lib": r"A;B;C:\sdl3", "Path": "V;P;W", "X": "1",
                               "VSCMD_VER": "17"})
        self.assertEqual(congo.MsvcEnvironment.apply(templates, {"Path": "P"})["LIB"], "A;B")

    def test_the_key_follows_visual_studio(self):
        install = self.root / "VS"
        build = install / "VC" / "Auxiliary" / "Build"
        write(build / "vcvars64.bat", '@call "%~dp0vcvarsall.bat" x64 %*\n')
        write(build / "vcvarsall.bat", "@rem vcvarsall\n")
        write(build / "Microsoft.VCToolsVersion.default.txt", "14.42.34433\n")
        write(install / "Common7" / "Tools" / "VsDevCmd.bat", "@rem VsDevCmd\n")
        vcvars = str(build / "vcvars64.bat")
        key = congo.MsvcEnvironment.key
        first = key(vcvars)
        self.assertEqual(key(vcvars), first)
        # An update of the tools, or of any batch file that sets them
        for file, text in (("Microsoft.VCToolsVersion.default.txt", "14.44.35207\n"),
                           ("vcvarsall.bat", "@rem vcvarsall, updated\n"),
                           ("vcvars64.bat", '@call "%~dp0vcvarsall.bat" x64 %* \n')):
            before = key(vcvars)
            write(build / file, text)
            self.assertNotEqual(key(vcvars), before, file)
        before = key(vcvars)
        write(install / "Common7" / "Tools" / "VsDevCmd.bat", "@rem VsDevCmd, updated\n")
        self.assertNotEqual(key(vcvars), before)
        # Another installation, the same files
        other = self.root / "VS2"
        shutil.copytree(install, other)
        self.assertNotEqual(key(str(other / "VC" / "Auxiliary" / "Build" / "vcvars64.bat")),
                            key(vcvars))

    def linker(self, **base) -> congo.Linker:
        return congo.Linker(congo.conestd_path(congo.REPO_CONEC), {**clean_environment(), **base})

    def made(self) -> tuple[int, bool]:
        """A Linker made: how many runs of vcvars64.bat there have been since
        the test began, and whether its environment came from the cache."""
        linker = self.linker()
        return self.runs, linker.cached

    def edit(self, change) -> None:
        file = congo.MsvcEnvironment.path()
        data = json.loads(file.read_text(encoding="utf-8"))
        change(data)
        file.write_text(json.dumps(data), encoding="utf-8")

    def test_the_environment_is_captured_once_and_captured_again_when_stale(self):
        first = self.linker()
        self.assertEqual((self.runs, first.cached), (1, False))
        self.assertTrue(congo.MsvcEnvironment.path().is_file())
        self.assertTrue(first.tool.lower().endswith("link.exe"))
        # Kept: no run, and what the environment lists kept behind Visual
        # Studio's folders, as a run would keep it
        again = self.linker(LIB=r"C:\mine")
        self.assertEqual((self.runs, again.cached), (1, True))
        self.assertEqual(again.tool, first.tool)
        lib = congo.env_value(again.env, "LIB")
        self.assertTrue(lib.endswith(r";C:\mine"), lib)
        self.assertEqual(lib[:-len(r";C:\mine")], congo.env_value(first.env, "LIB"))
        self.assertEqual(congo.env_value(again.env, "VSCMD_ARG_TGT_ARCH"), "x64")
        # Another key, as a Visual Studio update makes: captured again
        self.edit(lambda data: data.update(key="0" * 64))
        self.assertEqual(self.made(), (2, False))
        self.assertEqual(self.linker().cached, True)
        # A folder LIB names is gone: captured again
        self.edit(lambda data: data["variables"].update(
            LIB=r"C:\congo-no-such-folder;" + data["variables"]["LIB"]))
        self.assertEqual(self.made(), (3, False))
        # No link.exe in it: captured again, once
        self.edit(lambda data: data["variables"].update(PATH="<congo:base>"))
        recaught = self.linker()
        self.assertEqual((self.runs, recaught.cached, recaught.recaptured), (4, True, True))
        self.assertTrue(recaught.tool.lower().endswith("link.exe"))
        self.assertEqual(self.linker().cached, True)
        # --no-cache: captured again
        congo.USE_CACHE[0] = False
        self.assertEqual(self.made(), (5, False))

    def test_a_link_that_fails_for_the_environment_captures_it_again(self):
        env = {**clean_environment(), "CONGO_HOME": str(self.home)}

        def run(pkg: Path, ok: bool = True) -> subprocess.CompletedProcess:
            done = subprocess.run([*CONGO, "run"], cwd=pkg, env=env, capture_output=True,
                                  text=True)
            if ok:
                self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            return done

        hello = self.root / "hello"
        write(hello / "congo.toml",
              '[package]\nname = "hello"\nversion = "0.1.0"\noutput = "executable"\n')
        write(hello / "src" / "hello.cone", """
            mod hello;

            import stdio;

            fn main() i32 {
              stdio.print <- "hello\\n";
              0i32
            }
            """)
        file = congo.MsvcEnvironment.path()
        self.assertIn("hello\n", run(hello).stdout)
        self.assertTrue(file.is_file())
        stamp = file.stat().st_mtime_ns
        self.assertIn("hello\n", run(hello).stdout)
        self.assertEqual(file.stat().st_mtime_ns, stamp, "captured again with nothing stale")
        # LIB without Visual Studio's and the SDK's folders: link.exe cannot
        # open the C runtime's libraries, which no package names, so the
        # environment is captured again and the link done again
        self.edit(lambda data: data["variables"].update(LIB="<congo:base>"))
        self.assertIn("hello\n", run(hello).stdout)
        self.assertIn(r"\lib\x64", json.loads(file.read_text())["variables"]["LIB"])
        # A library a package names missing is the build's own fault: no
        # capture, and the link's error
        stamp = file.stat().st_mtime_ns
        needy = self.root / "needy"
        write(needy / "congo.toml",
              '[package]\nname = "needy"\nversion = "0.1.0"\noutput = "executable"\n\n'
              '[link]\nlibraries = ["congonosuchlib"]\n')
        write(needy / "src" / "needy.cone", "mod needy;\n\nfn main() i32 {\n  0i32\n}\n")
        failed = run(needy, ok=False)
        self.assertEqual(failed.returncode, 1)
        self.assertIn("congonosuchlib.lib", failed.stderr)
        self.assertNotIn("linked twice", failed.stderr)
        self.assertEqual(file.stat().st_mtime_ns, stamp)


# The Cone Congo, built here once, for the classes below. The default run of
# this file drives congo.py alone, so a Cone Congo that lost a feature of
# congo.py (the build cache, the kept Visual Studio environment, the skipped
# test), or no longer compiled (A5 and A6, 7 Oct), went unseen until someone
# ran the whole file with CONGO_EXE set: these classes build it and run it
# over the scenarios where it once fell behind. Set CONGO_EXE and they stand
# aside: every scenario of the file already drives the Cone Congo then.
_CONE_CONGO: list[Path] = []


def built_cone_congo() -> Path:
    """tools/congo/build/debug/congo.exe, built by congo.py (which skips what
    its cache holds, so a second call costs seconds). A build that fails is an
    error: it is the thing under test."""
    if CONGO_EXE:
        raise unittest.SkipTest("CONGO_EXE drives every scenario against the Cone Congo")
    if not IS_WINDOWS:
        raise unittest.SkipTest("the Cone Congo links on Windows only, for now")
    if not congo.REPO_CONEC.is_file():
        raise unittest.SkipTest(f"no conec at {congo.REPO_CONEC}: python test/run.py --build")
    if not _CONE_CONGO:
        built = subprocess.run([sys.executable, str(HERE / "congo.py"), "build"], cwd=HERE,
                               capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(f"the Cone Congo does not build:\n{built.stdout}{built.stderr}")
        _CONE_CONGO.append(HERE / "build" / "debug" / "congo.exe")
    exe = _CONE_CONGO[0]
    assert exe.is_file(), f"congo.py build wrote no {exe}"
    return exe


class ConeCongoParity(unittest.TestCase):
    """The Cone Congo run where it fell behind congo.py (Bug 3, 30 Oct 2026):
    the build cache (a second build compiles nothing, and a changed input
    compiles what reads it), the cache of a test program, and a test that
    skips itself (status 77). The scenarios are those of BuildCache and
    Testing, driven through the Cone Congo."""

    @classmethod
    def setUpClass(cls):
        global CONGO
        exe = built_cone_congo()
        cls.saved = CONGO
        CONGO = [str(exe)]

    @classmethod
    def tearDownClass(cls):
        global CONGO
        CONGO = cls.saved

    setUp = Scenarios.setUp
    tearDown = Scenarios.tearDown
    congo = Scenarios.congo
    program_output = Scenarios.program_output
    registry = Scenarios.registry
    LEAF = BuildCache.LEAF
    ALL = BuildCache.ALL
    chain = BuildCache.chain
    set_leaf = BuildCache.set_leaf
    set_app = BuildCache.set_app
    run_app = BuildCache.run_app
    test_only_what_reads_a_change_is_compiled_again = (
        BuildCache.test_only_what_reads_a_change_is_compiled_again)
    test_a_test_program_is_compiled_only_when_it_changes = (
        BuildCache.test_a_test_program_is_compiled_only_when_it_changes)
    test_a_test_that_skips_itself = Testing.test_a_test_that_skips_itself


@unittest.skipUnless(IS_WINDOWS, "vcvars64.bat is Visual Studio's, on Windows")
class ConeCongoMsvcEnvironment(unittest.TestCase):
    """The Cone Congo keeps the environment vcvars64.bat sets in the Congo home
    (msvc-env.json, the file congo.py writes), and captures it again for a
    link that fails the way a stale environment makes one fail."""

    @classmethod
    def setUpClass(cls):
        global CONGO
        exe = built_cone_congo()
        cls.saved = CONGO
        CONGO = [str(exe)]

    @classmethod
    def tearDownClass(cls):
        global CONGO
        CONGO = cls.saved

    setUp = MsvcEnvironmentCache.setUp
    tearDown = MsvcEnvironmentCache.tearDown
    edit = MsvcEnvironmentCache.edit
    test_a_link_that_fails_for_the_environment_captures_it_again = (
        MsvcEnvironmentCache.test_a_link_that_fails_for_the_environment_captures_it_again)


@unittest.skipUnless(shutil.which("git"), "Congo fetches packages with git")
class IndexRegistries(unittest.TestCase):
    """A package fetched by name from an index registry: a git repository of one
    TOML file per package, each naming a repository, a commit and a folder. No
    network: the index and the source repositories are bare repositories on this
    disk, reached by file:// URLs."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        # Hermetic git: no user or system config (credential helpers, autocrlf), and an
        # author for the commits Congo and the tests make
        (self.root / "gitconfig").write_text("")
        self.env = dict(os.environ, CONGO_HOME=str(self.root / "home"),
                        GIT_CONFIG_GLOBAL=str(self.root / "gitconfig"), GIT_CONFIG_NOSYSTEM="1",
                        GIT_AUTHOR_NAME="T", GIT_AUTHOR_EMAIL="t@example.com",
                        GIT_COMMITTER_NAME="T", GIT_COMMITTER_EMAIL="t@example.com")
        self.env.pop("CONEC", None)

    def tearDown(self):
        self.tmp.cleanup()

    congo = Scenarios.congo
    program_output = Scenarios.program_output

    def git(self, *args: str, cwd: Path) -> str:
        run = subprocess.run(["git", *args], cwd=cwd, env=self.env, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, f"git {' '.join(args)}: {run.stderr}")
        return run.stdout.strip()

    def repo(self, name: str) -> tuple[Path, str]:
        """A working repository and its bare origin: (the working folder, the
        origin's file:// URL)."""
        bare = self.root / "origins" / f"{name}.git"
        bare.mkdir(parents=True)
        self.git("init", "--quiet", "--bare", "-b", "main", cwd=bare)
        work = self.root / "work" / name
        work.mkdir(parents=True)
        self.git("init", "--quiet", "-b", "main", cwd=work)
        self.git("remote", "add", "origin", bare.as_uri(), cwd=work)
        return work, bare.as_uri()

    def commit(self, work: Path, message: str) -> str:
        """Commit everything, push it, and answer the commit's hash."""
        self.git("add", "-A", cwd=work)
        self.git("commit", "--quiet", "-m", message, cwd=work)
        self.git("push", "--quiet", "origin", "HEAD:main", cwd=work)
        return self.git("rev-parse", "HEAD", cwd=work)

    def library(self, folder: Path, name: str, body: str, imports: str = "") -> None:
        write(folder / "congo.toml",
              f'[package]\nname = "{name}"\nversion = "0.1.0"\noutput = "library"\n')
        write(folder / "src" / f"{name}.cone", f"mod {name};\n\n{imports}\n{body}\n")

    def answer(self, value: int) -> str:
        return f"pub fn answer() i64 {{\n  {value}i64\n}}\n"

    def index(self, name: str = "index") -> tuple[Path, str]:
        work, url = self.repo(name)
        write(work / "README", "an index\n")
        self.commit(work, "start")
        return work, url

    def entry(self, index: Path, name: str, url: str, commit: str, path: str = "") -> None:
        write(index / f"{name}.toml", congo.index_entry_text(name, url, commit, path))

    def config(self, folders: list[Path] = (), indexes: list[str] = ()) -> None:
        listed = ", ".join(f'"{f.as_posix()}"' for f in folders)
        urls = ", ".join(f'"{u}"' for u in indexes)
        write(self.root / "home" / "config.toml",
              f"[registry]\nfolders = [{listed}]\nindexes = [{urls}]\n")

    def app(self, *names: str) -> Path:
        imports = "".join(f"import {n};\n" for n in names)
        prints = "".join(f'  stdio.print <- {n}.answer();\n  stdio.print <- "\\n";\n' for n in names)
        write(self.root / "app.cone", f"mod app;\n\nimport stdio;\n{imports}\n"
                                      f"fn main() i32 {{\n{prints}  0i32\n}}\n")
        return self.root / "app.cone"

    def test_a_package_is_fetched_by_name_at_its_pinned_commit(self):
        # Two packages in one repository, in folders, and a third at another's root
        src, src_url = self.repo("src")
        self.library(src / "pkgs" / "alpha", "alpha", self.answer(11))
        self.library(src / "pkgs" / "beta", "beta", self.answer(22))
        first = self.commit(src, "first")
        solo, solo_url = self.repo("solo")
        self.library(solo, "gamma", self.answer(33))
        gamma = self.commit(solo, "gamma")
        index, index_url = self.index()
        self.entry(index, "alpha", src_url, first, "pkgs/alpha")
        self.entry(index, "beta", src_url, first, "pkgs/beta")
        self.entry(index, "gamma", solo_url, gamma)
        self.commit(index, "entries")
        self.config(indexes=[index_url])
        self.app("alpha", "beta", "gamma")
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "11\n22\n33\n")
        compiled = [line.split()[1] for line in run.stdout.splitlines()
                    if line.strip().startswith("Compiling")]
        self.assertEqual(compiled, ["libc", "core", "stdio", "alpha", "beta", "gamma", "app"])
        # The registry was cloned once, and alpha and beta share one checkout of src
        self.assertEqual(run.stdout.count("Updating registry"), 1)
        self.assertEqual(len(re.findall(r"Downloading ", run.stdout)), 2)
        checkouts = sorted(p.parent.parent.name[:3]
                           for p in (self.root / "home" / "src").glob("*/*/.git"))
        self.assertEqual(checkouts, ["sol", "src"])
        self.assertTrue((self.root / "home" / "src" / congo.url_slug(src_url) / first).is_dir())
        # A second run looks at the index again, for its pins, and fetches nothing
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "11\n22\n33\n")
        self.assertNotIn("Downloading", run.stdout)
        self.assertNotIn("Updating registry", run.stdout)

    def test_a_push_to_the_source_changes_nothing_until_publish_moves_the_pin(self):
        src, src_url = self.repo("src")
        self.library(src, "alpha", self.answer(11))
        first = self.commit(src, "first")
        index, index_url = self.index()
        self.entry(index, "alpha", src_url, first)
        self.commit(index, "entry")
        self.config(indexes=[index_url])
        self.app("alpha")
        self.assertEqual(self.program_output(self.congo("run", "app.cone", cwd=self.root)), "11\n")
        # The author changes the package and pushes: consumers still build the pinned commit
        write(src / "src" / "alpha.cone", "mod alpha;\n\n" + self.answer(99))
        second = self.commit(src, "second")
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "11\n")
        self.assertNotIn("Downloading", run.stdout)
        # 'congo publish', run in the package's own repository, moves the pin
        run = self.congo("publish", cwd=src)
        self.assertIn(f"alpha at {second[:10]}", run.stdout)
        cache = self.root / "home" / "registry" / congo.url_slug(index_url)
        self.assertEqual(self.git("show", "HEAD:alpha.toml", cwd=cache).splitlines()[2],
                         f'commit = "{second}"')
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "99\n")
        self.assertIn("Downloading alpha", run.stdout)
        # Publishing again moves nothing
        self.assertIn("Unchanged", self.congo("publish", cwd=src).stdout)
        # The index repository itself holds the new pin, whoever clones it
        fresh = self.root / "fresh"
        self.git("clone", "--quiet", index_url, str(fresh), cwd=self.root)
        self.assertEqual(congo.tomllib.loads((fresh / "alpha.toml").read_text())["commit"], second)

    def test_a_fetched_package_imports_another_by_name(self):
        src, src_url = self.repo("src")
        self.library(src / "a", "alpha", "pub fn answer() i64 {\n  beta.answer() + 1i64\n}\n",
                     imports="import beta;\n")
        self.library(src / "b", "beta", self.answer(40))
        commit = self.commit(src, "both")
        index, index_url = self.index()
        self.entry(index, "alpha", src_url, commit, "a")
        self.entry(index, "beta", src_url, commit, "b")
        self.commit(index, "entries")
        self.config(indexes=[index_url])
        self.app("alpha")
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "41\n")

    def test_a_folder_registry_is_searched_before_an_index_and_an_index_before_the_next(self):
        src, src_url = self.repo("src")
        self.library(src / "a", "alpha", self.answer(1))
        self.library(src / "b", "beta", self.answer(2))
        self.library(src / "c", "gamma", self.answer(3))
        self.library(src / "other", "gamma", self.answer(30))
        commit = self.commit(src, "all")
        first, first_url = self.index("first")
        second, second_url = self.index("second")
        self.entry(first, "beta", src_url, commit, "b")
        self.entry(first, "gamma", src_url, commit, "c")
        self.entry(second, "alpha", src_url, commit, "a")
        self.entry(second, "gamma", src_url, commit, "other")
        self.commit(first, "entries")
        self.commit(second, "entries")
        local = self.root / "local"
        self.library(local / "beta", "beta", self.answer(200))
        self.config(folders=[local], indexes=[first_url, second_url])
        self.app("alpha", "beta", "gamma")
        run = self.congo("run", "app.cone", cwd=self.root)
        # alpha only the second index has; beta the local folder wins; gamma the first index wins
        self.assertEqual(self.program_output(run), "1\n200\n3\n")

    def test_a_name_no_registry_has(self):
        index, index_url = self.index()
        self.config(indexes=[index_url])
        self.app("nosuch")
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertEqual(run.returncode, 1)
        self.assertIn("import nosuch: no package named 'nosuch' in the registries searched:",
                      run.stderr)
        self.assertIn(index_url, run.stderr)

    def test_an_unusable_entry_or_source_is_refused(self):
        src, src_url = self.repo("src")
        self.library(src, "alpha", self.answer(11))
        commit = self.commit(src, "first")
        index, index_url = self.index()
        self.config(indexes=[index_url])
        self.app("alpha")
        cases = {
            "commit must be a full commit hash": ("alpha", src_url, "main", ""),
            "names the package 'other'": ("other", src_url, commit, ""),
            "not an option or a transport helper": ("alpha", "ext::sh -c true", commit, ""),
            "path must be a folder inside the repository": ("alpha", src_url, commit, "../x"),
            "has no congo.toml in 'nowhere'": ("alpha", src_url, commit, "nowhere"),
            "is the commit in that repository?": ("alpha", src_url, "0" * 40, ""),
        }
        for message, (name, url, pin, path) in cases.items():
            write(index / "alpha.toml", congo.index_entry_text(name, url, pin, path))
            self.commit(index, message)
            run = self.congo("run", "app.cone", cwd=self.root, ok=False)
            self.assertEqual(run.returncode, 1, message)
            self.assertIn(message, run.stderr)
        # A stray key is its own error
        write(index / "alpha.toml", congo.index_entry_text("alpha", src_url, commit, "")
              + 'version = "1"\n')
        self.commit(index, "stray key")
        run = self.congo("run", "app.cone", cwd=self.root, ok=False)
        self.assertIn("'version' is not an index key", run.stderr)

    def test_an_index_that_cannot_be_reached_falls_back_to_the_copy_fetched_before(self):
        src, src_url = self.repo("src")
        self.library(src, "alpha", self.answer(11))
        commit = self.commit(src, "first")
        index, index_url = self.index()
        self.entry(index, "alpha", src_url, commit)
        self.commit(index, "entry")
        self.config(indexes=[index_url])
        self.app("alpha")
        self.congo("run", "app.cone", cwd=self.root)
        # The index is gone: its copy from before is used, and so is the fetched source
        congo.remove_tree(self.root / "origins" / "index.git")
        congo.remove_tree(self.root / "origins" / "src.git")
        run = self.congo("run", "app.cone", cwd=self.root)
        self.assertEqual(self.program_output(run), "11\n")
        self.assertIn("warning: cannot update the registry", run.stderr)

    def test_publish_refuses_what_a_consumer_could_not_fetch(self):
        src, src_url = self.repo("src")
        self.library(src, "alpha", self.answer(11))
        self.git("add", "-A", cwd=src)
        self.git("commit", "--quiet", "-m", "local only", cwd=src)
        index, index_url = self.index()
        # No index configured
        run = self.congo("publish", cwd=src, ok=False)
        self.assertIn("no index registry to publish to", run.stderr)
        self.config(indexes=[index_url])
        # A commit that is not pushed
        run = self.congo("publish", cwd=src, ok=False)
        self.assertIn("is on no remote branch", run.stderr)
        self.git("push", "--quiet", "origin", "HEAD:main", cwd=src)
        # Changes not committed
        write(src / "src" / "alpha.cone", "mod alpha;\n\n" + self.answer(12))
        run = self.congo("publish", cwd=src, ok=False)
        self.assertIn("has changes that are not committed", run.stderr)
        # --registry names the index when the config lists none
        self.git("checkout", "--quiet", "--", ".", cwd=src)
        self.config()
        run = self.congo("publish", "--registry", index_url, cwd=src)
        self.assertIn("Published", run.stdout)
        # Outside any git repository
        loose = self.root / "loose"
        self.library(loose, "delta", self.answer(1))
        run = self.congo("publish", "--registry", index_url, cwd=loose, ok=False)
        self.assertIn("is not in a git repository", run.stderr)

    def test_publish_records_the_folder_of_a_package_inside_a_repository(self):
        src, src_url = self.repo("src")
        self.library(src / "pkgs" / "alpha", "alpha", self.answer(11))
        commit = self.commit(src, "first")
        index, index_url = self.index()
        self.config(indexes=[index_url])
        self.congo("publish", cwd=src / "pkgs" / "alpha")
        fresh = self.root / "fresh"
        self.git("clone", "--quiet", index_url, str(fresh), cwd=self.root)
        self.assertEqual((fresh / "alpha.toml").read_text(),
                         congo.index_entry_text("alpha", src_url, commit, "pkgs/alpha"))
        self.app("alpha")
        self.assertEqual(self.program_output(self.congo("run", "app.cone", cwd=self.root)), "11\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
