cmake_minimum_required(VERSION 2.8)
project(cone)
set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/bin)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})

include_directories("${CMAKE_SOURCE_DIR}/src/c-compiler/")

add_executable(cone 
	src/c-compiler/cone.c
	src/c-compiler/shared/ast.c
	src/c-compiler/shared/error.c
	src/c-compiler/shared/fileio.c
	src/c-compiler/shared/memory.c
	src/c-compiler/shared/symbol.c
	src/c-compiler/shared/type.c
	src/c-compiler/parser/lexer.c
	src/c-compiler/parser/parser.c
	src/c-compiler/genllvm/genllvm.c
)


