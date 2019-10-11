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

Name *plusEqName;   // "+="
Name *minusEqName;  // "-="
Name *multEqName;   // "*="
Name *divEqName;    // "/="
Name *remEqName;    // "%="
Name *orEqName;     // "|="
Name *andEqName;    // "&="
Name *xorEqName;    // "^="
Name *shlEqName;    // "<<="
Name *shrEqName;    // ">>="

Name *plusName;     // "+"
Name *minusName;    // "-"
Name *multName;     // "*"
Name *divName;      // "/"
Name *remName;      // "%"
Name *orName;       // "|"
Name *andName;      // "&"
Name *xorName;      // "^"
Name *shlName;      // "<<"
Name *shrName;      // ">>"

Name *incrName;     // "++"
Name *decrName;     // "--"
Name *incrPostName; // "_++"
Name *decrPostName; // "_--"

Name *eqName;       // "=="
Name *neName;       // "!="
Name *leName;       // "<="
Name *ltName;       // "<"
Name *geName;       // ">="
Name *gtName;       // ">"

Name *parensName;   // "()"
Name *indexName;    // "[]"
Name *refIndexName; // "&[]"

// Represents the absence of type information
INode *voidType;

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
AllocNode *ownAlloc;
AllocNode *rcAlloc;

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

IMethodNode *ptrType;
IMethodNode *refType;
IMethodNode *arrayRefType;

void stdlibInit(int ptrsize);
void keywordInit();
void stdNbrInit(int ptrsize);

#endif
