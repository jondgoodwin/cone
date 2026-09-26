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
    ShapeSliceMutT      // &[]mut T: a borrowed slice, writable
} IntrinsicShape;

// Where the compiler answers an intrinsic: every one built so far is answered
// or expanded before a back end would need an instruction of its own for it,
// except moveRaw, a block move
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
    uint8_t parms[3];       // IntrinsicShape of each parameter
    uint8_t result;         // IntrinsicShape of the result
    uint8_t trust;          // A call can break memory safety, so belongs in 'trust'.
                            // Recorded, not enforced: 'trust' is not built (doc/design/safety.md)
    uint8_t fallback;       // A Cone body may be written, used where there is no lowering
    uint8_t phase;          // IntrinsicPhase
    uint8_t lowered;        // This back end implements it itself
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
    default:
        break;
    }
    if (intrinsicIsTParm(type, tparm))
        return 0;
    INode *dcl = itypeGetTypeDcl(type);
    switch (shape) {
    case ShapeVoid:  return dcl->tag == VoidTag;
    case ShapeUsize: return dcl == (INode *)usizeType;
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
static int intrinsicInCore(FnDclNode *fndcl) {
    INode *owner = fndcl->dclinfo.owner;
    if (owner && owner->tag == StructTag) {
        StructNode *type = (StructNode *)owner;
        if (type->genericinfo || (type->flags & TraitType) || (fndcl->flags & FlagMethFld))
            return 0;
        owner = type->dclinfo.owner;
    }
    if (owner == NULL || owner->tag != ModuleTag)
        return 0;
    ModuleNode *mod = (ModuleNode *)owner;
    while (mod->dclinfo.owner && mod->dclinfo.owner->tag == ModuleTag)
        mod = (ModuleNode *)mod->dclinfo.owner;
    return mod->dclinfo.owner == NULL && mod->namesym != NULL
        && strcmp(&mod->namesym->namestr, "core") == 0;
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
}

// Whether a function is a declared intrinsic with its meaning from the registry
int intrinsicIsDeclared(FnDclNode *fndcl) {
    return fndcl->value != NULL && fndcl->value->tag == IntrinsicTag
        && ((IntrinsicNode *)fndcl->value)->intrinsicFn >= FirstDeclaredIntrinsic;
}
