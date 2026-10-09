/** 'parallel each': a loop whose passes run at the same time on the actors' workers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef pareach_h
#define pareach_h

// 'parallel each x in src { body }' is parsed as 'each' is (parseEach): the same
// block marked FlagEach, with FlagParallel beside it. A number range keeps its
// two bounds in two hidden variables, a first and a last, each initialised
// with one bound (FlagParIncl for '<='), and the block has three statements;
// anything else keeps the source in one, and has two.
//
// Type check builds the loop (parallelEachLower, from blockTypeCheck) once it
// knows what the source is. Only a source the compiler can split into index
// ranges is accepted -- an array, a slice, a type lending an array (a list), a
// number range -- and the loop is built over an index range:
//
//   [ the source, held and lent as a slice, or the range's bounds ]
//   imm lo = 0;                 // the range of passes: [lo, hi)
//   imm hi = the source's length   (a range: its count, which generation works out)
//   mut k = lo;                 // the index a piece counts through
//   loop { if k >= hi {break}; imm x = &slice[k]; k++; ...body... }
//
// The statements before 'k' run where the loop is written; 'k' and the loop
// are the PIECE, which generation outlines (genlParallelRun) into a function of
// its own, '(data *u8, lo usize, hi usize)', with 'lo' and 'hi' that piece's.
// The actors package's parallelEach runs it over [0, hi) in pieces; each piece
// finds whatever the loop uses from outside through a record of pointers to
// the variables, which stay where they are in the caller's frame. Variables the
// loop uses from outside are therefore read in place, borrowed for the loop;
// that nothing writes them is checked here (parallelEachCheckBody).
//
// The hidden variables carry names no source can spell, so generation finds
// the parts of the loop by name, whatever statements are hoisted between them.
extern Name *parLoName;     // lo
extern Name *parHiName;     // hi
extern Name *parKName;      // k
extern Name *parFirstName;  // A number range's first bound
extern Name *parLastName;   // and its last

// Make the names, once
void parallelEachNames();

// The actors package's parallelEach, which the loop is run by, once a parallel
// each has found it
extern FnDclNode *parallelEachFn;

// Which of a range's two hidden bound variables is type checked first (0 or 1):
// the one that is not an untyped literal, so that the other takes its type; and
// the second takes the first's type once the first is checked
uint32_t parallelEachBoundsFirst(BlockNode *outer);
void parallelEachBoundType(BlockNode *outer, uint32_t first);

// Build the loop, once the source (and a range's bounds) have been checked
void parallelEachLower(TypeCheckState *pstate, BlockNode *outer);

// Check the body's rules that need its types, once its statements are checked:
// nothing declared outside the loop is written or lent for writing
void parallelEachCheckBody(TypeCheckState *pstate, BlockNode *outer);

#endif
