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
    while (1) {
        switch (type->tag) {
        case TypeNameUseTag:
            type = ((NameUseNode *)type)->dclnode;
            break;
        case TypedefTag:
            type = ((TypedefNode *)type)->typeval;
        default:
            return type;
        }
    }
}

// Return node's type's declaration node (or pvtype if a ref or ptr)
INode *itypeGetDerefTypeDcl(INode *node) {
    INode *typnode = itypeGetTypeDcl(node);
    if (typnode->tag == RefTag || typnode->tag == VirtRefTag)
        return itypeGetTypeDcl(((RefNode*)node)->pvtype);
    else if (node->tag == PtrTag)
        return itypeGetTypeDcl(((PtrNode*)node)->pvtype);
    return typnode;
}

// Return 1 if nominally (or structurally) identical, 0 otherwise
// Nodes must both be types, but may be name use or declare nodes
int itypeIsSame(INode *node1, INode *node2) {

    if (node1->tag == TypeNameUseTag)
        node1 = (INode*)((NameUseNode *)node1)->dclnode;
    if (node2->tag == TypeNameUseTag)
        node2 = (INode*)((NameUseNode *)node2)->dclnode;

    // If they are the same type name, types match
    if (node1 == node2)
        return 1;
    if (node1->tag != node2->tag)
        return 0;

    // For non-named types, equality is determined structurally
    // because they specify the same typed parts
    switch (node1->tag) {
    case FnSigTag:
        return fnSigEqual((FnSigNode*)node1, (FnSigNode*)node2);
    case RefTag: 
        return refEqual((RefNode*)node1, (RefNode*)node2);
    case ArrayRefTag:
        return arrayRefEqual((RefNode*)node1, (RefNode*)node2);
    case VirtRefTag:
        return refEqual((RefNode*)node1, (RefNode*)node2);
    case PtrTag:
        return ptrEqual((PtrNode*)node1, (PtrNode*)node2);
    case VoidTag:
        return 1;
    default:
        return 0;
    }
}

// Is totype equivalent or a non-changing subtype of fromtype
// 0 - no
// 1 - yes, without conversion
// 2 - requires casting/coercion (non-lossy)
// 3+ - requires increasingly lossy number conversion (literal only)
int itypeMatches(INode *totype, INode *fromtype) {
    // Convert, if needed, from names to the type declaration
    if (totype->tag == TypeNameUseTag)
        totype = (INode*)((NameUseNode *)totype)->dclnode;
    if (fromtype->tag == TypeNameUseTag)
        fromtype = (INode*)((NameUseNode *)fromtype)->dclnode;

    // If they are the same value type info, types match
    if (totype == fromtype)
        return 1;

    // Type-specific matching logic
    switch (totype->tag) {
    case StructTag:
        return structMatches((StructNode*)totype, (StructNode*)fromtype);

    case RefTag:
        if (fromtype->tag != RefTag)
            return 0;
        return refMatches((RefNode*)totype, (RefNode*)fromtype);

    case VirtRefTag:
        if (fromtype->tag != VirtRefTag && fromtype->tag != RefTag)
            return 0;
        return refvirtMatches((RefNode*)totype, (RefNode*)fromtype);

    case ArrayRefTag:
        if (fromtype->tag != ArrayRefTag)
            return 0;
        return arrayRefMatches((RefNode*)totype, (RefNode*)fromtype);

    case PtrTag:
        if (fromtype->tag == RefTag || fromtype->tag == ArrayRefTag)
            return itypeIsSame(((RefNode*)fromtype)->pvtype, ((PtrNode*)totype)->pvtype)? 2 : 0;
        if (fromtype->tag != PtrTag)
            return 0;
        return ptrMatches((PtrNode*)totype, (PtrNode*)fromtype);

    case ArrayTag:
        if (totype->tag != fromtype->tag)
            return 0;
        return arrayEqual((ArrayNode*)totype, (ArrayNode*)fromtype);

    //case FnSigTag:
    //    return fnSigMatches((FnSigNode*)totype, (FnSigNode*)fromtype);

    case UintNbrTag:
        if ((fromtype->tag == RefTag || fromtype->tag == PtrTag) && totype == (INode*)boolType)
            return 2;
        // Fall through is intentional here...
    case IntNbrTag:
    case FloatNbrTag:
        if (totype == (INode*)boolType)
            return 2;
        if (totype->tag != fromtype->tag)
            return isNbr(totype) && isNbr(fromtype) ? 4 : 0;
        if (((NbrNode *)totype)->bits == ((NbrNode *)fromtype)->bits)
            return 1;
        return ((NbrNode *)totype)->bits > ((NbrNode *)fromtype)->bits ? 2 : 3;

    case VoidTag:
        return 1;
    default:
        return itypeIsSame(totype, fromtype);
    }
}

// Add type mangle info to buffer
char *itypeMangle(char *bufp, INode *vtype) {
    switch (vtype->tag) {
    case NameUseTag:
    case TypeNameUseTag:
    {
        strcpy(bufp, &((INsTypeNode*)((NameUseNode *)vtype)->dclnode)->namesym->namestr);
        break;
    }
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    {
        RefNode *reftype = (RefNode *)vtype;
        *bufp++ = vtype->tag==VirtRefTag? '<' : ArrayRefTag? '+' : '&';
        if (permIsSame(reftype->perm, (INode*)constPerm)) {
            bufp = itypeMangle(bufp, reftype->perm);
            *bufp++ = ' ';
        }
        bufp = itypeMangle(bufp, reftype->pvtype);
        break;
    }
    case PtrTag:
    {
        PtrNode *pvtype = (PtrNode *)vtype;
        *bufp++ = '*';
        bufp = itypeMangle(bufp, pvtype->pvtype);
        break;
    }
    default:
        assert(0 && "unknown type for parameter type mangling");
    }
    return bufp + strlen(bufp);
}

// Return true if type has a concrete and instantiable. 
// Opaque (field-less) structs, traits, functions, void will be false.
int itypeIsConcrete(INode *type) {
    if (!isTypeNode(type))
        return 0;
    INode *dcltype = itypeGetTypeDcl(type);
    return !(dcltype->flags & OpaqueType);
}
