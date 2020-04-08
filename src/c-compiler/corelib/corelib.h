/** Standard library initialiation
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef stdlib_h
#define stdlib_h

// Unique (unclonable) nodes representing the absence of value or type
INode *unknownType;   // Unknown/unspecified type
INode *noCareType;    // When the receiver does not care what type is returned
INode *elseCond;   // node representing the 'else' condition for an 'if' node
INode *borrowRef;  // When a reference's region is unspecified, as it is borrowed
INode *noValue;    // For return and break nodes that do not "return" a value

// Built-in permission types - for implicit (non-declared but known) permissions
PermNode *uniPerm;
PermNode *mutPerm;
PermNode *immPerm;
PermNode *constPerm;
PermNode *mut1Perm;
PermNode *opaqPerm;

// Built-in lifetimes
LifetimeNode *staticLifetimeNode;

// Built-in allocator types
AllocNode *soRegion;
AllocNode *rcRegion;

// Primitive numeric types - for implicit (nondeclared but known) types
NbrNode *boolType;    // i1
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

INsTypeNode *ptrType;
INsTypeNode *refType;
INsTypeNode *arrayRefType;

char *stdlibInit(int ptrsize);
void keywordInit();
void stdNbrInit(int ptrsize);

#endif
