/** Generic Type node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *itypeGetTypeDcl(INode *type) {
    assert(isTypeNode(type));
    // A name that names something other than a type is handed back as it is,
    // so that a caller's tag test reports it where it was written
    while (1) {
        if (isNameUseNode(type) && isTypeNode(type))
            type = nameUseGetDcl((NameUseNode *)type);
        // A type alias stands for a type expression, and a name use bound to one
        // already answers for its target, so this is the alias reached directly
        else if (type->tag == AliasDclTag)
            type = ((AliasDclNode *)type)->target;
        else
            return type;
    }
}

// Return node's type's declaration node (or vtexp if a ref or ptr)
INode *itypeGetDerefTypeDcl(INode *node) {
    INode *typnode = itypeGetTypeDcl(node);
    if (typnode->tag == RefTag || typnode->tag == VirtRefTag)
        return itypeGetTypeDcl(((RefNode*)typnode)->vtexp);
    else if (typnode->tag == PtrTag)
        return itypeGetTypeDcl(((StarNode*)typnode)->vtexp);
    return typnode;
}

// Look for named field/method in type
INode *iTypeFindFnField(INode *type, Name *name) {
    switch (type->tag) {
    case StructTag:
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        return iNsTypeFindFnField((INsTypeNode*)type, name);
    case PtrTag:
        return iNsTypeFindFnField(ptrType, name);
    default:
        return NULL;
    }
}

// Type check node, expecting it to be a type. Give error and return 0, if not.
int itypeTypeCheck(TypeCheckState *pstate, INode **node) {
    inodeTypeCheckAny(pstate, node);
    if (!isTypeNode(*node)) {
        errorMsgNode(*node, ErrorNotTyped, "Expected a type.");
        return 0;
    }
    return 1;
}

// Return 1 if nominally (or structurally) identical, 0 otherwise
// Nodes must both be types, but may be name use or declare nodes
int itypeIsSame(INode *node1, INode *node2) {

    node1 = itypeGetTypeDcl(node1);
    node2 = itypeGetTypeDcl(node2);

    // If they are the same type name, types match
    if (node1 == node2)
        return 1;
    if (node1->tag != node2->tag)
        return 0;

    // For non-named types, equality is determined structurally
    // because they specify the same typed parts
    switch (node1->tag) {
    case RefTag: 
        return refIsSame((RefNode*)node1, (RefNode*)node2);
    case VirtRefTag:
        return refIsSame((RefNode*)node1, (RefNode*)node2);
    case ArrayRefTag:
        return arrayRefIsSame((RefNode*)node1, (RefNode*)node2);
    case PtrTag:
        return ptrEqual((StarNode*)node1, (StarNode*)node2);
    case ArrayTag:
        return arrayEqual((ArrayNode*)node1, (ArrayNode*)node2);
    case TTupleTag:
        return ttupleEqual((TupleNode*)node1, (TupleNode*)node2);
    case FnSigTag:
        return fnSigEqual((FnSigNode*)node1, (FnSigNode*)node2);
    case VoidTag:
        return 1;
    default:
        return 0;
    }
}

// Calculate the hash for a type to use in type table indexing
size_t itypeHash(INode *node) {
    INode *type = itypeGetTypeDcl(node);
    switch (type->tag) {
    case RefTag:
    case VirtRefTag:
        return refHash((RefNode*)type);
    case ArrayRefTag:
        return arrayRefHash((RefNode*)type);
    case PermTag:
        return ((size_t)immPerm) >> 3;  // Hash for all static permissions is the same
    default:
        // Turn type's pointer into the hash, removing expected 0's in bottom bits
        return ((size_t)type) >> 3;
    }
}

// Return 1 if nominally (or structurally) identical at runtime, 0 otherwise
// Nodes must both be types, but may be name use or declare nodes
// Is a companion for indexing into the type table
int itypeIsRunSame(INode *node1, INode *node2) {

    node1 = itypeGetTypeDcl(node1);
    node2 = itypeGetTypeDcl(node2);

    // If they are the same type name, types match
    if (node1 == node2)
        return 1;
    if (node1->tag != node2->tag)
        return 0;

    // For non-named types, equality is determined structurally
    // because they specify the same typed parts
    switch (node1->tag) {
    case RefTag:
        return refIsRunSame((RefNode*)node1, (RefNode*)node2);
    case VirtRefTag:
        return refIsRunSame((RefNode*)node1, (RefNode*)node2);
    case ArrayRefTag:
        return arrayRefIsRunSame((RefNode*)node1, (RefNode*)node2);
    case PtrTag:
        return ptrEqual((StarNode*)node1, (StarNode*)node2);
    case ArrayTag:
        return arrayEqual((ArrayNode*)node1, (ArrayNode*)node2);
    case TTupleTag:
        return ttupleEqual((TupleNode*)node1, (TupleNode*)node2);
    case FnSigTag:
        return fnSigEqual((FnSigNode*)node1, (FnSigNode*)node2);
    case VoidTag:
        return 1;
    case PermTag:
        return 1;    // Static permissions are erased/equivalent at runtime
    default:
        return 0;
    }
}

// Is totype equivalent or a subtype of fromtype
TypeCompare itypeMatches(INode *totype, INode *fromtype, SubtypeConstraint constraint) {
    fromtype = itypeGetTypeDcl(fromtype);
    totype = itypeGetTypeDcl(totype);

    // If they are the same value type info, types match
    if (totype == fromtype)
        return EqMatch;

    // Either side already reported as bad matches anything. The diagnostic that
    // made it bad has been issued, and a mismatch derived from it says nothing
    // the programmer does not already know.
    if (totype == errorType || fromtype == errorType)
        return EqMatch;

    // Type-specific matching logic
    switch (totype->tag) {

    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        return nbrMatches(totype, fromtype, constraint);

    case StructTag:
        return structMatches((StructNode*)totype, fromtype, constraint);

    case TTupleTag:
        if (fromtype->tag == TTupleTag)
            return itypeIsSame(totype, fromtype) ? EqMatch : NoMatch;
        return NoMatch;

    case ArrayTag:
        if (fromtype->tag == ArrayTag)
            return arrayMatches((ArrayNode*)totype, (ArrayNode*)fromtype, constraint);
        return NoMatch;

    case FnSigTag:
        if (fromtype->tag == FnSigTag)
            return fnSigMatches((FnSigNode*)totype, (FnSigNode*)fromtype, constraint);
        return NoMatch;

    case RefTag:
        if (fromtype->tag == RefTag)
            return refMatches((RefNode*)totype, (RefNode*)fromtype, constraint);
        return NoMatch;

    case VirtRefTag:
        if (fromtype->tag == VirtRefTag)
            return refvirtMatches((RefNode*)totype, (RefNode*)fromtype, constraint);
        else if (fromtype->tag == RefTag)
            return refvirtMatchesRef((RefNode*)totype, (RefNode*)fromtype, constraint);
        return NoMatch;

    case ArrayRefTag:
        if (fromtype->tag == ArrayRefTag)
            return arrayRefMatches((RefNode*)totype, (RefNode*)fromtype, constraint);
        else if (fromtype->tag == RefTag)
            return arrayRefMatchesRef((RefNode*)totype, (RefNode*)fromtype, constraint);
        return NoMatch;

    case PtrTag:
        if (fromtype->tag == RefTag || fromtype->tag == ArrayRefTag)
            return itypeIsSame(((RefNode*)fromtype)->vtexp, ((StarNode*)totype)->vtexp) ? ConvSubtype : NoMatch;
        if (fromtype->tag == PtrTag)
            return ptrMatches((StarNode*)totype, (StarNode*)fromtype, constraint);
        return NoMatch;

    case VoidTag:
        return fromtype->tag == VoidTag ? EqMatch : NoMatch;

    default:
        return itypeIsSame(totype, fromtype) ? EqMatch : NoMatch;
    }
}

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *itypeFindSuper(INode *type1, INode *type2) {
    INode *typ1 = itypeGetTypeDcl(type1);
    INode *typ2 = itypeGetTypeDcl(type2);

    if (typ1->tag != typ2->tag)
        return NULL;
    if (itypeIsSame(typ1, typ2))
        return type1;
    switch (typ1->tag) {
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
        return nbrFindSuper(type1, type2);

    case StructTag:
        return structFindSuper(type1, type2);

    case RefTag:
    case VirtRefTag:
        return refFindSuper(type1, type2);

    default:
        return NULL;
    }
}

// The type arguments a generic instance was instantiated with, or NULL when the
// declaration is not an instance of a generic.
//
// cloneNode stamps the instantiating node on every node of an instance, and for
// a generic instance that node is the call carrying the type arguments. It is
// required to be a call with a non-empty list of types. A macro expansion's node
// is not, and neither is the implementing struct that a trait's default method
// is cloned into: an inherited default is a copy, not an instance.
Nodes *itypeInstanceTypeArgs(INode *dclnode) {
    INode *instnode = dclnode->instnode;
    if (instnode == NULL
        || (instnode->tag != FnCallTag && instnode->tag != TypeLitTag
            && instnode->tag != ArrIndexTag && instnode->tag != FldAccessTag))
        return NULL;
    Nodes *typeargs = ((FnCallNode*)instnode)->args;
    if (typeargs == NULL || typeargs->used == 0)
        return NULL;
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(typeargs, cnt, argsp)) {
        if (*argsp == NULL || !isTypeNode(*argsp))
            return NULL;
    }
    return typeargs;
}

// Return true if type has a concrete and instantiable value. 
// Opaque structs, traits, functions will be false.
int itypeIsConcrete(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    return !(dcltype->flags & OpaqueType);
}

// How many hops of an infection path are worth following or printing. Ordinary
// code is one or two; the bound is here so that a pathological nesting -- or a
// by-value cycle already reported and still in the tree -- cannot run this off
// the stack or bury the diagnostic.
#define NoSizeChainMax 8

static INode *itypeNoSizeField(INode *dcltype, uint32_t depth);

// Is this an enum with a variant still being laid out?
//
// An enum's size is the largest of its variants, and generation is what computes
// that. Its own TypeChecked mark says only that its own fields are settled, which
// begins with the tag: the variants are type checked separately, each pulling the
// enum in as its base and finishing it before its own fields are walked. So the
// mark cannot be read as 'has a size' here, and one variant still in flight is
// exactly the case where the enum has none -- which is what a variant holding its
// own enum by value asks for.
//
// In flight, not merely unfinished. A variant not yet begun is one the module
// walk has not reached: a parameter written above the enum, or in a parent of
// the enum's module, asks before any variant is. Nothing of that variant is on
// the demand stack, so it cannot close a cycle with the asker, and if it does
// hold the enum, its own field asks again once it is in flight, and is refused.
static int itypeVariantPending(INode *dcltype) {
    // TraitType and a derived list are both required: a *variant* carries the
    // closed flags too, inherited from its enum, and has no derived list at all.
    if (dcltype->tag != StructTag || !(dcltype->flags & TraitType)
        || !(dcltype->flags & (HasTagField | SameSize))
        || ((StructNode*)dcltype)->derived == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((StructNode*)dcltype)->derived, cnt, nodesp)) {
        if (((*nodesp)->flags & TypeChecking) && !((*nodesp)->flags & TypeChecked))
            return 1;
    }
    return 0;
}

// A type's name, for a diagnostic. See itype.h.
char *itypeName(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    switch (dcltype->tag) {
    case FnSigTag:
        return "a function signature";
    case ArrayTag:
        return "an array";
    case RefTag: case VirtRefTag: case ArrayRefTag: case PtrTag:
        return "a reference";
    default:
        break;
    }
    Name *namesym = inodeGetName(dcltype);
    return namesym ? (char*)&namesym->namestr : "this type";
}

// This type's own reason for having no size, ignoring anything it caught from a
// field, or NULL when it has a size or is unsized only by infection.
//
// Order matters: a trait or an '@unsized' enum, and a struct infected by an
// unsized field, all carry OpaqueType, so each is asked before the plain
// declared-opaque reading that would otherwise absorb it.
static char *itypeNoSizeOwnCause(INode *dcltype, uint32_t depth) {
    // A reference of any kind is one or two pointers wide whatever it points at,
    // so it has a size from the moment it exists, even while its own check is in
    // flight. That happens without any cycle: a typedef of '&Quad' written above
    // Quad demands Quad from inside the reference, and a method of Quad taking
    // the typedef reaches the same reference again before it finishes.
    if (dcltype->tag == RefTag || dcltype->tag == VirtRefTag
        || dcltype->tag == ArrayRefTag || dcltype->tag == PtrTag)
        return NULL;

    // Still being laid out. Its own fields are what this walk is in the middle
    // of settling, so there is no size to give yet -- and no cycle check is
    // needed to say so, since a finished type would not be in this state.
    if ((dcltype->flags & TypeChecking) && !(dcltype->flags & TypeChecked))
        return "is still being laid out, so it would have to contain itself. Break the cycle by holding it through a reference";

    // An enum is laid out only once every variant is, whatever its own mark says
    if (itypeVariantPending(dcltype))
        return "is an enum with a variant still being laid out, so it would have to contain itself. Break the cycle by holding it through a reference";

    if (!(dcltype->flags & OpaqueType))
        return NULL;

    // A function signature is a description of a call, not a value
    if (dcltype->tag == FnSigTag)
        return "is not a value at all. Use a reference to a function instead";

    if (dcltype->tag == StructTag) {
        // A type whose implementations differ in size has no one size: a trait,
        // whose implementers are open-ended, or an enum that declined the padding
        if ((dcltype->flags & TraitType) && !(dcltype->flags & SameSize))
            return (dcltype->flags & EnumType)
                ? "is an '@unsized' enum, so its variants differ in size. Reach it through a reference"
                : "is a trait whose implementations may differ in size. Use a virtual reference, '&<Trait>'";

        // Opacity is infectious. Where a field carried it, this type is not the
        // cause and the caller keeps walking.
        if (itypeNoSizeField(dcltype, depth) != NULL)
            return NULL;

        return "is declared @opaque, so this program is not told how large it is. Hold it through a reference";
    }

    return "has no known size, so it cannot be held by value";
}

// The first field of this struct whose own type has no size, or NULL if none
// does. This is the hop an infected type's size went missing across.
//
// Depth-bounded because the type graph it walks may be cyclic. A cycle through
// a reference is legal and terminates on its own -- a reference is not a struct
// -- but a by-value cycle that was already reported still sits in the tree, and
// this runs on error paths, where the tree is exactly the shape nothing checked.
static INode *itypeNoSizeField(INode *dcltype, uint32_t depth) {
    if (dcltype->tag != StructTag || depth >= NoSizeChainMax)
        return NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&((StructNode*)dcltype)->fields, cnt, nodesp)) {
        INode *fldtype = itypeGetTypeDcl(((IExpNode*)*nodesp)->vtype);
        if (itypeNoSizeOwnCause(fldtype, depth + 1) != NULL
            || itypeNoSizeField(fldtype, depth + 1) != NULL)
            return *nodesp;
    }
    return NULL;
}

// Why this type cannot report a size. See itype.h.
char *itypeNoSizeCause(INode *type, INode **rootp) {
    INode *dcltype = itypeGetTypeDcl(type);
    uint32_t hops = 0;
    while (++hops <= NoSizeChainMax) {
        char *own = itypeNoSizeOwnCause(dcltype, 0);
        if (own) {
            if (rootp)
                *rootp = dcltype;
            return own;
        }
        INode *fld = itypeNoSizeField(dcltype, 0);
        if (fld == NULL)
            return NULL;
        dcltype = itypeGetTypeDcl(((IExpNode*)fld)->vtype);
    }
    if (rootp)
        *rootp = dcltype;
    return "has no known size, and the chain that led there is deeper than this message will follow";
}

// Name the path from an unsized type to its cause. See itype.h.
void itypeNoSizeExplain(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    uint32_t hops = 0;
    while (++hops <= NoSizeChainMax) {
        // Its own cause is what the diagnostic already said, so the walk ends
        // rather than repeating it
        if (itypeNoSizeOwnCause(dcltype, 0) != NULL)
            return;
        INode *fld = itypeNoSizeField(dcltype, 0);
        if (fld == NULL)
            return;
        INode *fldtype = itypeGetTypeDcl(((IExpNode*)fld)->vtype);
        errorMsgNode(fld, Uncounted, "... %s has no size because its field %s has type %s",
            itypeName(dcltype), &inodeGetName(fld)->namestr, itypeName(fldtype));
        dcltype = fldtype;
    }
}

// Return true if type has zero size (e.g., void, empty struct)
int itypeIsZeroSize(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    return dcltype->flags & ZeroSizeType;
}

// Return true if type implements move semantics
int itypeIsMove(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    // A tuple is like a struct: it moves when any of its elements does. It has
    // no declaration to carry the flag, so the question is asked of its elements.
    if (dcltype->tag == TTupleTag) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(((TupleNode*)dcltype)->elems, cnt, nodesp)) {
            if (itypeIsMove(*nodesp))
                return 1;
        }
        return 0;
    }
    return dcltype->flags & MoveType;
}

// Return true if this is an instantiation of a generic type, such as 'Box[i64]'.
//
// An instantiation is an unlowered FnCallNode until type check replaces it with
// the instance it names, and isTypeNode asks this so that the passes running
// before then -- name resolution's type-versus-value disambiguation, above all
// -- can tell one from a call. Without it '*Box[i64]' reads as a dereference
// and '[2; Box[i64]]' as an array literal, so an instantiation is a type
// everywhere but inside a composite type.
//
// What tells a generic from anything else is the GenericInfo its declaration
// carries: a generic is an ordinary FnDcl or StructNode with a type parameter
// list attached, which is also how genericSubstitute recognizes one. Only a
// struct's instantiation is a type: a generic function's names a function, and
// a macro's names a MacroDcl.
int itypeIsGenericType(INode *type) {
    if (type->tag != FnCallTag)
        return 0;
    FnCallNode *gentype = (FnCallNode*)type;
    if (!isNameUseNode(gentype->objfn))
        return 0;
    INode *dclnode = nameUseGetDcl((NameUseNode*)gentype->objfn);
    if (dclnode == NULL || dclnode->tag != StructTag || genericGetInfo(dclnode) == NULL)
        return 0;
    return gentype->args != NULL && gentype->args->used > 0 && nodesGet(gentype->args, 0) != NULL;
}

// Return drop function (or NULL) for type
INode *itypeGetDropFnDcl(INode *typenode) {
    INode *type = itypeGetTypeDcl(typenode);
    switch (type->tag) {
    case StructTag:
        return ((StructNode*)type)->dropfn;
    default:
        return NULL;
    }
}
