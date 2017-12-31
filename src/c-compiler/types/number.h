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

// Primitive numeric types - for implicit (nondeclared but known) types
NbrAstNode *boolType;	// i1
NbrAstNode *i8Type;
NbrAstNode *i16Type;
NbrAstNode *i32Type;
NbrAstNode *i64Type;
NbrAstNode *u8Type;
NbrAstNode *u16Type;
NbrAstNode *u32Type;
NbrAstNode *u64Type;
NbrAstNode *f32Type;
NbrAstNode *f64Type;

void nbrDclNames();
NbrAstNode *newNbrTypeNode(uint16_t typ, char bits);
void nbrTypePrint(NbrAstNode *node);
int isNbr(AstNode *node);

#endif