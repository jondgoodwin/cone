/** Handling for cast nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create node for recasting to a new type without conversion
CastNode *newRecastNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, CastTag);
    node->flags |= FlagRecast;
    node->typ = node->vtype = type;
    node->exp = exp;
    return node;
}

// Create node for converting exp to a new type
CastNode *newConvCastNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, CastTag);
    node->typ = node->vtype = type;
    node->exp = exp;
    return node;
}

// Clone cast
INode *cloneCastNode(CloneState *cstate, CastNode *node) {
    CastNode *newnode;
    newnode = memAllocBlk(sizeof(CastNode));
    memcpy(newnode, node, sizeof(CastNode));
    newnode->exp = cloneNode(cstate, node->exp);
    newnode->typ = cloneNode(cstate, node->typ);
    return (INode *)newnode;
}

// Create a new cast node
CastNode *newIsNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, IsTag);
    node->typ = type;
    node->exp = exp;
    return node;
}

// Serialize cast
void castPrint(CastNode *node) {
    inodeFprint(node->tag==CastTag? "(cast, " : "(is, ");
    inodePrintNode(node->typ);
    inodeFprint(", ");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

// Name resolution of cast node
void castNameRes(NameResState *pstate, CastNode *node) {
    inodeNameRes(pstate, &node->exp);
    inodeNameRes(pstate, &node->typ);
}

#define ptrsize 10000
// Give a rough idea of comparable type size for use with type checking reinterpretation casts
uint32_t castBitsize(INode *type) {
    if (type->tag == UintNbrTag || type->tag == IntNbrTag || type->tag == FloatNbrTag) {
        if (type == (INode*)usizeType)
            return ptrsize;
        return ((NbrNode *)type)->bits;
    }
    switch (type->tag) {
    case PtrTag:
    case RefTag:
        return ptrsize;
    case ArrayRefTag:
        return ptrsize << 1;
    default:
        return 0;
    }
}

// Type check cast node:
// - reinterpret cast types must be same size
// - Ensure type can be safely converted to target type
void castTypeCheck(TypeCheckState *pstate, CastNode *node) {
    if (iexpTypeCheckAny(pstate, &node->exp) == 0)
        return;
    if (itypeTypeCheck(pstate, &node->typ) == 0)
        return;

    node->vtype = node->typ;
    INode *fromtype = iexpGetTypeDcl(node->exp);
    INode *totype = itypeGetTypeDcl(node->vtype);

    // Handle reinterpret casts, which must be same size
    if (node->flags & FlagRecast) {
        if (totype->tag != StructTag) {
            uint32_t tosize = castBitsize(totype);
            if (tosize == 0 || tosize != castBitsize(fromtype))
                errorMsgNode(node->exp, ErrorInvType, "May only reinterpret value to the same sized primitive type");
        }
        return;
    }

    // Handle conversion to bool
    if (totype == (INode*)boolType) {
        switch (fromtype->tag) {
        case UintNbrTag:
        case IntNbrTag:
        case FloatNbrTag:
        case RefTag:
        case PtrTag:
            break;
        default:
            errorMsgNode(node->exp, ErrorInvType, "Only numbers and ref/ptr may convert to Bool");
        }
        return;
    }
    switch (totype->tag) {
    case UintNbrTag:
        if (fromtype->tag == ArrayRefTag)
            return;
        // Fall-through expected here
    case IntNbrTag:
    case FloatNbrTag:
        if (fromtype->tag == UintNbrTag || fromtype->tag == IntNbrTag || fromtype->tag == FloatNbrTag)
            return;
        break;
    case RefTag:
        if (fromtype->tag == VirtRefTag)
            return;
        // Deliberate fall-through here
    case PtrTag:
        if (fromtype->tag == RefTag || fromtype->tag == PtrTag)
            return;
        break;
    case VirtRefTag:
        break;
    case StructTag:
        if (fromtype->tag == StructTag && (fromtype->flags & SameSize))
            return;
        break;
    }
    errorMsgNode(node->vtype, ErrorInvType, "Unsupported built-in type conversion");
}

// Analyze type comparison (is) node
void castIsTypeCheck(TypeCheckState *pstate, CastNode *node) {
    node->vtype = (INode*)boolType;
    iexpTypeCheckAny(pstate, &node->exp);
    itypeTypeCheck(pstate, &node->typ);
    if (!isExpNode(node->exp)) {
        errorMsgNode(node->exp, ErrorInvType, "'is' requires a typed expression to the left");
        return;
    }
    if (!isTypeNode(node->typ)) {
        errorMsgNode(node->typ, ErrorInvType, "'is' requires a type to the right");
        return;
    }
    INode *totype = itypeGetTypeDcl(node->typ);
    INode *fromtype = iexpGetTypeDcl(node->exp);

    // Handle the specialization check of a virtual reference
    if (fromtype->tag == VirtRefTag) {
        if (totype->tag == RefTag) {
            StructNode *strnode = (StructNode*)itypeGetTypeDcl(((RefNode*)totype)->pvtype);
            if (strnode->tag == StructTag) {
                if (structVirtRefMatches((StructNode*)itypeGetTypeDcl(((RefNode*)fromtype)->pvtype), strnode))
                    return;
            }
        }
        errorMsgNode((INode*)node, ErrorInvType, "Types are not compatible for this specialization");
    }

    // Make sure the checked type is a subtype of the value
    INode *needtypedcl = itypeGetTypeDcl(node->typ);
    INode *havetypedcl = itypeGetTypeDcl(((IExpNode*)node->exp)->vtype);
    if (NoMatch == itypeMatches(havetypedcl, needtypedcl, Coercion)) {
        errorMsgNode((INode*)node, ErrorInvType, "Types are not compatible for this specialization");
        return;
    }

    // Make sure we have a mechanism to check the specialization at runtime
    INode *basetypedcl = needtypedcl;
    if (needtypedcl->tag == RefTag)
        basetypedcl = itypeGetTypeDcl(((RefNode*)needtypedcl)->pvtype);
    if (basetypedcl->tag != StructTag || (basetypedcl->flags & TraitType) || !(basetypedcl->flags & HasTagField)) {
        errorMsgNode((INode*)node, ErrorInvType, "No mechanism exists to check this specialization");
        return;
    }

}
