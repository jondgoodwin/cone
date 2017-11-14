/** Compiler types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef type_h
#define type_h

#include <stdint.h>

// All the possible types for a language type
enum LangTypes {
	// PrimTypeInfo
	IntType,	// Integer
	UintType,	// Unsigned integer
	FloatType,	// Floating point number

	PtrType,	// Also smart pointers?

	ArrayType,	// Also dynamic arrays? SOA?

	FuncType,	// Also method, closure, behavior, co-routine, thread, ...

	StructType,	// Also class, interface, actor, etc.?

	EnumType,	// Also sum type, etc.?

	ModuleType,	// Modules, Generics ?

	// Type types
	ValueType,	// Value Type
	PermType,	// Permission
	AllocType,	// Allocator

	NbrLangTypes
};

#define allocTypeInfo(TypeInfo) ((TypeInfo *) memAllocBlk(sizeof(TypeInfo)))

#define TypeHeader uint16_t type

// Generic type info structure for all types
typedef struct LangTypeInfo {
	TypeHeader;
} LangTypeInfo;

// For primitives such as integer, unsigned integet, floats
typedef struct PrimTypeInfo {
	TypeHeader;
	unsigned char nbytes;	// e.g., int32 uses 4 bytes
} PrimTypeInfo;

// For pointers
typedef struct PtrTypeInfo {
	TypeHeader;
	unsigned char nbytes;	// e.g., 32-bit uses 4 bytes
	unsigned char subtype;	// Simple, vtabled
	LangTypeInfo *ptrtotype;	// Type of value pointer points to
} PtrTypeInfo;

// For arrays
typedef struct ArrTypeInfo {
	TypeHeader;
	uint32_t nbrelems;		// Number of elements
	LangTypeInfo *elemtype;	// Type of array's elements
} ArrTypeInfo;

// For identifiers that are types rather than values
typedef struct TypeTypeInfo {
	TypeHeader;
	unsigned char subtype;
	LangTypeInfo *typeinfo;
} TypeTypeInfo;

LangTypeInfo *i8Type;
LangTypeInfo *i16Type;
LangTypeInfo *i32Type;
LangTypeInfo *i64Type;
LangTypeInfo *u8Type;
LangTypeInfo *u16Type;
LangTypeInfo *u32Type;
LangTypeInfo *u64Type;
LangTypeInfo *f32Type;
LangTypeInfo *f64Type;

void typInit();

#endif