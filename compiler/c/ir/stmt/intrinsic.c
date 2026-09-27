/** Handling for intrinsic nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>

int intrinsicForceFallback = 0;

// Create a new intrinsic node
IntrinsicNode *newIntrinsicNode(int16_t intrinsic) {
    IntrinsicNode *intrinsicNode;
    newNode(intrinsicNode, IntrinsicNode, IntrinsicTag);
    intrinsicNode->intrinsicFn = intrinsic;
    intrinsicNode->typearg = NULL;
    return intrinsicNode;
}

// Clone an intrinsic node. A declared generic intrinsic's type parameter use is
// substituted like any other, which is how an instance learns the type it is for
INode *cloneIntrinsicNode(CloneState *cstate, IntrinsicNode *node) {
    IntrinsicNode *newnode = memAllocBlk(sizeof(IntrinsicNode));
    memcpy(newnode, node, sizeof(IntrinsicNode));
    newnode->typearg = cloneNode(cstate, node->typearg);
    return (INode *)newnode;
}

// ---- The registry ------------------------------------------------------------
//
// The definition of record for every intrinsic declared in Cone. Each entry is
// what the name means in Cone's terms: its signature, whether a call is unsafe,
// whether a Cone fallback body may be written, where in the compiler it is
// answered, and whether this back end lowers it itself. The reference manual
// (refintrinsic.html) states each one's meaning and edge cases in words, and the
// 'intrinsic' test group pins them. An LLVM name appears nowhere here: the LLVM
// instructions that implement an entry live in genlDeclaredIntrinsic, as one
// implementation of it [Jon 26 Sep: capabilities we would be proud to build in
// the native code generator].

// The shape of one parameter or result, in terms of the one type parameter T
typedef enum {
    ShapeVoid,          // no result
    ShapeUsize,         // usize
    ShapeBool,          // Bool
    ShapeT,             // T
    ShapePtrT,          // *T
    ShapeSliceT,        // &[]T: a borrowed slice, read only
    ShapeSliceMutT,     // &[]mut T: a borrowed slice, writable
    ShapePtrTypeRecord, // *TypeRecord: core's type record (typeRecordIsPtr)
    ShapeU32,           // u32
    ShapeOrder,         // MemOrder: core's enum of atomic orderings, a constant at each call
    ShapeTBool          // T, Bool: a tuple of the two
} IntrinsicShape;

// The types T may be, where an entry does not take every type with a size. A
// bit each, so an entry names the union it accepts
typedef enum {
    ClassSized = 0,     // any type with a size (intrinsicDclTypeCheck)
    ClassInt = 1,       // an integer type of 8 to 64 bits, usize and isize among them
    ClassBool = 2,      // Bool
    ClassPtr = 4        // a raw pointer, to anything
} IntrinsicClass;

// Where the compiler answers an intrinsic: every one built so far is answered
// or expanded before a back end would need an instruction of its own for it,
// except moveRaw, a block move, and the atomic operations
typedef enum {
    PhaseConstant,      // a constant for the target, from the type alone
    PhaseExpansion,     // expanded at the call into operations every back end has
    PhaseOperation      // an operation a back end may do with an instruction of its own
} IntrinsicPhase;

typedef struct IntrinsicSpec {
    char *name;
    int16_t intrinsicFn;
    char *signature;        // As written after 'fn @intrinsic', for diagnostics
    uint8_t ntypeparms;
    uint8_t nparms;
    uint8_t parms[5];       // IntrinsicShape of each parameter
    uint8_t result;         // IntrinsicShape of the result
    uint8_t trust;          // A call can break memory safety, so belongs in 'trust'.
                            // Recorded, not enforced: 'trust' is not built (doc/design/safety.md)
    uint8_t fallback;       // A Cone body may be written, used where there is no lowering
    uint8_t phase;          // IntrinsicPhase
    uint8_t lowered;        // This back end implements it itself
    uint8_t tclass;         // IntrinsicClass: the types T may be, beyond having a size
} IntrinsicSpec;

static IntrinsicSpec intrinsicRegistry[] = {
    {"sizeof", SizeofIntrinsic, "sizeof[T]() usize",
        1, 0, {0}, ShapeUsize, 0, 0, PhaseConstant, 1},
    {"alignof", AlignofIntrinsic, "alignof[T]() usize",
        1, 0, {0}, ShapeUsize, 0, 0, PhaseConstant, 1},
    {"needsFinal", NeedsFinalIntrinsic, "needsFinal[T]() Bool",
        1, 0, {0}, ShapeBool, 0, 0, PhaseConstant, 1},
    {"finalize", FinalizeIntrinsic, "finalize[T](p *T)",
        1, 1, {ShapePtrT}, ShapeVoid, 1, 0, PhaseExpansion, 1},
    {"sliceFromParts", SliceFromPartsIntrinsic, "sliceFromParts[T](p *T, len usize) &[]T",
        1, 2, {ShapePtrT, ShapeUsize}, ShapeSliceT, 1, 0, PhaseExpansion, 1},
    {"sliceFromPartsMut", SliceFromPartsMutIntrinsic, "sliceFromPartsMut[T](p *T, len usize) &[]mut T",
        1, 2, {ShapePtrT, ShapeUsize}, ShapeSliceMutT, 1, 0, PhaseExpansion, 1},
    {"readRaw", ReadRawIntrinsic, "readRaw[T](p *T) T",
        1, 1, {ShapePtrT}, ShapeT, 1, 1, PhaseExpansion, 1},
    {"writeRaw", WriteRawIntrinsic, "writeRaw[T](p *T, value T)",
        1, 2, {ShapePtrT, ShapeT}, ShapeVoid, 1, 0, PhaseExpansion, 1},
    {"moveRaw", MoveRawIntrinsic, "moveRaw[T](to *T, from *T, count usize)",
        1, 3, {ShapePtrT, ShapePtrT, ShapeUsize}, ShapeVoid, 1, 1, PhaseOperation, 1},
    {"typeRecord", TypeRecordIntrinsic, "typeRecord[T]() *TypeRecord",
        1, 0, {0}, ShapePtrTypeRecord, 0, 0, PhaseConstant, 1},
    {"holdsTraced", HoldsTracedIntrinsic, "holdsTraced[T]() Bool",
        1, 0, {0}, ShapeBool, 0, 0, PhaseConstant, 1},
    {"trace", TraceIntrinsic, "trace[T](p *T, mode u32)",
        1, 2, {ShapePtrT, ShapeU32}, ShapeVoid, 1, 0, PhaseExpansion, 1},
    {"traceRoots", TraceRootsIntrinsic, "traceRoots(mode u32)",
        0, 1, {ShapeU32}, ShapeVoid, 0, 0, PhaseExpansion, 1},
    // The atomic operations, an instruction each: T is held to what a target
    // does indivisibly without a lock
    {"atomicLoad", AtomicLoadIntrinsic, "atomicLoad[T](p *T, order MemOrder) T",
        1, 2, {ShapePtrT, ShapeOrder}, ShapeT, 1, 1, PhaseOperation, 1, ClassInt | ClassBool | ClassPtr},
    {"atomicStore", AtomicStoreIntrinsic, "atomicStore[T](p *T, value T, order MemOrder)",
        1, 3, {ShapePtrT, ShapeT, ShapeOrder}, ShapeVoid, 1, 1, PhaseOperation, 1, ClassInt | ClassBool | ClassPtr},
    {"atomicSwap", AtomicSwapIntrinsic, "atomicSwap[T](p *T, value T, order MemOrder) T",
        1, 3, {ShapePtrT, ShapeT, ShapeOrder}, ShapeT, 1, 1, PhaseOperation, 1, ClassInt | ClassBool | ClassPtr},
    {"atomicAdd", AtomicAddIntrinsic, "atomicAdd[T](p *T, value T, order MemOrder) T",
        1, 3, {ShapePtrT, ShapeT, ShapeOrder}, ShapeT, 1, 1, PhaseOperation, 1, ClassInt},
    {"atomicSub", AtomicSubIntrinsic, "atomicSub[T](p *T, value T, order MemOrder) T",
        1, 3, {ShapePtrT, ShapeT, ShapeOrder}, ShapeT, 1, 1, PhaseOperation, 1, ClassInt},
    {"atomicAnd", AtomicAndIntrinsic, "atomicAnd[T](p *T, value T, order MemOrder) T",
        1, 3, {ShapePtrT, ShapeT, ShapeOrder}, ShapeT, 1, 1, PhaseOperation, 1, ClassInt | ClassBool},
    {"atomicOr", AtomicOrIntrinsic, "atomicOr[T](p *T, value T, order MemOrder) T",
        1, 3, {ShapePtrT, ShapeT, ShapeOrder}, ShapeT, 1, 1, PhaseOperation, 1, ClassInt | ClassBool},
    {"atomicXor", AtomicXorIntrinsic, "atomicXor[T](p *T, value T, order MemOrder) T",
        1, 3, {ShapePtrT, ShapeT, ShapeOrder}, ShapeT, 1, 1, PhaseOperation, 1, ClassInt | ClassBool},
    {"atomicCompareSwap", AtomicCompareSwapIntrinsic,
        "atomicCompareSwap[T](p *T, expected T, desired T, success MemOrder, failure MemOrder) T, Bool",
        1, 5, {ShapePtrT, ShapeT, ShapeT, ShapeOrder, ShapeOrder}, ShapeTBool, 1, 1, PhaseOperation, 1,
        ClassInt | ClassBool | ClassPtr},
};

#define IntrinsicCount (sizeof(intrinsicRegistry) / sizeof(IntrinsicSpec))

static IntrinsicSpec *intrinsicFind(Name *name) {
    for (size_t i = 0; i < IntrinsicCount; ++i) {
        if (strcmp(intrinsicRegistry[i].name, &name->namestr) == 0)
            return &intrinsicRegistry[i];
    }
    return NULL;
}

static IntrinsicSpec *intrinsicFindKind(int16_t intrinsicFn) {
    for (size_t i = 0; i < IntrinsicCount; ++i) {
        if (intrinsicRegistry[i].intrinsicFn == intrinsicFn)
            return &intrinsicRegistry[i];
    }
    return NULL;
}

// Serialize an intrinsic node
void intrinsicPrint(IntrinsicNode *node) {
    IntrinsicSpec *spec = intrinsicFindKind(node->intrinsicFn);
    if (spec)
        inodeFprint("intrinsic %s", spec->name);
    else
        inodeFprint("intrinsic %d", (int)node->intrinsicFn);
    if (node->typearg) {
        inodeFprint("[");
        inodePrintNode(node->typearg);
        inodeFprint("]");
    }
}

// Whether a declared type is a use of the type parameter 'tparm'
static int intrinsicIsTParm(INode *type, INode *tparm) {
    return tparm != NULL && type != NULL && isNameUseNode(type) && ((NameUseNode *)type)->dclnode == tparm;
}

static int memOrderIs(INode *type);

// Whether a name-resolved declared type has the registry's shape
static int intrinsicShapeIs(INode *type, IntrinsicShape shape, INode *tparm) {
    if (type == NULL)
        return 0;
    switch (shape) {
    case ShapeT:
        return intrinsicIsTParm(type, tparm);
    case ShapePtrT:
        // In a template '*T' is held as a dereference, since a type parameter
        // is not yet a type; cloning makes it a pointer (cloneStarNode)
        return (type->tag == PtrTag || type->tag == DerefTag)
            && intrinsicIsTParm(((StarNode *)type)->vtexp, tparm);
    case ShapeSliceT:
    case ShapeSliceMutT: {
        // Held as a borrow in a template for the same reason (cloneRefNode)
        if (type->tag != ArrayRefTag && type->tag != ArrayBorrowTag)
            return 0;
        RefNode *ref = (RefNode *)type;
        if (ref->region != borrowRef || !intrinsicIsTParm(ref->vtexp, tparm))
            return 0;
        // An unwritten permission is a borrowed reference's default, 'ro'
        INode *perm = ref->perm == unknownType ? (INode *)roPerm : itypeGetTypeDcl(ref->perm);
        return perm == (INode *)(shape == ShapeSliceT ? roPerm : mutPerm);
    }
    case ShapePtrTypeRecord:
        return typeRecordIsPtr(type);
    case ShapeOrder:
        return memOrderIs(type);
    case ShapeTBool: {
        if (type->tag != TTupleTag && type->tag != TupleTag)
            return 0;
        Nodes *elems = ((TupleNode *)type)->elems;
        return elems->used == 2 && intrinsicShapeIs(nodesGet(elems, 0), ShapeT, tparm)
            && intrinsicShapeIs(nodesGet(elems, 1), ShapeBool, tparm);
    }
    default:
        break;
    }
    if (intrinsicIsTParm(type, tparm))
        return 0;
    INode *dcl = itypeGetTypeDcl(type);
    switch (shape) {
    case ShapeVoid:  return dcl->tag == VoidTag;
    case ShapeUsize: return dcl == (INode *)usizeType;
    case ShapeU32:   return dcl == (INode *)u32Type;
    case ShapeBool:  return dcl == (INode *)boolType;
    default:         return 0;
    }
}

// Whether a name-resolved declaration's signature is the registry's
static int intrinsicSigMatches(FnDclNode *fndcl, IntrinsicSpec *spec) {
    Nodes *tparms = fndcl->genericinfo ? fndcl->genericinfo->parms : NULL;
    if ((tparms ? tparms->used : 0) != spec->ntypeparms)
        return 0;
    INode *tparm = tparms ? nodesGet(tparms, 0) : NULL;
    FnSigNode *sig = (FnSigNode *)fndcl->vtype;
    if (sig == NULL || sig->tag != FnSigTag || sig->parms->used != spec->nparms)
        return 0;
    for (uint32_t i = 0; i < spec->nparms; ++i) {
        VarDclNode *parm = (VarDclNode *)nodesGet(sig->parms, i);
        // A default value would give a call a way to leave out what the meaning needs
        if (parm->value != NULL || !intrinsicShapeIs(parm->vtype, spec->parms[i], tparm))
            return 0;
    }
    return intrinsicShapeIs(sig->rettype, spec->result, tparm);
}

// Whether a declaration is a function of the core package: of its root module
// or a submodule of it at any depth, or a function (not a method: it takes no
// 'self') of a plain struct one of them declares. The struct is how core names
// its intrinsics through one name, 'mem', while a submodule of core cannot be
// reached from outside it (refintrinsic.html). A generic type's or a trait's is
// refused: it would be cloned into each instance or implementer. The root is
// the module with no owner, as an imported package's and a compile's root are
static int intrinsicModuleIsCore(INode *owner) {
    if (owner == NULL || owner->tag != ModuleTag)
        return 0;
    ModuleNode *mod = (ModuleNode *)owner;
    while (mod->dclinfo.owner && mod->dclinfo.owner->tag == ModuleTag)
        mod = (ModuleNode *)mod->dclinfo.owner;
    return mod->dclinfo.owner == NULL && mod->namesym != NULL
        && strcmp(&mod->namesym->namestr, "core") == 0;
}

static int intrinsicInCore(FnDclNode *fndcl) {
    INode *owner = fndcl->dclinfo.owner;
    if (owner && owner->tag == StructTag) {
        StructNode *type = (StructNode *)owner;
        if (type->genericinfo || (type->flags & TraitType) || (fndcl->flags & FlagMethFld))
            return 0;
        owner = type->dclinfo.owner;
    }
    return intrinsicModuleIsCore(owner);
}

// The type record is known by its name and its package, as the intrinsics are:
// what the compiler puts in one (genlTypeRecord) is the layout core declares,
// which the compiler holds it to when it builds the first one. A struct of
// another module named TypeRecord is an ordinary struct.
static StructNode *typeRecordPointee(INode *type) {
    if (type == NULL)
        return NULL;
    INode *ptr = type->tag == DerefTag || !isTypeNode(type) ? type : itypeGetTypeDcl(type);
    if (ptr->tag != PtrTag && ptr->tag != DerefTag)
        return NULL;
    INode *vtexp = ((StarNode *)ptr)->vtexp;
    if (vtexp == NULL)
        return NULL;
    // An unresolved name, or one a template still holds as an expression
    INode *rec = isNameUseNode(vtexp) ? nameUseGetDcl((NameUseNode *)vtexp) : vtexp;
    if (rec == NULL || rec->tag != StructTag)
        return NULL;
    StructNode *strnode = (StructNode *)rec;
    if (strnode->namesym != NULL && strcmp(&strnode->namesym->namestr, "TypeRecord") == 0
        && strnode->genericinfo == NULL && intrinsicModuleIsCore(strnode->dclinfo.owner))
        return strnode;
    return NULL;
}

int typeRecordIsPtr(INode *type) {
    return typeRecordPointee(type) != NULL;
}

// Core's MemOrder, known by its name and its package as TypeRecord is: the
// enum whose variants name the orderings (MemOrderKind), in that order
static StructNode *memOrderEnum(INode *type) {
    if (type == NULL)
        return NULL;
    INode *dcl = isNameUseNode(type) ? nameUseGetDcl((NameUseNode *)type) : type;
    if (dcl == NULL || dcl->tag != StructTag)
        return NULL;
    StructNode *strnode = (StructNode *)dcl;
    if (strnode->namesym != NULL && strcmp(&strnode->namesym->namestr, "MemOrder") == 0
        && (strnode->flags & HasTagField) && strnode->genericinfo == NULL
        && intrinsicModuleIsCore(strnode->dclinfo.owner))
        return strnode;
    return NULL;
}

static int memOrderIs(INode *type) {
    return memOrderEnum(type) != NULL;
}

// Core's TypeRecord, remembered from the result of core's 'mem.typeRecord'
// declaration as the registry accepts it
static StructNode *typeRecordCore = NULL;

StructNode *typeRecordStruct(void) {
    return typeRecordCore;
}

// Check an '@intrinsic' declaration against the registry, once its signature
// and any body are name resolved. A declaration that passes gets its meaning:
// its value becomes the IntrinsicNode that generation expands at each call,
// carrying a use of its type parameter for each instance to substitute. One
// whose body is to be used instead -- the back end has no lowering, or
// '--intrinsic-fallback' asked for bodies -- keeps it as an inline function,
// so that it too is expanded where it is called and never has a symbol.
// A declaration that fails is left as written: its compile stops at errors.
void intrinsicDclNameRes(FnDclNode *fndcl) {
    Name *name = fndcl->namesym;
    if (name == NULL)
        return;     // parseFn reported the missing name
    if (!intrinsicInCore(fndcl)) {
        errorMsgNode((INode *)fndcl, ErrorIntrinsicPlace,
            "'@intrinsic' is allowed only on a function of a module of the core package, which is where the compiler's intrinsics are declared.");
        return;
    }
    IntrinsicSpec *spec = intrinsicFind(name);
    if (spec == NULL) {
        errorMsgNode((INode *)fndcl, ErrorIntrinsicName,
            "The compiler defines no intrinsic named %s.", &name->namestr);
        return;
    }
    if (!intrinsicSigMatches(fndcl, spec)) {
        errorMsgNode((INode *)fndcl, ErrorIntrinsicSig,
            "The compiler defines the intrinsic %s as 'fn @intrinsic %s', and this declaration says something else.",
            &name->namestr, spec->signature);
        return;
    }
    int hasbody = fndcl->value != NULL;
    if (hasbody && !spec->fallback) {
        errorMsgNode((INode *)fndcl, ErrorIntrinsicBody,
            "The intrinsic %s has no fallback body: nothing written in Cone can do what it does, so declare it without one.",
            &name->namestr);
        return;
    }
    if (!hasbody && !spec->lowered) {
        errorMsgNode((INode *)fndcl, ErrorIntrinsicBody,
            "This back end has no lowering of its own for the intrinsic %s, so its declaration needs a fallback body in Cone.",
            &name->namestr);
        return;
    }
    if (spec->intrinsicFn == TypeRecordIntrinsic)
        typeRecordCore = typeRecordPointee(((FnSigNode *)fndcl->vtype)->rettype);
    if (hasbody && (intrinsicForceFallback || !spec->lowered)) {
        fndcl->flags |= FlagInline;
        return;
    }
    IntrinsicNode *node = newIntrinsicNode(spec->intrinsicFn);
    inodeLexCopy((INode *)node, (INode *)fndcl);
    if (fndcl->genericinfo)
        node->typearg = newNameUseFromDclNode(nodesGet(fndcl->genericinfo->parms, 0), (INode *)fndcl);
    fndcl->value = (INode *)node;
}

// Type check a declared intrinsic that has its meaning from the registry: an
// instance of a generic one, whose type argument is now concrete, or a
// non-generic one. Every intrinsic built so far acts on a value of its type or
// its layout, so the type must have a size. Reported where the instance was
// asked for, since the declaration in core is not what is wrong.
void intrinsicDclTypeCheck(TypeCheckState *pstate, FnDclNode *fndcl) {
    IntrinsicNode *node = (IntrinsicNode *)fndcl->value;
    if (node->typearg == NULL)
        return;
    if (itypeTypeCheck(pstate, &node->typearg) == 0)
        return;
    INode *nosizeroot;
    char *nosize = itypeNoSizeCause(node->typearg, &nosizeroot);
    if (nosize) {
        INode *where = fndcl->instnode ? fndcl->instnode : (INode *)fndcl;
        errorMsgNode(where, ErrorIntrinsicType,
            "The intrinsic %s needs a type with a size, and %s %s.",
            &fndcl->namesym->namestr, itypeName(nosizeroot), nosize);
        itypeNoSizeExplain(node->typearg);
    }
    // A raw placement of a traced reference hides it from every collector
    // (judged once every type is laid out)
    regionTracedRawNote(fndcl, node->intrinsicFn, node->typearg);
}

// Whether a function is a declared intrinsic with its meaning from the registry
int intrinsicIsDeclared(FnDclNode *fndcl) {
    return fndcl->value != NULL && fndcl->value->tag == IntrinsicTag
        && ((IntrinsicNode *)fndcl->value)->intrinsicFn >= FirstDeclaredIntrinsic;
}

// ---- Type classes and orderings ---------------------------------------------
//
// Both are asked of a declared intrinsic whether it is lowered or runs its
// fallback body, so '--intrinsic-fallback' refuses what the lowering refuses:
// the class is checked on the instance before its body is (fnDclTypeCheck), and
// the orderings where the call is made (fnCallFinalizeArgs).

// The class bit a type is of, or 0 for none of them
static int intrinsicClassOf(INode *type) {
    INode *dcl = itypeGetTypeDcl(type);
    if (dcl == (INode *)boolType)
        return ClassBool;
    if ((dcl->tag == IntNbrTag || dcl->tag == UintNbrTag)
        && ((NbrNode *)dcl)->bits >= 8 && ((NbrNode *)dcl)->bits <= 64)
        return ClassInt;
    if (dcl->tag == PtrTag)
        return ClassPtr;
    return 0;
}

static char *intrinsicClassWords(int tclass) {
    switch (tclass) {
    case ClassInt:                          return "an integer type of 8 to 64 bits";
    case ClassInt | ClassBool:              return "an integer type of 8 to 64 bits or Bool";
    case ClassInt | ClassBool | ClassPtr:   return "an integer type of 8 to 64 bits, Bool or a raw pointer";
    default:                                return "of another class";
    }
}

int intrinsicClassCheck(FnDclNode *fndcl) {
    IntrinsicSpec *spec = fndcl->namesym ? intrinsicFind(fndcl->namesym) : NULL;
    if (spec == NULL || spec->tclass == ClassSized)
        return 1;
    // A template still holds '*T' as a dereference, and a declaration the
    // registry refused may say anything: neither has an instance's type to judge
    FnSigNode *sig = (FnSigNode *)fndcl->vtype;
    if (sig == NULL || sig->tag != FnSigTag || sig->parms->used != spec->nparms || spec->parms[0] != ShapePtrT)
        return 1;
    INode *ptr = itypeGetTypeDcl(((VarDclNode *)nodesGet(sig->parms, 0))->vtype);
    if (ptr->tag != PtrTag)
        return 1;
    INode *type = ((StarNode *)ptr)->vtexp;
    if (intrinsicClassOf(type) & spec->tclass)
        return 1;
    // Reported where the program's own source chose the type: an instance
    // called from a generic's instance -- a 'Bump[Bool]' whose unconstrained
    // 'add' calls atomicAdd[Bool] -- at the outermost place that asked, as a raw
    // placement of a traced reference is (regionTracedRawNote), and once
    // there however many of that instance's calls are refused
    static INode *lastwhere = NULL;
    INode *where = fndcl->instnode ? fndcl->instnode : (INode *)fndcl;
    int nested = 0;
    while (where->instnode != NULL && where->instnode != where) {
        where = where->instnode;
        nested = 1;
    }
    if (where == lastwhere)
        return 0;
    lastwhere = where;
    errorMsgNode(where, ErrorIntrinsicType,
        nested ? "The intrinsic %s acts on a T that is %s, and %s is not one: the generic instance made here calls it."
            : "The intrinsic %s acts on a T that is %s, and %s is not one.",
        &fndcl->namesym->namestr, intrinsicClassWords(spec->tclass),
        itypeGetTypeDcl(type)->tag == PtrTag ? "a raw pointer" : itypeName(type));
    return 0;
}

// Is this a type some atomic operation acts on: an integer of 8 to 64 bits,
// Bool or a raw pointer? What an atomic value may hold (structAtomicValueCheck)
int intrinsicIsAtomicType(INode *type) {
    return intrinsicClassOf(type) != 0;
}

static char *memOrderNames[] = {"Relaxed", "Acquire", "Release", "AcqRel", "SeqCst"};

// The ordering an argument passed as a MemOrder names, when it is a constant:
// a variant's literal, 'MemOrder.SeqCst[]', or a const holding one, coerced to
// the enum. Answers 0 for anything else, whose value is known only as it runs
static int intrinsicOrderOf(INode *arg, MemOrderKind *order) {
    for (;;) {
        if (arg->tag == CastTag)
            arg = ((CastNode *)arg)->exp;
        else if (nameUseNames(arg, ConstDclTag))
            arg = ((ConstDclNode *)((NameUseNode *)arg)->dclnode)->value;
        else
            break;
    }
    if (arg == NULL || arg->tag != TypeLitTag)
        return 0;
    INode *variant = itypeGetTypeDcl(((IExpNode *)arg)->vtype);
    if (variant->tag != StructTag || memOrderEnum(((StructNode *)variant)->basetrait) == NULL)
        return 0;
    Name *name = ((StructNode *)variant)->namesym;
    for (int i = OrderRelaxed; i <= OrderSeqCst; ++i) {
        if (strcmp(&name->namestr, memOrderNames[i]) == 0) {
            *order = (MemOrderKind)i;
            return 1;
        }
    }
    return 0;
}

// How strongly an ordering orders what follows a load: a compareSwap that fails
// only loads, and what it promises there may not exceed what success promises
static int memOrderLoadRank(MemOrderKind order) {
    switch (order) {
    case OrderAcquire: case OrderAcqRel: return 1;
    case OrderSeqCst: return 2;
    default: return 0;
    }
}

void intrinsicCallCheck(FnCallNode *call, FnDclNode *fndcl) {
    IntrinsicSpec *spec = fndcl->namesym ? intrinsicFind(fndcl->namesym) : NULL;
    if (spec == NULL || call->args == NULL || call->args->used != spec->nparms)
        return;
    char *name = &fndcl->namesym->namestr;
    MemOrderKind orders[2];
    int norders = 0;
    for (uint32_t i = 0; i < spec->nparms; ++i) {
        if (spec->parms[i] != ShapeOrder)
            continue;
        INode *arg = nodesGet(call->args, i);
        if (!intrinsicOrderOf(arg, &orders[norders])) {
            errorMsgNode(arg, ErrorAtomicConst,
                "The ordering mem.%s is given must be a constant, MemOrder.SeqCst[] or a const holding one: which instruction the call is depends on it.",
                name);
            return;
        }
        ++norders;
    }
    if (norders == 0)
        return;
    INode *orderarg = nodesGet(call->args, spec->nparms - norders);
    switch (spec->intrinsicFn) {
    case AtomicLoadIntrinsic:
        if (orders[0] == OrderRelease || orders[0] == OrderAcqRel)
            errorMsgNode(orderarg, ErrorAtomicOrder,
                "A load has nothing to release, so mem.atomicLoad takes Relaxed, Acquire or SeqCst, not %s.",
                memOrderNames[orders[0]]);
        break;
    case AtomicStoreIntrinsic:
        if (orders[0] == OrderAcquire || orders[0] == OrderAcqRel)
            errorMsgNode(orderarg, ErrorAtomicOrder,
                "A store has nothing to acquire, so mem.atomicStore takes Relaxed, Release or SeqCst, not %s.",
                memOrderNames[orders[0]]);
        break;
    case AtomicCompareSwapIntrinsic: {
        INode *failarg = nodesGet(call->args, spec->nparms - 1);
        if (orders[1] == OrderRelease || orders[1] == OrderAcqRel)
            errorMsgNode(failarg, ErrorAtomicOrder,
                "A compareSwap that fails only loads, which has nothing to release, so its failure ordering is Relaxed, Acquire or SeqCst, not %s.",
                memOrderNames[orders[1]]);
        else if (memOrderLoadRank(orders[1]) > memOrderLoadRank(orders[0]))
            errorMsgNode(failarg, ErrorAtomicOrder,
                "The failure ordering of mem.atomicCompareSwap may not be stronger than its success ordering, and %s is stronger than %s.",
                memOrderNames[orders[1]], memOrderNames[orders[0]]);
        break;
    }
    default:
        break;
    }
}

FnDclNode *intrinsicAtomicCallee(FnCallNode *call) {
    INode *callee = call->objfn;
    if (isNameUseNode(callee))
        callee = ((NameUseNode *)callee)->dclnode;
    if (callee == NULL || callee->tag != FnDclTag || !(((FnDclNode *)callee)->dclinfo.facts & DclIntrinsic))
        return NULL;
    FnDclNode *fndcl = (FnDclNode *)callee;
    if (!intrinsicIsDeclared(fndcl))
        return NULL;
    int16_t kind = ((IntrinsicNode *)fndcl->value)->intrinsicFn;
    return kind >= AtomicLoadIntrinsic && kind <= AtomicCompareSwapIntrinsic ? fndcl : NULL;
}

void intrinsicCallOrders(FnCallNode *call, FnDclNode *fndcl, MemOrderKind *orders) {
    IntrinsicSpec *spec = intrinsicFindKind(((IntrinsicNode *)fndcl->value)->intrinsicFn);
    for (uint32_t i = 0; i < spec->nparms; ++i) {
        if (spec->parms[i] == ShapeOrder && !intrinsicOrderOf(nodesGet(call->args, i), orders++))
            errorUnreachable((INode *)call, "an atomic intrinsic's ordering that type check found constant is not");
    }
}
