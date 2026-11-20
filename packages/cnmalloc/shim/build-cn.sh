#!/bin/bash
# build-cn.sh: Linux's build-cn.ps1 (row 550). Builds the cnmalloc link shim:
#   cn_shim.o    the shim, for programs that link the second design's tests' hooks off (the library that ships)
#   cn_shim_d.o  the same source with -DCN_HOOKS -DCN_DEBUG: the second design's test hooks, and a double free that aborts
#   libcnpkg.a   the Cone package's release object (cnmalloc.o), as an archive: it supplies cnm_* to a program that does not
#                import the package itself, and is not pulled by one that does (that program's own objects define them)
#   bash packages/cnmalloc/shim/build-cn.sh [outdir]       (default /tmp/cnmalloc-alloc)
# Then link a program with it: CONGO_LINK_EXTRA="<outdir>/cn_shim.o <outdir>/libcnpkg.a" congo build ...
# The shim is a plain OBJECT on the executable's link line, never a member of an archive (an archive member is pulled only
# for a symbol the program references by name, and a program need not name malloc) and never a shared library: its
# thread-locals are initial-exec, so that no access calls __tls_get_addr, which allocates. Flags: -fno-builtin-malloc and
# friends so that the compiler does not fold the shim's own calls to malloc and free (it is the definition of them).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
pkg="$(dirname "$here")"
tree="$(dirname "$(dirname "$pkg")")"
out="${1:-/tmp/cnmalloc-alloc}"
mkdir -p "$out"
(cd "$pkg" && python3 "$tree/tools/congo/congo.py" build --release)
obj="$pkg/build/release/cnmalloc.o"
[ -f "$obj" ] || { echo "no $obj"; exit 1; }
CF="${CC_FLAGS:--O2 -fPIC -ftls-model=initial-exec -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -pthread -Wall -Wno-unused-function}"
cc $CF -c "$here/cn_shim.c" -o "$out/cn_shim.o"
cc $CF -DCN_HOOKS -DCN_DEBUG -c "$here/cn_shim.c" -o "$out/cn_shim_d.o"
rm -f "$out/libcnpkg.a"
ar rcs "$out/libcnpkg.a" "$obj"
ls -l "$out"
