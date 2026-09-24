/** Handling for primitive numbers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Clone number node
INode *cloneNbrNode(CloneState *cstate, NbrNode *node) {
    NbrNode *newnode = memAllocBlk(sizeof(NbrNode));
    memcpy(newnode, node, sizeof(NbrNode));
    return (INode *)newnode;
}

// Serialize a number type as its name. Every number type is declared with one
// (stdNbrInit), so this covers usize and isize, which a list of the fixed-width
// types left printing as nothing.
void nbrTypePrint(NbrNode *node) {
    inodeFprint("%s", &node->namesym->namestr);
}

// Is a number-typed node
int isNbr(INode *node) {
    return (node->tag == IntNbrTag || node->tag == UintNbrTag || node->tag == FloatNbrTag);
}

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *nbrFindSuper(INode *type1, INode *type2) {
    NbrNode *typ1 = (NbrNode *)itypeGetTypeDcl(type1);
    NbrNode *typ2 = (NbrNode *)itypeGetTypeDcl(type2);

    return typ1->bits >= typ2->bits ? type1 : type2;
}

// Is from-type a subtype of to-struct (we know they are not the same)
TypeCompare nbrMatches(INode *totype, INode *fromtype, SubtypeConstraint constraint) {
    // If coming from a ref, we cannot support type conversion
    if (constraint != Monomorph && constraint != Coercion)
        return NoMatch;

    // Bool is handled as a special case (also see iexpMatches)
    if (totype == (INode*)boolType)
        return NoMatch;

    if (totype->tag != fromtype->tag)
        return NoMatch;
    if (((NbrNode *)totype)->bits == ((NbrNode *)fromtype)->bits)
        return EqMatch;
    return ((NbrNode *)totype)->bits > ((NbrNode *)fromtype)->bits ? ConvSubtype : NoMatch;
}
