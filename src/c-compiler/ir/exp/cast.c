/** Handling for cast nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new cast node
CastNode *newCastNode(INode *exp, INode *type) {
    CastNode *node;
    newNode(node, CastNode, CastTag);
    node->vtype = type;
    node->exp = exp;
    return node;
}

// Serialize cast
void castPrint(CastNode *node) {
    inodeFprint("(cast, ");
    inodePrintNode(node->vtype);
    inodeFprint(", ");
    inodePrintNode(node->exp);
    inodeFprint(")");
}

// Name resolution of cast node
void castNameRes(NameResState *pstate, CastNode *node) {
    inodeNameRes(pstate, &node->exp);
    inodeNameRes(pstate, &node->vtype);
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
    inodeTypeCheck(pstate, &node->exp);
    inodeTypeCheck(pstate, &node->vtype);
    INode *totype = itypeGetTypeDcl(node->vtype);
    INode *fromtype = iexpGetTypeDcl(node->exp);

    // Handle reinterpret casts, which must be same size
    if (node->flags & FlagAsIf) {
        uint32_t tosize = castBitsize(totype);
        if (tosize == 0 || tosize != castBitsize(fromtype))
            errorMsgNode(node->exp, ErrorInvType, "May only reinterpret value to the same sized primitive type");
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
    case PtrTag:
        if (fromtype->tag == RefTag || fromtype->tag == PtrTag)
            return;
    }
    errorMsgNode(node->vtype, ErrorInvType, "Unsupported built-in type conversion");
}
