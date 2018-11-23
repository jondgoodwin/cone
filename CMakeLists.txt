cmake_minimum_required(VERSION 2.8)
project(conec)
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/bin)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})

SET (LLVM_INCLUDE "/usr/lib/llvm-5.0/include")
SET (LLVM_LIB     "/usr/lib/llvm-5.0/lib/libLLVM-5.0.so")

include_directories(
    "${CMAKE_SOURCE_DIR}/src/c-compiler/"
    "${LLVM_INCLUDE}"
)

add_executable(conec 
	src/c-compiler/conec.c
	src/c-compiler/coneopts.c

	src/c-compiler/shared/error.c
	src/c-compiler/shared/fileio.c
	src/c-compiler/shared/memory.c
	src/c-compiler/shared/options.c
	src/c-compiler/shared/utf8.c

	src/c-compiler/ir/flow.c
	src/c-compiler/ir/inode.c
	src/c-compiler/ir/imethod.c
	src/c-compiler/ir/iexp.c
	src/c-compiler/ir/itype.c
	src/c-compiler/ir/namespace.c
	src/c-compiler/ir/nametbl.c
	src/c-compiler/ir/nodes.c

	src/c-compiler/ir/stmt/break.c
	src/c-compiler/ir/stmt/fndcl.c
	src/c-compiler/ir/stmt/intrinsic.c
	src/c-compiler/ir/stmt/module.c
	src/c-compiler/ir/stmt/return.c
	src/c-compiler/ir/stmt/vardcl.c
	src/c-compiler/ir/stmt/while.c

	src/c-compiler/ir/exp/addr.c
	src/c-compiler/ir/exp/assign.c
	src/c-compiler/ir/exp/block.c
	src/c-compiler/ir/exp/cast.c
	src/c-compiler/ir/exp/deref.c
	src/c-compiler/ir/exp/fncall.c
	src/c-compiler/ir/exp/if.c
	src/c-compiler/ir/exp/literal.c
	src/c-compiler/ir/exp/logic.c
	src/c-compiler/ir/exp/namedval.c
	src/c-compiler/ir/exp/nameuse.c
	src/c-compiler/ir/exp/typelit.c
	src/c-compiler/ir/exp/vtuple.c
	src/c-compiler/ir/exp/sizeof.c

	src/c-compiler/ir/types/array.c
	src/c-compiler/ir/types/fnsig.c
	src/c-compiler/ir/types/number.c
	src/c-compiler/ir/types/permission.c
	src/c-compiler/ir/types/reference.c
	src/c-compiler/ir/types/pointer.c
	src/c-compiler/ir/types/struct.c
	src/c-compiler/ir/types/void.c
	src/c-compiler/ir/types/ttuple.c

	src/c-compiler/std/stdlib.c
	src/c-compiler/std/stdnumber.c

	src/c-compiler/parser/lexer.c
	src/c-compiler/parser/parser.c
	src/c-compiler/parser/parseflow.c
	src/c-compiler/parser/parseexpr.c
	src/c-compiler/parser/parsetype.c

	src/c-compiler/genllvm/genllvm.c
	src/c-compiler/genllvm/genlstmt.c
	src/c-compiler/genllvm/genlexpr.c
	src/c-compiler/genllvm/genlalloc.c
	src/c-compiler/genllvm/genltype.c
)

target_link_libraries(conec "${LLVM_LIB}")

add_library(conestd
	src/conestd/stdio.c
)
