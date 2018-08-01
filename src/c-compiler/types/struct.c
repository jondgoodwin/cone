/** AST handling for structs
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"

// Create a new struct type whose info will be filled in afterwards
StructAstNode *newStructNode(Name *namesym) {
	StructAstNode *snode;
	newAstNode(snode, StructAstNode, StructType);
    snode->vtype = NULL;
    snode->owner = NULL;
    snode->namesym = namesym;
    snode->llvmtype = NULL;
    snode->subtypes = newNodes(0);
    methnodesInit(&snode->methprops, 8);
	return snode;
}

// Serialize a struct type
void structPrint(StructAstNode *node) {
	astFprint(node->asttype == StructType? "struct %s {}" : "alloc %s {}", &node->namesym->namestr);
}

// Semantically analyze a struct type
void structPass(PassState *pstate, StructAstNode *node) {
    nametblHookPush();
    AstNode **nodesp;
    uint32_t cnt;
    for (methnodesFor(&node->methprops, cnt, nodesp)) {
        if (isNamedNode(*nodesp))
            nametblHookNode((NamedAstNode*)*nodesp);
    }
    for (methnodesFor(&node->methprops, cnt, nodesp)) {
        astPass(pstate, (AstNode*)*nodesp);
    }
    nametblHookPop();
}

// Compare two struct signatures to see if they are equivalent
int structEqual(StructAstNode *node1, StructAstNode *node2) {
	// inodes must match exactly in order
	return 1;
}

// Will from struct coerce to a to struct (we know they are not the same)
int structCoerces(StructAstNode *to, StructAstNode *from) {
	return 1;
}