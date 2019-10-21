/** Handling for primitive numbers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef number_h
#define number_h

// For primitives such as integer, unsigned integet, floats
typedef struct NbrNode {
    INsTypeNodeHdr;
    unsigned char bits;    // e.g., int32 uses 32 bits
} NbrNode;

void nbrTypePrint(NbrNode *node);
int isNbr(INode *node);

#endif