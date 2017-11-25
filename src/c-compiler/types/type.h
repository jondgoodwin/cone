/** Compiler types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef type_h
#define type_h

#include "../shared/ast.h"
#include <stdint.h>
typedef struct Symbol Symbol;

// All the possible types for a language type
enum LangTypes {
	VoidType,	// represening no values, e.g., no return values on a fn

	// PrimTypeAstNode
	IntType,	// Integer
	UintType,	// Unsigned integer
	FloatType,	// Floating point number

	PtrType,	// Also smart pointers?

	ArrayType,	// Also dynamic arrays? SOA?

	FnType,		// Also method, closure, behavior, co-routine, thread, ...

	StructType,	// Also class, interface, actor, etc.?

	EnumType,	// Also sum type, etc.?

	ModuleType,	// Modules, Generics ?

	// Type types
	ValueType,	// Value Type
	PermType,	// Permission
	AllocType,	// Allocator

	NbrLangTypes
};

#define allocTypeAstNode(TypeAstNode) ((TypeAstNode *) memAllocBlk(sizeof(TypeAstNode)))

// For primitives such as integer, unsigned integet, floats
typedef struct PrimTypeAstNode {
	AstNodeHdr;
	unsigned char nbytes;	// e.g., int32 uses 4 bytes
} PrimTypeAstNode;

// For function signatures
typedef struct FnTypeAstNode {
	AstNodeHdr;
	AstNode *rettype;	// return type
	// named parameters and their types
} FnTypeAstNode;

// For pointers
typedef struct PtrTypeAstNode {
	AstNodeHdr;
	unsigned char nbytes;	// e.g., 32-bit uses 4 bytes
	unsigned char subtype;	// Simple, vtabled
	AstNode *ptrtotype;	// Type of value pointer points to
} PtrTypeAstNode;

// For arrays
typedef struct ArrTypeAstNode {
	AstNodeHdr;
	uint32_t nbrelems;		// Number of elements
	AstNode *elemtype;	// Type of array's elements
} ArrTypeAstNode;

// For identifiers that are types rather than values
typedef struct TypeTypeAstNode {
	AstNodeHdr;
	unsigned char subtype;
	AstNode *TypeAstNode;
} TypeTypeAstNode;

// The multi-dimensional type info for a value
typedef struct QuadTypeAstNode {
	AstNodeHdr;
	Symbol *name;
	AstNode *valtype;
	AstNode *permtype;
	AstNode *alloctype;
	AstNode *lifetype;
} QuadTypeAstNode;

#include "permission.h"

// Primitive numeric types
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

// Built-in permission types
AstNode *mutPerm;
AstNode *mmutPerm;
AstNode *immPerm;
AstNode *constPerm;
AstNode *constxPerm;
AstNode *mutxPerm;
AstNode *idPerm;

AstNode *voidType;

// Communication block between function impl and type parser
typedef struct TypeAndName {
	FnTypeAstNode *TypeAstNode;
	struct Symbol *symname;	// NULL = no name specified
} TypeAndName;

void typInit();

#endif