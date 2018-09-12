/** Standard library initialiation
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef stdlib_h
#define stdlib_h

// Common symbols
Name *anonName;  // "_" - the absence of a name
Name *selfName;  // "self"
Name *thisName;  // "this"

// Represents the absence of type information
INode *voidType;

// Built-in permission types - for implicit (non-declared but known) permissions
PermNode *uniPerm;
PermNode *mutPerm;
PermNode *immPerm;
PermNode *constPerm;
PermNode *mutxPerm;
PermNode *idPerm;

// Primitive numeric types - for implicit (nondeclared but known) types
NbrNode *boolType;	// i1
NbrNode *i8Type;
NbrNode *i16Type;
NbrNode *i32Type;
NbrNode *i64Type;
NbrNode *isizeType;
NbrNode *u8Type;
NbrNode *u16Type;
NbrNode *u32Type;
NbrNode *u64Type;
NbrNode *usizeType;
NbrNode *f32Type;
NbrNode *f64Type;

RefNode *strType;

void stdlibInit();
void keywordInit();
void stdNbrInit();

#endif
