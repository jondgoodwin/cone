/** Handling for primitive numbers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

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

    // A bool and a number have none: neither coerces to the other's type
    if (typ1 == boolType || typ2 == boolType)
        return NULL;
    // So have a char and a number: a char is a code point, no quantity
    if ((typ1 == charType) != (typ2 == charType))
        return NULL;
    return typ1->bits >= typ2->bits ? type1 : type2;
}

// Is from-type a subtype of to-struct (we know they are not the same)
TypeCompare nbrMatches(INode *totype, INode *fromtype, SubtypeConstraint constraint) {
    // If coming from a ref, we cannot support type conversion
    if (constraint != Monomorph && constraint != Coercion)
        return NoMatch;

    // bool is handled as a special case (also see iexpMatches). A number reaches
    // bool through isTrue; a bool reaches no number implicitly, though it is a
    // 1-bit unsigned: true is not a count, and 'T.from(b)' says it is 0 or 1.
    if (totype == (INode*)boolType || fromtype == (INode*)boolType)
        return NoMatch;

    // A char reaches no number implicitly, nor a number a char, though a char is
    // a 32-bit unsigned: a u8 above 127 would become a Latin-1 character, and a
    // u32 may be no code point. They convert explicitly, 'u8.from(c)'
    if (totype == (INode*)charType || fromtype == (INode*)charType)
        return NoMatch;

    if (totype->tag != fromtype->tag)
        return NoMatch;
    if (((NbrNode *)totype)->bits == ((NbrNode *)fromtype)->bits)
        return EqMatch;
    return ((NbrNode *)totype)->bits > ((NbrNode *)fromtype)->bits ? ConvSubtype : NoMatch;
}
