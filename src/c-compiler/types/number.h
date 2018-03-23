/** AST handling for primitive numbers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef number_h
#define number_h

// For primitives such as integer, unsigned integet, floats
typedef struct NbrAstNode {
	TypeAstHdr;
	unsigned char bits;	// e.g., int32 uses 32 bits
} NbrAstNode;

NbrAstNode *newNbrTypeNode(uint16_t typ, char bits);
void nbrTypePrint(NbrAstNode *node);
int isNbr(AstNode *node);

#endif