/** AST Node List
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef nodes_h
#define nodes_h

typedef struct AstNode AstNode;

#include <stdint.h>

// Header for a variable-sized structure holding a list of AstNodes
// The nodes immediately follow the header
typedef struct Nodes {
	uint32_t used;
	uint32_t avail;
} Nodes;

// Helper Functions
Nodes *newNodes(int size);
void nodesAdd(Nodes **nodesp, AstNode *node);

#define nodesFor(node, cnt, nodesp) nodesp = (AstNode**)((node)+1), cnt = (node)->used; cnt; cnt--, nodesp++

#endif