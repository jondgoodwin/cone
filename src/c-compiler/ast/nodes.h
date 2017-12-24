/** AST Node List
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef nodes_h
#define nodes_h

typedef struct AstNode AstNode;
typedef struct Symbol Symbol;

#include <stdint.h>

// *** Nodes: Dynamically-sized array of AstNodes ***

// Header for a variable-sized structure holding a list of AstNodes
// The nodes immediately follow the header
typedef struct Nodes {
	uint32_t used;
	uint32_t avail;
} Nodes;

// Helper Functions
Nodes *newNodes(int size);
void nodesAdd(Nodes **nodesp, AstNode *node);

#define nodesNodes(node) ((AstNode**)((node)+1))
#define nodesFor(node, cnt, nodesp) nodesp = (AstNode**)((node)+1), cnt = (node)->used; cnt; cnt--, nodesp++
#define nodesGet(node, index) ((AstNode**)((node)+1))[index]

// *** Inodes: Hash-indexed & ordered array of Symbol:AstNode pairs ***

// Header for a variable-sized structure holding a list of Symbol:AstNode pairs
// The SymNode pairs immediately follow the header
typedef struct Inodes {
	uint32_t used;
	uint32_t avail;
} Inodes;

typedef struct SymNode {
	Symbol *name;
	AstNode *node;
} SymNode;

Inodes *newInodes(int size);
void inodesAdd(Inodes **nodesp, Symbol *name, AstNode *node);
SymNode *inodesFind(Inodes *inodes, Symbol *name);
void inodesHook(Inodes *inodes);
void inodesUnhook(Inodes *inodes);

#define inodesNodes(node) ((SymNode*)((node)+1))
#define inodesFor(node, cnt, nodesp) nodesp = (SymNode*)((node)+1), cnt = (node)->used; cnt; cnt--, nodesp++
#define inodesGet(node, index) ((SymNode*)((node)+1))[index]

#endif