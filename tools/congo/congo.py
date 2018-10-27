#!/usr/bin/env python3

# Build tool for a Cone program

import os
import sys
from subprocess import call

conehome = os.environ['CONEHOME']

# Operating system-specific configuration
if sys.platform.startswith('linux'):
  osname = 'linux'
  cpu = 'x64'
  objext = '.o'
  exeext = ''
elif sys.platform.startswith('win'):
  osname = 'win'
  cpu = 'x86'
  objext = '.obj'
  exeext = '.exe'
  mstoolbin = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Community\\VC\\Tools\\MSVC\\14.14.26428\\bin\\Hostx86\\x86\\'
  msucrtlib = 'C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.17134.0\\ucrt\\x86\\ucrt.lib'
  msvclib = '/LIBPATH:C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Community\\VC\\Tools\\MSVC\\14.14.26428\\lib\\x86\\'
  msumlib = '/LIBPATH:C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.17134.0\\um\\x86\\'
else:
   print("Unsupported operating system")
   exit(1)

# Obtain project name
sep = '\\' if osname == 'win' else '/'
project = os.getcwd().split(sep)[-1]

# Determine desired build configuration
if len(sys.argv) > 1:
  config = sys.argv[1]
else:
  config = 'release'


def link():
  obj = builddir + "/main.obj"
  ourlib = "../../packages/conert/release/conert.lib"
  tolink = [mstoolbin+"link", msucrtlib, msvclib, msumlib, obj, ourlib, "/LTCG", "/OUT:"+exename]
  # + "msvcrt.lib oldnames.lib kernel32.lib vcruntime.lib"
  call(tolink)


# Establish build directory and ensure it exists
if config == 'release':
  builddir = 'release'
elif config == 'debug':
  builddir = 'debug'
elif config == 'web':
  builddir = 'public'
else:
  print("Unknown configuration: " + config)
  exit(1)
if not os.path.exists(builddir):
  os.mkdir(builddir)

pgm = 'main'
exename = os.path.join(builddir, project + exeext)
objlist = []

# Compile Cone program
if config == 'release':
  compopt = ""
elif config == 'debug':
  compopt = "-d --ir --asm --llvmir"
elif config == 'web':
  compopt = "--wasm"
call("conec " + compopt + " -o"+builddir + " "+pgm)
objlist.append(os.path.join(builddir, pgm + objext))
print(objlist)

if not config == 'web':
  link()

# Compile C
# clang-cl /c /W3 /analyze- /Gy /O2 /sdl /Zc:inline /D "_MBCS" /WX- /Zc:forScope /Gd /Oy- /Oi /MD /FC /Fa"Release\\" /EHsc /nologo /Fo"public\\" /diagnostics:classic \cone\packages\conert\main.c
# llvm-lib main.obj  (llvm-ar rc main.a main.o)
# link /MANIFEST /NXCOMPAT /PDB:"D:\sol\Cone\Debug\ConeTest.pdb" /DYNAMICBASE "D:\sol\Cone\Debug\Conestd.lib" "D:\sol\Cone\Debug\test.obj" "kernel32.lib" "user32.lib" "gdi32.lib" "winspool.lib" "comdlg32.lib" "advapi32.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "uuid.lib" "odbc32.lib" "odbccp32.lib" /DEBUG:FASTLINK /MACHINE:X86 /INCREMENTAL /PGD:"D:\sol\Cone\Debug\ConeTest.pgd" /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /ManifestFile:"Debug\ConeTest.exe.intermediate.manifest" /ERRORREPORT:PROMPT /NOLOGO /TLBID:1 