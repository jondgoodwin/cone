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
	NamedAstHdr;
	unsigned char nbytes;	// e.g., int32 uses 4 bytes
} NbrTypeAstNode;

// Primitive numeric types - for implicit (nondeclared but known) types
AstNode *i8Type;
AstNode *i16Type;
AstNode *i32Type;
AstNode *i64Type;
AstNode *u8Type;
AstNode *u16Type;
AstNode *u32Type;
AstNode *u64Type;
AstNode *f32Type;
AstNode *f64Type;

NbrTypeAstNode *newNbrTypeNode(uint16_t typ, char nbytes, Symbol *name);
void nbrTypePrint(int indent, NbrTypeAstNode *node, char* prefix);

#endif