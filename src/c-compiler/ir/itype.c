/** Generic Type node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"

#include <string.h>
#include <assert.h>

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *itypeGetTypeDcl(INode *type) {
    assert(isTypeNode(type));
    while (1) {
        switch (type->tag) {
        case TypeNameUseTag:
            type = ((NameUseNode *)type)->dclnode;
            break;
        case TypedefTag:
            type = ((TypedefNode *)type)->typeval;
            break;
        default:
            return type;
        }
    }
}

// Return node's type's declaration node (or vtexp if a ref or ptr)
INode *itypeGetDerefTypeDcl(INode *node) {
    INode *typnode = itypeGetTypeDcl(node);
    if (typnode->tag == RefTag || typnode->tag == VirtRefTag)
        return itypeGetTypeDcl(((RefNode*)node)->vtexp);
    else if (node->tag == PtrTag)
        return itypeGetTypeDcl(((StarNode*)node)->vtexp);
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

// Add a named type declaration to the buffer: its name, plus the type arguments
// it was instantiated with if it is a generic instance.
//
// The arguments are what tell two instances of one generic type apart. Both carry
// the generic's name, and each instance's methods are clones sharing the generic's
// 'genname', so without them both instances' copies of a method land on one LLVM
// symbol. That symbol is 'linkonce': inside a single translation unit LLVM appends
// '.1' and every call still reaches the body it meant, but once modules are
// compiled separately the linker keeps one body and calls to the other instance
// silently reach the wrong code. A generic *function*'s instances already differ,
// because genlMangleMethName walks their parameter types and a type argument
// appears in at least one of them; a generic *type*'s method may name no type
// parameter anywhere in its signature -- 'fn tally(self) i64' -- so the arguments
// have to come from the instance.
//
// cloneNode stamps the instantiating node on every node of an instance, and that
// node is the call carrying the type arguments. It is required to be a call with a
// non-empty list of types, which is what a macro expansion's node is not.
static char *itypeMangleNamed(char *bufp, INode *dclnode) {
    Name *namesym = inodeGetName(dclnode);
    if (namesym == NULL)
        return bufp;
    strcpy(bufp, &namesym->namestr);
    bufp += strlen(bufp);

    INode *instnode = dclnode->instnode;
    if (instnode == NULL
        || (instnode->tag != FnCallTag && instnode->tag != TypeLitTag
            && instnode->tag != ArrIndexTag && instnode->tag != FldAccessTag))
        return bufp;
    Nodes *typeargs = ((FnCallNode*)instnode)->args;
    if (typeargs == NULL || typeargs->used == 0)
        return bufp;

    // Checked before anything is written, so a node that is not an instantiation
    // after all leaves the plain name rather than half a suffix
    INode **argsp;
    uint32_t cnt;
    for (nodesFor(typeargs, cnt, argsp)) {
        if (*argsp == NULL || !isTypeNode(*argsp))
            return bufp;
    }
    for (nodesFor(typeargs, cnt, argsp)) {
        *bufp++ = ':';
        bufp = itypeMangle(bufp, *argsp);
    }
    return bufp;
}

// Add type mangle info to buffer
char *itypeMangle(char *bufp, INode *vtype) {
    switch (vtype->tag) {
    case NameUseTag:
    case TypeNameUseTag:
        return itypeMangleNamed(bufp, itypeGetTypeDcl(vtype));

    // A generic's type argument arrives as the type declaration itself rather
    // than as a use of its name, so the named types are mangled here as well.
    // Through the name, not the one-letter kind these used to answer with, since
    // 'i8' and 'i64' have to be told apart.
    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
    case StructTag:
    case EnumTag:
        return itypeMangleNamed(bufp, vtype);

    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *reftype = (RefNode *)vtype;
        *bufp++ = vtype->tag==VirtRefTag? '<' : ArrayRefTag? '+' : '&';
        if (permIsSame(reftype->perm, (INode*)roPerm)) {
            bufp = itypeMangle(bufp, reftype->perm);
            *bufp++ = ' ';
        }
        bufp = itypeMangle(bufp, reftype->vtexp);
        break;
    }
    case PtrTag:
    {
        StarNode *vtexp = (StarNode *)vtype;
        *bufp++ = '*';
        bufp = itypeMangle(bufp, vtexp->vtexp);
        break;
    }
    default:
        assert(0 && "unknown type for parameter type mangling");
    }
    return bufp + strlen(bufp);
}

// Return true if type has a concrete and instantiable value. 
// Opaque structs, traits, functions will be false.
int itypeIsConcrete(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    return !(dcltype->flags & OpaqueType);
}

// Return true if type has zero size (e.g., void, empty struct)
int itypeIsZeroSize(INode *type) {
    INode *dcltype = itypeGetTypeDcl(type);
    return dcltype->flags & ZeroSizeType;
}

// Return true if type implements move semantics
int itypeIsMove(INode *type) {
    return itypeGetTypeDcl(type)->flags & MoveType;
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
// struct's instantiation is a type -- a generic function's names a function --
// and a macro cannot arrive here at all, since a use of a MacroDcl is tagged
// MacroNameTag rather than as a name use of a type.
int itypeIsGenericType(INode *type) {
    if (type->tag != FnCallTag)
        return 0;
    FnCallNode *gentype = (FnCallNode*)type;
    if (gentype->objfn->tag != TypeNameUseTag)
        return 0;
    INode *dclnode = ((NameUseNode*)gentype->objfn)->dclnode;
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
