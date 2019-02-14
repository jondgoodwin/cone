/** Handling for array reference (slice) type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef arrayref_h
#define arrayref_h

// #define FlagRefNull  0x0001

// Reference node
typedef struct ArrayRefNode {
    INodeHdr;
    INode *pvtype;    // Value type of each slice's element
    INode *perm;      // Permission
    INode *alloc;     // Allocator
    int16_t scope;    // Lifetime
} ArrayRefNode;

// Create a new array reference type whose info will be filled in afterwards
ArrayRefNode *newArrayRefNode();

// Is type a nullable array reference?
int arrayRefIsNullable(INode *typenode);

// Serialize a pointer type
void arrayRefPrint(ArrayRefNode *node);

// Semantically analyze a reference node
void arrayRefPass(PassState *pstate, ArrayRefNode *name);

// Compare two reference signatures to see if they are equivalent
int arrayRefEqual(ArrayRefNode *node1, ArrayRefNode *node2);

// Will from reference coerce to a to reference (we know they are not the same)
int arrayRefMatches(ArrayRefNode *to, ArrayRefNode *from);

#endif