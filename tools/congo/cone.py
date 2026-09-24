#!/usr/bin/env python3

#

import os
from subprocess import call

conehome = os.environ['CONEHOME']
os.chdir(conehome)

if (not os.path.exists(os.path.join(conehome,"conec"))):
    call("git clone https://github.com/jondgoodwin/cone.git conec")
else:
    os.chdir("conec")
    call("git pull --rebase")

os.chdir(os.path.join(conehome,"conec"))
call("cmake .")
call("make")
