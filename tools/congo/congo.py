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
elif target == 'debug':
  cone_compile("main.cone", "obj", "-d --ir --asm --llvmir")
elif target == 'wasm':
  cone_compile("main.cone", "public", "--wasm")
else:
  print("Unknown target: "+target)

