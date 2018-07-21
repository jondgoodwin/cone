/** Shared logic for method-based types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../ast/nametbl.h"
#include "../parser/lexer.h"
#include "../shared/error.h"

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

// Add an AstNode to the end of a MethNodes, growing it if full (changing its memory location)
void methnodesAdd(MethNodes *mnodes, AstNode *node) {
    if (mnodes->used >= mnodes->avail)
        methnodesGrow(mnodes);
    mnodes->nodes[mnodes->used++] = node;
}

// Add a function or potentially overloaded method
// If method is overloaded, add it to the link chain of same named methods
void methnodesAddFn(MethNodes *mnodes, FnDclAstNode *fnnode) {
    FnDclAstNode *lastmeth = (FnDclAstNode *)methnodesFind(mnodes, fnnode->namesym);
    if (lastmeth && (lastmeth->asttype != FnDclTag
        || !(lastmeth->flags & FlagFnMethod) || !(fnnode->flags & FlagFnMethod))) {
        errorMsgNode((AstNode*)fnnode, ErrorDupName, "Duplicate name %s: Only methods can be overloaded.", &fnnode->namesym->namestr);
        return;
    }
    if (lastmeth) {
        while (lastmeth->nextnode)
            lastmeth = lastmeth->nextnode;
        lastmeth->nextnode = fnnode;
    }
    methnodesAdd(mnodes, (AstNode*)fnnode);
}

// Add a field
void methnodesAddField(MethNodes *mnodes,  VarDclAstNode *varnode) {
    FnDclAstNode *lastmeth = (FnDclAstNode *)methnodesFind(mnodes, varnode->namesym);
    if (lastmeth && (lastmeth->asttype != VarDclTag)) {
        errorMsgNode((AstNode*)varnode, ErrorDupName, "Duplicate name %s: Only methods can be overloaded.", &varnode->namesym->namestr);
        return;
    }
    methnodesAdd(mnodes, (AstNode*)varnode);
}