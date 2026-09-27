/** Standard library initialization
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../ir/nametbl.h"
#include "../parser/lexer.h"

#include <string.h>

INode *unknownType;
INode *noCareType;
INode *errorType;
INode *elseCond;
INode *borrowRef;
INode *neverType;
PermNode *uniPerm;
PermNode *mutPerm;
PermNode *immPerm;
PermNode *roPerm;
PermNode *mut1Perm;
PermNode *opaqPerm;
LifetimeNode *staticLifetimeNode;
NbrNode *boolType;
NbrNode *i8Type;
NbrNode *i16Type;
NbrNode *i32Type;
NbrNode *i64Type;
NbrNode *isizeType;
NbrNode *u8Type;
NbrNode *u16Type;
NbrNode *u32Type;
NbrNode *u64Type;
NbrNode *usizeType;
NbrNode *f32Type;
NbrNode *f64Type;
INsTypeNode *ptrType;
INsTypeNode *refType;
INsTypeNode *arrayRefType;

PermNode *newPermNodeStr(char *name, uint16_t flags) {
    Name *namesym = nametblFind(name, strlen(name));
    PermNode *perm = newPermDclNode(namesym, flags);
    namesym->node = (INode*)perm;
    return perm;
}

// Declare built-in permission types and their names
void stdPermInit() {
    uniPerm = newPermNodeStr("uni", MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless);
    mutPerm = newPermNodeStr("mut", MayRead | MayWrite | MayAlias | MayAliasWrite | IsLockless);
    immPerm = newPermNodeStr("imm", MayRead | MayAlias | RaceSafe | MayIntRefSum | IsLockless);
    roPerm = newPermNodeStr("ro", MayRead | MayAlias | IsLockless);
    mut1Perm = newPermNodeStr("mut1", MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless);
    opaqPerm = newPermNodeStr("opaq", MayAlias | RaceSafe | IsLockless);
}

// What core declares in Cone -- Option, Result, and the 'so' and 'rc' regions --
// is the core package's source, packages/core/src/core.cone, not this file's.

FnDclNode *initAllFn;
FnDclNode *finalAllFn;

// A compiler-provided function of no parameters returning nothing, bound as a
// name every module reaches unless it declares the name itself. It has no
// symbol: a call to it is generated as a call to what the intrinsic stands for
static FnDclNode *newStitchFn(char *name, int16_t intrinsic) {
    Name *namesym = nametblFind(name, strlen(name));
    FnSigNode *sig = newFnSigNode();
    sig->rettype = (INode*)newVoidNode();
    FnDclNode *fn = newFnDclNode(namesym, FlagPub, (INode*)sig, (INode*)newIntrinsicNode(intrinsic));
    fn->flags |= TypeChecked;
    namesym->node = (INode*)fn;
    return fn;
}

StructNode *regionRefTrait;
StructNode *moveTrait;
StructNode *copyTrait;
StructNode *tracedTrait;
StructNode *shapeChangingTrait;
StructNode *noLoanMutTrait;
StructNode *noLoanReadTrait;
StructNode *atomicValueTrait;
StructNode *integerTrait;
StructNode *pointerTrait;

// A trait the compiler declares, with no members, bound as a name every module
// reaches unless it declares the name itself
static StructNode *newBuiltinTrait(Name *name) {
    StructNode *trait = newStructNode(name);
    trait->flags |= TraitType | FlagPub | NameResolved | TypeChecked;
    name->node = (INode*)trait;
    return trait;
}

int corelibIsBuiltinTrait(INode *node) {
    return node == (INode*)regionRefTrait || node == (INode*)moveTrait || node == (INode*)copyTrait
        || node == (INode*)tracedTrait || node == (INode*)shapeChangingTrait
        || node == (INode*)noLoanMutTrait || node == (INode*)noLoanReadTrait
        || node == (INode*)atomicValueTrait || node == (INode*)integerTrait
        || node == (INode*)pointerTrait;
}

// Set up the standard library, whose names are always shared by all modules
void stdlibInit(int ptrsize) {

    unknownType = (INode*)newAbsenceNode();
    unknownType->tag = UnknownTag;
    noCareType = (INode*)newAbsenceNode();
    noCareType->tag = UnknownTag;
    // Distinct from unknownType by identity, and deliberately so. unknownType
    // means "not inferred yet", which must not silence anything. errorType means
    // "already reported as bad", which must: every check that would complain
    // about it stays quiet, so one compile reports several real problems instead
    // of a cascade descending from the first.
    errorType = (INode*)newAbsenceNode();
    errorType->tag = UnknownTag;
    elseCond = (INode*)newAbsenceNode();
    borrowRef = (INode*)newAbsenceNode();
    borrowRef->tag = BorrowRegTag;

    staticLifetimeNode = newLifetimeDclNode(nametblFind("'static", 7), 0);
    stdPermInit();
    stdNbrInit(ptrsize);

    // The program's stitched init and final [Jon 23 Sep], callable until the
    // entry glue calls them itself
    initAllFn = newStitchFn("initAll", InitAllIntrinsic);
    finalAllFn = newStitchFn("finalAll", FinalAllIntrinsic);

    // 'Never', the return type of a function that does not return: core's
    // 'panic', libc's 'abort'. It is 'void' wherever a type is asked about --
    // a node of VoidTag, generated as LLVM's void -- and known by identity
    // where it matters: a call returning it may end any block, one that must
    // produce a value included, as a 'return' does (blockTypeCheck,
    // fnImplicitReturn), and a function returning it must end in such a call
    // and is generated 'noreturn'. A name every module reaches, as 'i64' is,
    // unless it declares the name itself.
    neverType = (INode*)newVoidNode();
    nametblFind("Never", 5)->node = neverType;

    // 'RegionRef', the trait a region ref struct declares with 'is' [Jon 25
    // Sep]. Each method a region may declare is optional, with a fixed shape
    // when present, which no trait written in Cone can say, so the struct is
    // held to it by regionRefCheck rather than by the ordinary requirement check.
    regionRefTrait = newBuiltinTrait(regionRefName);
    // 'Move' and 'Copy' [Jon 26 Sep]: every type has exactly one. The compiler
    // grants them from what it infers (itypeIsMove: a 'final', a field that
    // moves, an owning reference that cannot be aliased); a type declaring 'is
    // Move' moves whatever it holds, and one declaring 'is Copy' is refused
    // where it moves after all (structCheckCopy).
    moveTrait = newBuiltinTrait(moveTraitName);
    copyTrait = newBuiltinTrait(copyTraitName);
    // 'Traced', which a region ref declares beside 'RegionRef' to say that
    // its references are found by tracing: a type's record carries a trace
    // that hands each one to the region's 'mark', and where such a reference
    // may be held is restricted to where a collector can find it
    // (regionTracedCheckAll). Held to 'mark' by regionRefCheck.
    tracedTrait = newBuiltinTrait(tracedTraitName);
    // What a container's element borrows cost it [Jon 26 Sep; names
    // provisional]. A borrow a method returns keeps its receiver loaned, the
    // Rust way (the loan walk, flowpath.c's pwCall). 'NoLoanMut' and
    // 'NoLoanRead' say its element borrows (any, or read-only ones) need no
    // loan on it -- every element is fresh and never moved, as an arena's
    // allocations are -- only that it is not moved, replaced or ended while
    // they are used; a container declaring 'NoLoanMut' is also borrowed
    // itself for its life only. Trusted: the compiler cannot check the
    // promise. 'ShapeChanging' says the container may move its elements (a
    // list's push reallocates). It has NO EFFECT YET: Jon's rule refuses an
    // element borrow of such a container reached through a shared path (a
    // '&mut' or '&' of unseen origin, a 'self' field, a '+rc-mut' owner),
    // but refusing it today breaks common collection code -- reading a
    // 'List[String]' element through a '&List' parameter -- that 'uni'
    // reborrowing is to make writable. Until then that is a documented hole
    // (refborref.html), and the marker waits for the check that reads it.
    shapeChangingTrait = newBuiltinTrait(shapeChangingTraitName);
    noLoanMutTrait = newBuiltinTrait(noLoanMutTraitName);
    noLoanReadTrait = newBuiltinTrait(noLoanReadTraitName);
    // 'AtomicValue' [Jon 26 Sep]: a struct declaring it is changed only by
    // atomic operations, even where it is 'imm' -- the one exception to 'imm
    // never changes'. It is a struct of one field, an integer, a Bool or a raw
    // pointer (structAtomicValueCheck), and it moves. A value holding one
    // anywhere inline (itypeHoldsAtomic) is never placed in read-only memory,
    // nor held in a 'const'. The sync package's 'Atomic[T]' declares it; the
    // compiler knows nothing else of that type.
    atomicValueTrait = newBuiltinTrait(atomicValueTraitName);
    // 'Integer' [Jon 27 Sep]: the integer types, signed and unsigned, of 8 to
    // 64 bits and pointer width. Granted by the compiler to i8 ... i64, u8 ...
    // u64, isize and usize while those are built here in C (genericTypeIs),
    // and asked only by a constraint, 'where T is Integer'; any other type
    // is one only by declaring it.
    integerTrait = newBuiltinTrait(integerTraitName);
    // 'Pointer' [Jon 27 Sep]: the raw pointer types, '*T' whatever T is and
    // whatever its permission, granted by the compiler (genericTypeIs) and
    // asked only by a constraint, 'where T is Pointer'. A reference is not
    // one, nor an integer; any other type is one only by declaring it.
    pointerTrait = newBuiltinTrait(pointerTraitName);
}
