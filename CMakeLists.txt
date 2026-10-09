cmake_minimum_required(VERSION 2.8)
project(conec)

# LLVM's package version check matches major.minor, so name both.
find_package(LLVM 23.1 REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

include_directories(
    "${CMAKE_SOURCE_DIR}/compiler/c/"
)

set(LLVM_LINK_COMPONENTS
		Analysis
		BitReader
		Core
		InstCombine
		MC
		MCDisassembler
		Object
		Passes
		ScalarOpts
		Support
		Target
		TransformUtils
		native
		nativecodegen
		AsmPrinter
		# Link every target backend included in the installed LLVM build.
		AllTargetsCodeGens
		AllTargetsAsmParsers
		AllTargetsDescs
		AllTargetsDisassemblers
		AllTargetsInfos
		)

llvm_map_components_to_libnames(llvm_libs support core irreader ${LLVM_LINK_COMPONENTS})


add_executable(conec 
	compiler/c/conec.c
	compiler/c/coneopts.c

	compiler/c/shared/error.c
	compiler/c/shared/fileio.c
	compiler/c/shared/memory.c
	compiler/c/shared/options.c
	compiler/c/shared/timer.c
	compiler/c/shared/utf8.c

	compiler/c/ir/checktree.c
	compiler/c/ir/clone.c
	compiler/c/ir/dclinfo.c
	compiler/c/ir/dclspan.c
	compiler/c/ir/export.c
	compiler/c/ir/incfile.c
	compiler/c/ir/flow.c
	compiler/c/ir/flowdrop.c
	compiler/c/ir/flowloan.c
	compiler/c/ir/flowpath.c
	compiler/c/ir/shapeinfer.c
	compiler/c/ir/reshape.c
	compiler/c/ir/iexp.c
	compiler/c/ir/inode.c
	compiler/c/ir/instype.c
	compiler/c/ir/itype.c
	compiler/c/ir/name.c
	compiler/c/ir/namespace.c
	compiler/c/ir/nametbl.c
	compiler/c/ir/nodelist.c
	compiler/c/ir/nodes.c
	compiler/c/ir/typetbl.c

	compiler/c/ir/stmt/aliasdcl.c
	compiler/c/ir/stmt/break.c
	compiler/c/ir/stmt/const.c
	compiler/c/ir/stmt/continue.c
	compiler/c/ir/stmt/fielddcl.c
	compiler/c/ir/stmt/fndcl.c
	compiler/c/ir/stmt/fold.c
	compiler/c/ir/stmt/import.c
	compiler/c/ir/stmt/intrinsic.c
	compiler/c/ir/stmt/module.c
	compiler/c/ir/stmt/modtrait.c
	compiler/c/ir/stmt/program.c
	compiler/c/ir/stmt/return.c
	compiler/c/ir/stmt/swap.c
	compiler/c/ir/stmt/vardcl.c

	compiler/c/ir/exp/allocate.c
	compiler/c/ir/exp/arraylit.c
	compiler/c/ir/exp/assign.c
	compiler/c/ir/exp/await.c
	compiler/c/ir/exp/block.c
	compiler/c/ir/exp/borrow.c
	compiler/c/ir/exp/cast.c
	compiler/c/ir/exp/contents.c
	compiler/c/ir/exp/deref.c
	compiler/c/ir/exp/each.c
	compiler/c/ir/exp/fncall.c
	compiler/c/ir/exp/if.c
	compiler/c/ir/exp/literal.c
	compiler/c/ir/exp/logic.c
	compiler/c/ir/exp/namedval.c
	compiler/c/ir/exp/nameuse.c
	compiler/c/ir/exp/sizeof.c
	compiler/c/ir/exp/typelit.c
	compiler/c/ir/exp/vtuple.c
	compiler/c/ir/exp/yield.c

	compiler/c/ir/types/actor.c
	compiler/c/ir/types/array.c
	compiler/c/ir/types/arrayref.c
	compiler/c/ir/types/enum.c
	compiler/c/ir/types/fnsig.c
	compiler/c/ir/types/lifetime.c
	compiler/c/ir/types/number.c
	compiler/c/ir/types/permission.c
	compiler/c/ir/types/pointer.c
	compiler/c/ir/types/reference.c
	compiler/c/ir/types/region.c
	compiler/c/ir/types/struct.c
	compiler/c/ir/types/ttuple.c
	compiler/c/ir/types/void.c

	compiler/c/ir/meta/macro.c
	compiler/c/ir/meta/genvardcl.c
	compiler/c/ir/meta/generic.c

	compiler/c/corelib/corelib.c
	compiler/c/corelib/corenumber.c

	compiler/c/parser/lexer.c
	compiler/c/parser/parseactor.c
	compiler/c/parser/parsegen.c
	compiler/c/parser/parsemod.c
	compiler/c/parser/parsebuild.c
	compiler/c/parser/parsefnflow.c
	compiler/c/parser/parseexpr.c
	compiler/c/parser/parsetype.c
	compiler/c/parser/parsehelper.c

	compiler/c/genllvm/genllvm.c
	compiler/c/genllvm/genlstmt.c
	compiler/c/genllvm/genlexpr.c
	compiler/c/genllvm/genlalloc.c
	compiler/c/genllvm/genlawait.c
	compiler/c/genllvm/genlyield.c
	compiler/c/genllvm/genltype.c
	compiler/c/genllvm/genlcabi.c
	compiler/c/genllvm/genlaggcopy.c
	compiler/c/genllvm/genlgpu.c
	compiler/c/genllvm/genlgpusync.c
)

target_link_libraries(conec ${llvm_libs})

# The test runner counts this file as a compiler source (R1.1), so a change to
# it relinks conec, even one that only changes conestd
set_property(TARGET conec APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/CMakeLists.txt)

# On Windows the manifest makes UTF-8 the process's code page, so that
# conec's arguments, environment and file names arrive, and are opened, in
# the UTF-8 it holds paths in (compiler/c/doc/compiler/architecture.md,
# "Paths are UTF-8")
if(WIN32)
	target_sources(conec PRIVATE compiler/c/conec.manifest)
endif()

# The fallback packages folder, where core and stdio are: this repository's
# own. conec first takes the CONE_PACKAGES environment variable, then the
# nearest packages folder at or above its own executable's folder (which, from
# build/<config>/, is this same one); this is used only when both are absent.
target_compile_definitions(conec PRIVATE CONE_PACKAGES_DIR="${CMAKE_SOURCE_DIR}/packages/")

# conestd, the runtime every program links. Its Cone files are one module,
# which the conec built above compiles into one object; mainthread.c is the
# piece Cone cannot say (it records the starting thread before 'main'). Both
# go into the one library. The object also holds what it uses of core and
# libc, so it is rebuilt when they change.
set(CONESTD_CONE
	${CMAKE_SOURCE_DIR}/packages/conestd/conestd.cone
	${CMAKE_SOURCE_DIR}/packages/conestd/barrier.cone
	${CMAKE_SOURCE_DIR}/packages/conestd/hash.cone
	${CMAKE_SOURCE_DIR}/packages/conestd/panic.cone
	${CMAKE_SOURCE_DIR}/packages/conestd/roots.cone
	${CMAKE_SOURCE_DIR}/packages/conestd/stdio.cone
)
set(CONESTD_OBJDIR ${CMAKE_BINARY_DIR}/conestd-cone)
set(CONESTD_OBJ ${CONESTD_OBJDIR}/conestd${CMAKE_C_OUTPUT_EXTENSION})
add_custom_command(
	OUTPUT ${CONESTD_OBJ}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CONESTD_OBJDIR}
	COMMAND $<TARGET_FILE:conec> --output=${CONESTD_OBJDIR} ${CMAKE_SOURCE_DIR}/packages/conestd/conestd.cone
	DEPENDS conec ${CONESTD_CONE}
		${CMAKE_SOURCE_DIR}/packages/core/src/core.cone
		${CMAKE_SOURCE_DIR}/packages/libc/src/libc.cone
	COMMENT "Compiling conestd's Cone source"
	VERBATIM
)
add_library(conestd STATIC
	packages/conestd/mainthread.c
	${CONESTD_OBJ}
)
