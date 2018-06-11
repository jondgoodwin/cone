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
FieldsAstNode *newStructNode() {
	FieldsAstNode *snode;
	newAstNode(snode, FieldsAstNode, StructType);
	snode->methods = newNodes(8);
	snode->fields = newNodes(8);
	return snode;
}

// Serialize a struct type
void structPrint(FieldsAstNode *node) {
	astFprint(node->asttype == StructType? "struct {}" : "alloc {}");
}

// Semantically analyze a struct type
void structPass(PassState *pstate, FieldsAstNode *node) {
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(node->fields, cnt, nodesp))
		astPass(pstate, *nodesp);
	for (nodesFor(node->methods, cnt, nodesp))
		astPass(pstate, *nodesp);
}

// Compare two struct signatures to see if they are equivalent
int structEqual(FieldsAstNode *node1, FieldsAstNode *node2) {
	// inodes must match exactly in order
	return 1;
}

// Will from struct coerce to a to struct (we know they are not the same)
int structCoerces(FieldsAstNode *to, FieldsAstNode *from) {
	return 1;
}