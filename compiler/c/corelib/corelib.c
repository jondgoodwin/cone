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
INode *nullLitType;
INode *elseCond;
INode *borrowRef;
INode *neverType;
StructNode *arrayTypeDcl;
StructNode *strTypeDcl;
static int strTypeAdopted;
StructNode *cstrTypeDcl;
static int cstrTypeAdopted;
PermNode *uniPerm;
PermNode *mutPerm;
PermNode *immPerm;
PermNode *roPerm;
PermNode *mut1Perm;
PermNode *opaqPerm;
PermNode *newPerm;
NbrNode *boolType;
NbrNode *charType;
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
    // An initializer's 'self': the one path to memory being filled, as 'uni'
    // is to a value. Its name stays the keyword's, so it is not bound here;
    // that nothing reads through it before it is filled, and that it does not
    // escape, is flow's to check (flowNewSelfUse).
    newPerm = newPermDclNode(nametblFind("new", 3), MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless);
}

// What core declares in Cone -- Option, Result, and the 'So' and 'Rc' regions --
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
StructNode *threadSafeTrait;
StructNode *shapeChangingTrait;
StructNode *noLoanMutTrait;
StructNode *noLoanReadTrait;
StructNode *atomicValueTrait;
StructNode *integerTrait;
StructNode *pointerTrait;
StructNode *sendableTrait;
StructNode *sizedTrait;
StructNode *dynSizedTrait;
StructNode *immutableTrait;
StructNode *lockPermTrait;

// A trait the compiler declares, with no members, bound as a name every module
// reaches unless it declares the name itself
static StructNode *newBuiltinTrait(Name *name) {
    StructNode *trait = newStructNode(name);
    trait->flags |= TraitType | FlagPub | NameResolved | TypeChecked;
    name->node = (INode*)trait;
    return trait;
}

StructNode *stdlibAdoptStr(Name *name, ModuleNode *mod) {
    if (name != strTypeName || strTypeAdopted || mod == NULL || mod->namesym != nametblFind("core", 4))
        return NULL;
    strTypeAdopted = 1;
    strTypeDcl->flags &= ~(NameResolved | TypeChecked);
    return strTypeDcl;
}

// core's declaration of 'cstr' is not a new struct but the compiler's own, given
// the field and the methods it writes. The struct the compiler made for the
// modules that have no core (stdlibInit) is emptied: core says what it holds.
StructNode *stdlibAdoptCStr(Name *name, ModuleNode *mod) {
    if (name != cstrTypeName || cstrTypeAdopted || mod == NULL || mod->namesym != nametblFind("core", 4))
        return NULL;
    cstrTypeAdopted = 1;
    iNsTypeInit((INsTypeNode*)cstrTypeDcl, 8);
    nodelistInit(&cstrTypeDcl->fields, 8);
    cstrTypeDcl->flags &= ~(NameResolved | TypeChecked);
    return cstrTypeDcl;
}

