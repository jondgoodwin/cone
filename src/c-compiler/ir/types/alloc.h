/** Allocator types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef alloc_h
#define alloc_h

typedef struct AddrAstNode AddrAstNode;
typedef struct PtrAstNode PtrAstNode;

void allocAllocate(AddrAstNode *anode, PtrAstNode *ptype);

#endif