/** Handling for array types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef array_h
#define array_h

// Array node
typedef struct ArrayNode {
    INsTypeNodeHdr;
    uint32_t size;    // LLVM 5 C-interface is restricted to 32-bits
    INode *elemtype;
} ArrayNode;

ArrayNode *newArrayNode();

// Clone array
INode *cloneArrayNode(CloneState *cstate, ArrayNode *node);

// Create a new array type of a specified size and element type
ArrayNode *newArrayNodeTyped(size_t size, INode *elemtype);
void arrayPrint(ArrayNode *node);

// Name resolution of an array type
void arrayNameRes(NameResState *pstate, ArrayNode *node);

// Type check an array type
void arrayTypeCheck(TypeCheckState *pstate, ArrayNode *name);

int arrayEqual(ArrayNode *node1, ArrayNode *node2);

// Is from-type a subtype of to-struct (we know they are not the same)
TypeCompare arrayMatches(ArrayNode *to, ArrayNode *from, SubtypeConstraint constraint);

#endif