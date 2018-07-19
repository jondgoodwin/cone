/** AST handling for method tuples (multiple methods with the same name)
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"

// Create a new FnTuple whose info will be filled in afterwards
FnTupleAstNode *newFnTupleNode(Name *name) {
	FnTupleAstNode *node;
	newAstNode(node, FnTupleAstNode, FnTupleNode);
	node->methods = newNodes(8);
    node->namesym = name;
	return node;
}

// Serialize a fnTuple type
void fnTuplePrint(FnTupleAstNode *node) {
	astFprint("Method tuple:");
	astPrintNL();

	AstNode **nodesp;
	uint32_t cnt;
	astPrintIncr();
	for (nodesFor(node->methods, cnt, nodesp)) {
		astPrintIndent();
		astPrintNode(*nodesp);
		astPrintNL();
	}
	astPrintDecr();
}

// Semantically analyze a fn tuple
void fnTuplePass(PassState *pstate, FnTupleAstNode *node) {
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(node->methods, cnt, nodesp))
		astPass(pstate, *nodesp);
}
