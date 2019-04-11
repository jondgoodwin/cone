/** Handling for reference types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef reference_h
#define reference_h

#define FlagRefNull  0x0001

// Reference node
typedef struct RefNode {
    INodeHdr;
    INode *pvtype;    // Value type
    INode *perm;    // Permission
    INode *alloc;    // Allocator
    int16_t scope;    // Lifetime
} RefNode;

// Create a new reference type whose info will be filled in afterwards
RefNode *newRefNode();

// Create a new ArrayDerefNode from an ArrayRefNode
RefNode *newArrayDerefNodeFrom(RefNode *refnode);

// Is type a nullable reference?
int refIsNullable(INode *typenode);

// Serialize a reference type
void refPrint(RefNode *node);

// Name resolution of a reference node
void refNameRes(PassState *pstate, RefNode *node);

// Type check a reference node
void refPass(PassState *pstate, RefNode *name);

// Compare two reference signatures to see if they are equivalent
int refEqual(RefNode *node1, RefNode *node2);

// Will from reference coerce to a to reference (we know they are not the same)
int refMatches(RefNode *to, RefNode *from);

// If self needs to auto-ref or auto-deref, make sure it legally can
int refAutoRefCheck(INode *selfnode, INode *totype);

// Auto-ref or auto-deref self node, if it is legal
void refAutoRef(INode **selfnode, INode *totype);

#endif