int corelibIsBuiltinTrait(INode *node) {
    return node == (INode*)regionRefTrait || node == (INode*)moveTrait || node == (INode*)copyTrait
        || node == (INode*)tracedTrait || node == (INode*)threadSafeTrait
        || node == (INode*)shapeChangingTrait
        || node == (INode*)noLoanMutTrait || node == (INode*)noLoanReadTrait
        || node == (INode*)atomicValueTrait || node == (INode*)integerTrait
        || node == (INode*)pointerTrait || node == (INode*)sendableTrait
        || node == (INode*)sizedTrait || node == (INode*)dynSizedTrait
        || node == (INode*)immutableTrait
        || node == (INode*)lockPermTrait;
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
    // A 'null' is any raw pointer type until the type it is wanted as says
    // which (litAdoptNullType). Distinct by identity, so that one left without
    // a pointer type is noticed rather than read as not inferred yet.
    nullLitType = (INode*)newAbsenceNode();
    nullLitType->tag = UnknownTag;
    elseCond = (INode*)newAbsenceNode();
    borrowRef = (INode*)newAbsenceNode();
    borrowRef->tag = BorrowRegTag;

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

    // 'Array', which names the fixed-size array type: 'Array[f32, 3]', and with
    // several sizes, row-major, 'Array[f32, 3, 3]'. It looks like a generic
    // type with number parameters, but is the compiler's: name resolution
    // lowers a bracketed use into the array type node (arrayTypeLower), and
    // any other use is refused (nameUseTypeCheckType). A name every module
    // reaches, as 'Never' is, unless it declares the name.
    arrayTypeDcl = newStructNode(nametblFind("Array", 5));
    arrayTypeDcl->flags |= FlagPub | NameResolved | TypeChecked | OpaqueType | DeclaredOpaque;
    arrayTypeDcl->namesym->node = (INode*)arrayTypeDcl;

    // 'str', the dynamically sized body of bytes. It has no fields and no size
    // of its own, so it is held only through a reference, which carries the
    // count of bytes: '&str', 'So[str]', 'Rc[str]'. A name every module
    // reaches, as 'Array' is, and a module may not declare it itself.
    strTypeDcl = newStructNode(strTypeName);
    strTypeDcl->flags |= FlagPub | NameResolved | TypeChecked | OpaqueType | DeclaredOpaque;
    strTypeName->node = (INode*)strTypeDcl;

    // 'cstr', a borrowed C string: a struct of one raw pointer to bytes that
    // end in a NUL. A name every module reaches, as 'str' is, so that the
    // C-named modules, which have no prelude, declare their strings with it.
    // It is built whole here, field and all, for a compile that has no core;
    // core's own declaration of it, with its methods, replaces the field with
    // its own (stdlibAdoptCStr). Name resolved, and type checked when first named.
    cstrTypeDcl = newStructNode(cstrTypeName);
    cstrTypeDcl->flags |= FlagPub | NameResolved;
    cstrTypeName->node = (INode*)cstrTypeDcl;
    {
        StarNode *ptrtype = newStarNode(PtrTag);
        ptrtype->vtexp = (INode*)u8Type;
        FieldDclNode *field = newFieldDclNode(cstrPtrFieldName, (INode*)mutPerm);
        field->vtype = (INode*)ptrtype;
        field->index = 0;
        field->flags |= FlagMethFld;
        structAddField(cstrTypeDcl, field);
    }

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
    // 'ThreadSafe', which a region ref declares beside 'RegionRef' to say that
    // owners of one value may be held by several threads at once: its 'aliasRef'
    // and 'dealiasRef' may run on different threads together (the sync
    // package's 'Arc', whose count is atomic; core's 'Rc' does not declare
    // it). The name is provisional. It is the region's say in whether a
    // reference may cross threads, which the thread check reads beside the
    // permission's RaceSafe: an owner that may be aliased crosses only where
    // its region declares it (refThreadBinds, through regionIsThreadSafe).
    // Declaring it on anything but a region ref is refused
    // (regionThreadSafeUseCheck). Trusted: the compiler cannot check the
    // promise.
    threadSafeTrait = newBuiltinTrait(threadSafeTraitName);
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
    // '&mut' or '&' of unseen origin, a 'self' field, a 'Rc[mut, T]' owner),
    // but refusing it today breaks common collection code -- reading a
    // 'List[String]' element through a '&List' parameter -- that 'uni'
    // reborrowing is to make writable. Until then that is a documented hole
    // (refborref.html), and the marker waits for the check that reads it.
    shapeChangingTrait = newBuiltinTrait(shapeChangingTraitName);
    noLoanMutTrait = newBuiltinTrait(noLoanMutTraitName);
    noLoanReadTrait = newBuiltinTrait(noLoanReadTraitName);
    // 'AtomicValue' [Jon 26 Sep]: a struct declaring it is changed only by
    // atomic operations, even where it is 'imm' -- the one exception to 'imm
    // never changes'. It is a struct of one field, an integer, a bool or a raw
    // pointer (structAtomicValueCheck), and it moves. A value holding one
    // anywhere inline (itypeHoldsAtomic) is never placed in read-only memory,
    // nor held in a 'const'. The core package's 'Atomic[T]' declares it; the
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
    // 'Sendable': a value of the type may cross to another thread -- be moved
    // to one, or, behind a shared owner such as 'Arc[imm, T]', be read from
    // several at once. Asked by a constraint, 'where T is Sendable', which
    // is how library code marks what crosses (thread.start, sync's
    // channels). Granted by the compiler (genericTypeIs, itypeThreadBound)
    // to every type holding no reference that is bound to its thread: a
    // borrowed one, a raw pointer, one whose permission is not RaceSafe, an
    // owner that may be aliased in a region not declaring ThreadSafe, or a
    // traced one. A type may also declare it, which is a promise the
    // compiler takes on trust for what it cannot see -- raw pointers a
    // library shares safely, as a channel's ends do -- except that an
    // instance of a generic type declaring it is Sendable only where its
    // type arguments are.
    sendableTrait = newBuiltinTrait(sendableTraitName);
    // 'Sized' and 'DynSized' [Jon 6 Oct]: what a type's size is. 'Sized': the
    // size is known at compile time, so a value may be held, and a reference
    // to it is one thin pointer. 'DynSized': the size is known at compile time
    // or carried by a reference to the type, which is then fat: a trait's
    // reference carries a vtable, a body such as 'str' a length. A type with
    // neither (declared @opaque, or an '@unsized' enum) is reached by a thin
    // reference and its size is not told. Granted by the compiler from the type
    // (genericTypeIs) and asked only by a constraint, 'where T is Sized'; a
    // type cannot declare either.
    sizedTrait = newBuiltinTrait(sizedTraitName);
    dynSizedTrait = newBuiltinTrait(dynSizedTraitName);
    // 'Immutable' [Jon 6 Oct]: a type that declares it, with 'is', is never
    // changed through a reference. Unlike 'Sized' it is declared, not granted:
    // 'str' declares it here, and any struct may. It has two effects, kept as
    // two rules apart (ir/types/reference.c) so the second may be loosened
    // later without touching the first: a reference to the type written with
    // no permission is 'imm' (refImmutableDefaultPerm), so '&str' is '&imm str'
    // and 'Rc[str]' copies; and the permissions that write through a shared
    // path, 'mut', 'mut1' and a lock permission, are refused on it
    // (refImmutableBan). Trusted: the compiler does not check the type's own
    // fields.
    immutableTrait = newBuiltinTrait(immutableTraitName);
    strTypeDcl->traits = newNodes(1);
    nodesAdd(&strTypeDcl->traits, (INode*)immutableTrait);
    // 'LockPermission': a struct declaring it may stand in a managed
    // reference's permission slot, 'Arc[Mutex, T]', as a lock permission. Its
    // value is the lock, kept in the allocation's header between the region's
    // part and the value; the reference gives no access to the value, and a
    // borrow through it takes the lock (the struct's 'acquireMut', or for a
    // read-only borrow its 'acquireRead' where it declares one) and the
    // borrow's end gives it back ('releaseMut', 'releaseRead'). Held to that
    // shape by lockPermCheck; where it may go, by refLockCheck. Declaring
    // 'ThreadSafe' beside it says the lock is for owners on several threads,
    // and fits only a region declaring it too.
    lockPermTrait = newBuiltinTrait(lockPermTraitName);
}
