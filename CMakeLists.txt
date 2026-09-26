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
	compiler/c/ir/flowloan.c
	compiler/c/ir/flowpath.c
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
	compiler/c/ir/exp/block.c
	compiler/c/ir/exp/borrow.c
	compiler/c/ir/exp/cast.c
	compiler/c/ir/exp/deref.c
	compiler/c/ir/exp/fncall.c
	compiler/c/ir/exp/if.c
	compiler/c/ir/exp/literal.c
	compiler/c/ir/exp/logic.c
	compiler/c/ir/exp/namedval.c
	compiler/c/ir/exp/nameuse.c
	compiler/c/ir/exp/sizeof.c
	compiler/c/ir/exp/typelit.c
	compiler/c/ir/exp/vtuple.c

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
	compiler/c/genllvm/genltype.c
)

target_link_libraries(conec ${llvm_libs})

# The fallback packages folder, where core and stdio are: this repository's
# own. conec first takes the CONE_PACKAGES environment variable, then the
# nearest packages folder at or above its own executable's folder (which, from
# build/<config>/, is this same one); this is used only when both are absent.
target_compile_definitions(conec PRIVATE CONE_PACKAGES_DIR="${CMAKE_SOURCE_DIR}/packages/")

add_library(conestd
	packages/conestd/stdio.c
)
