/** Compiler Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "type.h"
#include "memory.h"

#define primtype(dest, typ, nbyt) {\
	dest = (LangTypeInfo*) (ptype = allocTypeInfo(PrimTypeInfo)); \
	ptype->type = typ; \
	ptype->nbytes = nbyt; \
}

void typInit() {
	PrimTypeInfo *ptype;
	primtype(i8Type, IntType, 1);
	primtype(i16Type, IntType, 2);
	primtype(i32Type, IntType, 4);
	primtype(i64Type, IntType, 8);
	primtype(u8Type, UintType, 1);
	primtype(u16Type, UintType, 2);
	primtype(u32Type, UintType, 4);
	primtype(u64Type, UintType, 8);
	primtype(f32Type, FloatType, 4);
	primtype(f64Type, FloatType, 8);
}