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

// Find best acceptable method (across overloaded methods) whose signature matches argument types
FnDclAstNode *methtypeFindMethod(AstNode *objtype, Name *methsym, Nodes *args, AstNode *callnode) {
    if (objtype->asttype == RefType || objtype->asttype == PtrType)
        objtype = typeGetVtype(((PtrAstNode *)objtype)->pvtype);
    if (!isMethodType(objtype)) {
        errorMsgNode(callnode, ErrorNoMeth, "Object's type does not support methods or fields.");
        return NULL;
    }

    // Do lookup, then handle if it is just a single method
    NamedAstNode *foundnode = methnodesFind(&((MethodTypeAstNode*)objtype)->methfields, methsym);
    if (!foundnode || foundnode->asttype != FnDclTag || !(foundnode->flags & FlagFnMethod)) {
        errorMsgNode(callnode, ErrorNoMeth, "Object's type does not support a method of this name.");
        return NULL;
    }

    // Look for best-fit method
    FnDclAstNode *bestmethod = NULL;
    int bestnbr = 0x7fffffff; // ridiculously high number    
    for (FnDclAstNode *methnode = (FnDclAstNode *)foundnode; methnode; methnode = methnode->nextnode) {
        int match;
        switch (match = fnSigMatchesCall((FnSigAstNode *)methnode->vtype, args)) {
        case 0: continue;		// not an acceptable match
        case 1: return methnode;	// perfect match!
        default:				// imprecise match using conversions
            if (match < bestnbr) {
                // Remember this as best found so far
                bestnbr = match;
                bestmethod = methnode;
            }
        }
    }
    return bestmethod;
}