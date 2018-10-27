#!/usr/bin/env python3

# Build tool for a Cone program

import os
import sys
from subprocess import call

def cone_compile(pgm, objfolder, options):
  # Make sure 'obj' folder exists
  if not os.path.exists(objfolder):
    os.mkdir(objfolder)
  call("conec " + options + " -o"+objfolder + " "+pgm)

def cone_link():
  linkcmd = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Community\\VC\\Tools\\MSVC\\14.14.26428\\bin\\Hostx86\\x86\\link'
  ucrtlib = 'C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.17134.0\\ucrt\\x86\\ucrt.lib'
  msvclib = '/LIBPATH:C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Community\\VC\\Tools\\MSVC\\14.14.26428\\lib\\x86\\'
  umlib = '/LIBPATH:C:\\Program Files (x86)\\Windows Kits\\10\\Lib\\10.0.17134.0\\um\\x86\\'
  obj = "obj/main.obj"
  ourlib = "../../packages/conert/release/conert.lib"
  tolink = [linkcmd, ucrtlib, msvclib, umlib, obj, ourlib, "/LTCG"] 
  # + "msvcrt.lib oldnames.lib kernel32.lib vcruntime.lib"
  call(tolink)


conehome = os.environ['CONEHOME']
#  os.path.exists(os.path.join(conehome,"conec")))

# Get build target
if len(sys.argv) > 1:
  target = sys.argv[1]
else:
  target = 'pgm'

# Call Cone compiler with appropriate options
if target == 'pgm':
  cone_compile("main.cone", "obj", "")
  cone_link()
elif target == 'debug':
  cone_compile("main.cone", "obj", "-d --ir --asm --llvmir")
elif target == 'web':
  cone_compile("main.cone", "public", "--wasm")
else:
  print("Unknown target: "+target)

# Compile C
# clang-cl /c /W3 /analyze- /Gy /O2 /sdl /Zc:inline /D "_MBCS" /WX- /Zc:forScope /Gd /Oy- /Oi /MD /FC /Fa"Release\\" /EHsc /nologo /Fo"public\\" /diagnostics:classic \cone\packages\conert\main.c
# llvm-lib main.obj  (llvm-ar rc main.a main.o)
# link /MANIFEST /NXCOMPAT /PDB:"D:\sol\Cone\Debug\ConeTest.pdb" /DYNAMICBASE "D:\sol\Cone\Debug\Conestd.lib" "D:\sol\Cone\Debug\test.obj" "kernel32.lib" "user32.lib" "gdi32.lib" "winspool.lib" "comdlg32.lib" "advapi32.lib" "shell32.lib" "ole32.lib" "oleaut32.lib" "uuid.lib" "odbc32.lib" "odbccp32.lib" /DEBUG:FASTLINK /MACHINE:X86 /INCREMENTAL /PGD:"D:\sol\Cone\Debug\ConeTest.pgd" /MANIFESTUAC:"level='asInvoker' uiAccess='false'" /ManifestFile:"Debug\ConeTest.exe.intermediate.manifest" /ERRORREPORT:PROMPT /NOLOGO /TLBID:1 