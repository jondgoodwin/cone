/** Handling for reference types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef reference_h
#define reference_h

#define FlagArrRef 0x0001

// Reference node
typedef struct RefNode {
	INodeHdr;
	INode *pvtype;	// Value type
	INode *perm;	// Permission
	INode *alloc;	// Allocator
	int16_t scope;	// Lifetime
} RefNode;

// Create a new reference type whose info will be filled in afterwards
RefNode *newRefNode();

// Serialize a pointer type
void refPrint(RefNode *node);

// Semantically analyze a reference node
void refPass(PassState *pstate, RefNode *name);

// Compare two reference signatures to see if they are equivalent
int refEqual(RefNode *node1, RefNode *node2);

// Will from reference coerce to a to reference (we know they are not the same)
int refMatches(RefNode *to, RefNode *from);

#endif