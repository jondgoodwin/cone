/** Standard library initialiation
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef stdlib_h
#define stdlib_h

// Represents the absence of type information
AstNode *voidType;

// Built-in permission types - for implicit (non-declared but known) permissions
PermAstNode *uniPerm;
PermAstNode *mutPerm;
PermAstNode *immPerm;
PermAstNode *constPerm;
PermAstNode *mutxPerm;
PermAstNode *idPerm;

// Primitive numeric types - for implicit (nondeclared but known) types
NbrAstNode *boolType;	// i1
NbrAstNode *i8Type;
NbrAstNode *i16Type;
NbrAstNode *i32Type;
NbrAstNode *i64Type;
NbrAstNode *isizeType;
NbrAstNode *u8Type;
NbrAstNode *u16Type;
NbrAstNode *u32Type;
NbrAstNode *u64Type;
NbrAstNode *usizeType;
NbrAstNode *f32Type;
NbrAstNode *f64Type;

void stdlibInit();
void keywordInit();
void stdNbrInit();

#endif
