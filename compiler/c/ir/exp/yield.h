/** Handling for 'yield' nodes: a generator's seam
 *
 * A function declared 'fn walk(t &Tree) yields &Node' is a generator
 * (parser/parsegen.c). The parser makes it a hidden struct, 'walk.Gen', holding
 * its parameters as fields, with a method 'next(self &mut) Option[&Node]' whose
 * body is the author's, and a constructor, 'walk', that builds the struct. A
 * 'yield e' in that body hands the caller 'Some(e)' as 'next''s result and
 * leaves the body there, a seam: the next call of 'next' resumes just after it.
 * It stands as a statement, since a one-way yield has no value to give back.
 *
 * The split is 'await''s (ir/exp/await.h) with the dispatcher taken out. What
 * stays across the seam -- each local that is used after it or does something as
 * it dies -- lives in the generator's frame, hidden storage that follows the
 * parameters in the struct (genllvm/genlyield.c), instead of in a record moved
 * in and out: the local's storage is the frame's, so the seam moves nothing, and
 * resuming is a jump to the block after the seam. The loan walk applies the
 * seam's rules (flowpath.c, pwYield), noting on the 'yield' what each variable in
 * scope does there (seamvars, the same SeamFlags as 'await''s). Its rules differ
 * from 'await''s in one thing: a borrow the generator was given -- of its
 * parameters, which are the struct's fields, or reached through them -- lasts
 * across the seam, since the generator holds what it was lent; a borrow of the
 * generator's own local does not, and is refused if used after the seam.
 *
 * compiler/c/doc/phases/flow.md, "A yield", and generation.md, "A generator",
 * are the notes.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef yield_h
#define yield_h

// 'yield exp': exp is the generator's 'next' result, 'Some(value)', which the
// parser built
typedef struct YieldNode {
    IExpNodeHdr;
    INode *exp;         // The result handed to the caller: evaluated, then the body is left
    SeamVar *seamvars;  // The variables in scope at the seam, in the order they were declared
    uint32_t nseamvars;
    uint32_t seamcap;
    uint32_t yieldno;   // The generator's seams are numbered from 1 in the order written: the state resumed from
    uint8_t walked;     // The loan walk reached it on some path
} YieldNode;

YieldNode *newYieldNode();

// Clone yield
INode *cloneYieldNode(CloneState *cstate, YieldNode *node);

void yieldPrint(YieldNode *node);

// Name resolution of yield
void yieldNameRes(NameResState *pstate, YieldNode *node);

// Type check yield: it stands only in a generator's body, and its value is
// the result 'next' gives
void yieldTypeCheck(TypeCheckState *pstate, YieldNode *node);

// ---- A generator ----

// A generator, as the parser made it: its step function ('next'), the hidden
// struct holding its parameters and, after them in the LLVM layout, its frame,
// and the function giving 'None' when the body ends. Once the step is checked,
// its seams in the order written, each numbered
typedef struct GenInfo {
    FnDclNode *step;        // The generator's 'next'
    StructNode *gen;        // The hidden struct
    FnDclNode *none;        // The function giving 'None', the result when the body ends
    FnDclNode *ctor;        // The function that makes the generator: the one the author wrote, 'walk'
    Nodes *yields;          // Each seam (YieldNode) the loan walk reached, numbered from 1 in the order written
    FieldDclNode *state;    // The field holding where the body resumes: 0 unstarted, a seam's number, GenDone
    // Generation (genlyield.c): the locals the frame keeps, in the order first
    // met at a seam, and for each how it is kept: GenFrameInline, in the
    // frame; GenFrameBoxed, on the heap, its address in the frame, where a
    // generator of this kind would otherwise hold itself; or GenFrameFlag, a
    // match's binding in the matched variable's storage, which keeps only its
    // drop flag. 'valueat' and 'flagat' are the frame's elements for its value
    // and for its drop flag (-1 for none)
    VarDclNode **frame;
    uint8_t *kind;
    int32_t *valueat;
    int32_t *flagat;
    uint32_t nframe;
    uint32_t nelems;
    void *framellvm;        // The frame's LLVM type (an LLVMTypeRef), once laid out
    int framed;             // 0: not planned; 1: being planned; 2: planned
} GenInfo;

enum GenFrameKind {
    GenFrameInline,
    GenFrameBoxed,
    GenFrameFlag,
};

// The state of a generator whose body has ended
#define GenDone 0xFFFFFFFFu

// Make the record of a generator the parser built, and look one up
GenInfo *yieldGenNew(FnDclNode *step, StructNode *gen, FnDclNode *none);
GenInfo *yieldGenOf(FnDclNode *step);
GenInfo *yieldGenOfStruct(INode *type);
GenInfo *yieldGenOfCtor(FnDclNode *ctor);

// An instance of a generic generator's struct has been cloned from the
// template: the instance is a generator of its own, whose step, 'none' and
// state are the clone's members
void yieldGenCloned(StructNode *template, StructNode *copy);

// A generator's seams, once its step is checked and walked
void yieldSplitRegister(GenInfo *info, Nodes *yields);

// Whether any generator was made: lets generation skip its lookups
int yieldAny();

#endif
