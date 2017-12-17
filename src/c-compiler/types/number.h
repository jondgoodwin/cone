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
	TypedAstHdr;
	unsigned char nbytes;	// e.g., int32 uses 4 bytes
} NbrAstNode;

// Primitive numeric types - for implicit (nondeclared but known) types
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
NbrAstNode *newNbrTypeNode(uint16_t typ, char nbytes);
void nbrTypePrint(NbrAstNode *node);

#endif