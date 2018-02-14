/** AST handling for structs
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"

// Create a new struct type whose info will be filled in afterwards
StructAstNode *newStructNode() {
	StructAstNode *snode;
	newAstNode(snode, StructAstNode, StructType);
	snode->methods = newNodes(8);
	snode->fields = newInodes(8);
	return snode;
}

// Serialize a struct type
void structPrint(StructAstNode *node) {
	astFprint(node->asttype == StructType? "struct {}" : "alloc {}");
}

// Semantically analyze a struct type
void structPass(AstPass *pstate, StructAstNode *node) {
	SymNode *inodesp;
	AstNode **nodesp;
	uint32_t cnt;
	for (inodesFor(node->fields, cnt, inodesp))
		astPass(pstate, inodesp->node);
	for (nodesFor(node->methods, cnt, nodesp))
		astPass(pstate, *nodesp);
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