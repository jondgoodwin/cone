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

	src/c-compiler/shared/options.c
	src/c-compiler/shared/error.c
	src/c-compiler/shared/fileio.c
	src/c-compiler/shared/memory.c
	src/c-compiler/shared/utf8.c

	src/c-compiler/ast/ast.c
	src/c-compiler/ast/nametbl.c
	src/c-compiler/ast/module.c
	src/c-compiler/ast/nameuse.c
	src/c-compiler/ast/vardcl.c
	src/c-compiler/ast/literal.c
	src/c-compiler/ast/nodes.c
	src/c-compiler/ast/block.c
	src/c-compiler/ast/expr.c
	src/c-compiler/ast/copyexpr.c

	src/c-compiler/std/stdlib.c
	src/c-compiler/std/stdnumber.c

	src/c-compiler/types/type.c
	src/c-compiler/types/fnsig.c
	src/c-compiler/types/pointer.c
	src/c-compiler/types/struct.c
	src/c-compiler/types/array.c
	src/c-compiler/types/number.c
	src/c-compiler/types/permission.c
	src/c-compiler/types/alloc.c

	src/c-compiler/parser/lexer.c
	src/c-compiler/parser/parser.c
	src/c-compiler/parser/parseflow.c
	src/c-compiler/parser/parseexpr.c
	src/c-compiler/parser/parsetype.c

	src/c-compiler/genllvm/genllvm.c
	src/c-compiler/genllvm/genlstmt.c
	src/c-compiler/genllvm/genlexpr.c
)

target_link_libraries(conec "${LLVM_LIB}")

add_library(conestd
	src/conestd/stdio.c
)
