/** Shared logic for method-based types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../ast/nametbl.h"
#include "../parser/lexer.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// Initialize methnodes metadata
void methnodesInit(MethNodes *mnodes, uint32_t size) {
    mnodes->avail = size;
    mnodes->used = 0;
    mnodes->nodes = (AstNode **)memAllocBlk(size * sizeof(AstNode **));
}

// Double size, if full
void methnodesGrow(MethNodes *mnodes) {
    AstNode **oldnodes;
    oldnodes = mnodes->nodes;
    mnodes->avail <<= 1;
    mnodes->nodes = (AstNode **)memAllocBlk(mnodes->avail * sizeof(AstNode **));
    memcpy(mnodes->nodes, oldnodes, mnodes->used * sizeof(AstNode **));
}

// Add an AstNode to the end of a MethNodes, growing it if full (changing its memory location)
void methnodesAdd(MethNodes *mnodes, AstNode *node) {
    if (mnodes->used >= mnodes->avail)
        methnodesGrow(mnodes);
    mnodes->nodes[mnodes->used++] = node;
}

// Find the desired named node.
// Return the node, if found or NULL if not found
NamedAstNode *methnodesFind(MethNodes *mnodes, Name *name) {
	AstNode **nodesp;
	uint32_t cnt;
	for (methnodesFor(mnodes, cnt, nodesp)) {
		if (isNamedNode(*nodesp)) {
			if (((NamedAstNode*)*nodesp)->namesym == name)
				return (NamedAstNode*)*nodesp;
		}
	}
	return NULL;
}
