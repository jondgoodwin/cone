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
    INode *perm;      // Permission
    INode *region;    // Region
    uint16_t scope;   // Lifetime
} RefNode;

// Create a new reference type whose info will be filled in afterwards
RefNode *newRefNode();

// Clone reference
INode *cloneRefNode(CloneState *cstate, RefNode *node);

// Create a new reference type whose info is known and analyzeable
RefNode *newRefNodeFull(INode *region, INode *perm, INode *vtype);

// Set the inferred value type of a reference
void refSetPermVtype(RefNode *refnode, INode *perm, INode *vtype);

// Create a new ArrayDerefNode from an ArrayRefNode
RefNode *newArrayDerefNodeFrom(RefNode *refnode);

// Is type a nullable reference?
int refIsNullable(INode *typenode);

// Serialize a reference type
void refPrint(RefNode *node);

// Name resolution of a reference node
void refNameRes(NameResState *pstate, RefNode *node);

// Type check a reference node
void refTypeCheck(TypeCheckState *pstate, RefNode *name);

// Type check a virtual reference node
void refvirtTypeCheck(TypeCheckState *pstate, RefNode *node);

// Compare two reference signatures to see if they are equivalent
int refEqual(RefNode *node1, RefNode *node2);

// Will from reference coerce to a to reference (we know they are not the same)
int refMatches(RefNode *to, RefNode *from);

// Will from reference coerce to a virtual reference (we know they are not the same)
int refvirtMatches(RefNode *to, RefNode *from);

#endif