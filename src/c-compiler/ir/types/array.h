/** Handling for array types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef array_h
#define array_h

#define FlagArrNoSize 0x0001

// For pointers
typedef struct ArrayNode {
	IMethodNodeHdr;
	uint32_t size;    // LLVM 5 C-interface is restricted to 32-bits
	INode *elemtype;
} ArrayNode;

ArrayNode *newArrayNode();
void arrayPrint(ArrayNode *node);
void arrayPass(PassState *pstate, ArrayNode *name);
int arrayEqual(ArrayNode *node1, ArrayNode *node2);

#endif