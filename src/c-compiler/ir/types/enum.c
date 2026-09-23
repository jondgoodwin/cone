/** The discriminant type of a closed variant type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new discriminant type node
EnumNode *newEnumNode() {
    EnumNode *node;
    newNode(node, EnumNode, EnumTag);
    node->bytes = 1;
    node->fixedwidth = 0;
    node->extended = 0;
    node->underlying = NULL;
    node->namesym = anonName;
    node->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)node, 8);
    return node;
}

// Serialize a discriminant type
void enumPrint(EnumNode *node) {
    inodeFprint("tag");
}

// The number of bytes needed to hold tag value 'maxtag'
uint8_t enumBytesFor(uint32_t maxtag) {
    if (maxtag <= 0xff)
        return 1;
    if (maxtag <= 0xffff)
        return 2;
    return 4;
}

// Name resolution of a discriminant type
void enumNameRes(NameResState *pstate, EnumNode *node) {
    if (node->underlying)
        inodeNameRes(pstate, &node->underlying);
}

// Type check a discriminant type.
//
// An enum may name the integer type its tag values are laid out in, which is
// what lines an enum up with an external library's constants. It fixes the
// width: the tag is that many bytes however many variants there are, and a
// pinned value too large for it is the author's error rather than a silent
// widening -- structTypeCheck says so, where the variants are known.
void enumTypeCheck(TypeCheckState *pstate, EnumNode *node) {
    if (node->underlying == NULL)
        return;
    if (!itypeTypeCheck(pstate, &node->underlying)) {
        node->underlying = NULL;
        return;
    }
    INode *dcl = itypeGetTypeDcl(node->underlying);
    if (dcl->tag != UintNbrTag && dcl->tag != IntNbrTag) {
        errorMsgNode(node->underlying, ErrorInvType,
            "An enum lays its tag values out in an integer type.");
        node->underlying = NULL;
        return;
    }
    unsigned char bits = ((NbrNode*)dcl)->bits;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        errorMsgNode(node->underlying, ErrorInvType,
            "An enum's integer type must be 8, 16, 32 or 64 bits wide.");
        node->underlying = NULL;
        return;
    }
    node->bytes = bits / 8;
    node->fixedwidth = 1;
}
