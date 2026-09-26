/** Standard library initialiation
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef stdlib_h
#define stdlib_h

// Unique (unclonable) nodes representing the absence of value or type
extern INode *unknownType;   // Unknown/unspecified type
extern INode *noCareType;    // When the receiver does not care what type is returned
extern INode *errorType;     // The type of a node already reported as bad
extern INode *elseCond;   // node representing the 'else' condition for an 'if' node
extern INode *borrowRef;  // When a reference's region is unspecified, as it is borrowed

// Built-in permission types - for implicit (non-declared but known) permissions
extern PermNode *uniPerm;
extern PermNode *mutPerm;
extern PermNode *immPerm;
extern PermNode *roPerm;
extern PermNode *mut1Perm;
extern PermNode *opaqPerm;

// Built-in lifetimes
extern LifetimeNode *staticLifetimeNode;

// Primitive numeric types - for implicit (nondeclared but known) types
extern NbrNode *boolType;    // i1
extern NbrNode *i8Type;
extern NbrNode *i16Type;
extern NbrNode *i32Type;
extern NbrNode *i64Type;
extern NbrNode *isizeType;
extern NbrNode *u8Type;
extern NbrNode *u16Type;
extern NbrNode *u32Type;
extern NbrNode *u64Type;
extern NbrNode *usizeType;
extern NbrNode *f32Type;
extern NbrNode *f64Type;

extern INsTypeNode *ptrType;
extern INsTypeNode *refType;
extern INsTypeNode *arrayRefType;

// The two functions a program calls to run its stitched init and final
// (genlStitch): 'initAll()' and 'finalAll()', names every module reaches as it
// reaches 'i64', and which a declaration of its own hides
extern FnDclNode *initAllFn;
extern FnDclNode *finalAllFn;

// The built-in traits, each a name every module reaches unless it declares the
// name itself, and none with members: 'RegionRef', which a region ref struct
// declares with 'is'; and 'Move' and 'Copy', one of which every type has, the
// compiler granting it from what it infers and a type able to declare it
extern StructNode *regionRefTrait;
extern StructNode *moveTrait;
extern StructNode *copyTrait;

// Is this declaration one of the built-in traits?
int corelibIsBuiltinTrait(INode *node);

void stdlibInit(int ptrsize);
void keywordInit();
void stdNbrInit(int ptrsize);

#endif
