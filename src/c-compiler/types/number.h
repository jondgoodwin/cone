/** AST handling for primitive numbers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef number_h
#define number_h

// For primitives such as integer, unsigned integet, floats
typedef struct NbrTypeAstNode {
	TypedAstHdr;
	unsigned char nbytes;	// e.g., int32 uses 4 bytes
} NbrTypeAstNode;

// Primitive numeric types - for implicit (nondeclared but known) types
NbrTypeAstNode *i8Type;
NbrTypeAstNode *i16Type;
NbrTypeAstNode *i32Type;
NbrTypeAstNode *i64Type;
NbrTypeAstNode *u8Type;
NbrTypeAstNode *u16Type;
NbrTypeAstNode *u32Type;
NbrTypeAstNode *u64Type;
NbrTypeAstNode *f32Type;
NbrTypeAstNode *f64Type;

NbrTypeAstNode *newNbrTypeNode(uint16_t typ, char nbytes);
void nbrTypePrint(int indent, NbrTypeAstNode *node, char* prefix);

#endif