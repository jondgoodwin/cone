/** The discriminant type of a closed variant type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <stdio.h>
#include <inttypes.h>

// Create a new discriminant type node
EnumNode *newEnumNode() {
    EnumNode *node;
    newNode(node, EnumNode, EnumTag);
    node->bytes = 1;
    node->fixedwidth = 0;
    node->issigned = 0;
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

// Does a variant's tag value fit an integer 'bits' wide, signed or not? The
// literal rule, that a value must fit its type and that a negative sign counts
// (reftoken.html), read against the value as written. A negative value fits no
// unsigned integer: a tag is a constant lined up with another library's, so -4
// in a u32 enum is refused rather than taken as 0xFFFFFFFC.
int enumTagFits(struct StructNode *variant, unsigned int bits, int issigned) {
    if (variant->tagstate == TagNegative)
        return issigned && (bits >= 64 || variant->tagnbr >= -((int64_t)1 << (bits - 1)));
    uint64_t value = (uint64_t)variant->tagnbr;
    if (issigned)
        return value <= ((uint64_t)1 << (bits - 1)) - 1;
    return bits >= 64 || value <= ((uint64_t)1 << bits) - 1;
}

// The values an integer 'bits' wide holds, "from A to B", written into 'buf'
// (at least 64 bytes), which is returned. Worded as ErrorLitRange's.
char *enumRangeText(unsigned int bits, int issigned, char *buf) {
    if (issigned)
        sprintf(buf, "from %" PRId64 " to %" PRId64,
            (int64_t)(0 - ((uint64_t)1 << (bits - 1))), (int64_t)(((uint64_t)1 << (bits - 1)) - 1));
    else
        sprintf(buf, "from 0 to %" PRIu64, bits >= 64 ? UINT64_MAX : ((uint64_t)1 << bits) - 1);
    return buf;
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
    node->issigned = dcl->tag == IntNbrTag;
}
