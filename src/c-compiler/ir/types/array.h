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
	IMethodNodeHdr;
	uint32_t size;    // LLVM 5 C-interface is restricted to 32-bits
	INode *elemtype;
} ArrayNode;

ArrayNode *newArrayNode();
// Create a new array type of a specified size and element type
ArrayNode *newArrayNodeTyped(size_t size, INode *elemtype);
void arrayPrint(ArrayNode *node);
void arrayPass(PassState *pstate, ArrayNode *name);
int arrayEqual(ArrayNode *node1, ArrayNode *node2);

#endif