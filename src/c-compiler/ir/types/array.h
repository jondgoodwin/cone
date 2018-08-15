/** AST handling for array types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef array_h
#define array_h

// For pointers
typedef struct ArrayNode {
	IMethodNodeHdr;
	uint32_t size;
	INode *elemtype;
} ArrayNode;

ArrayNode *newArrayNode();
void arrayPrint(ArrayNode *node);
void arrayPass(PassState *pstate, ArrayNode *name);
int arrayEqual(ArrayNode *node1, ArrayNode *node2);

#endif