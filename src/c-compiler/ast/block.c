/** AST handling for block nodes: Program, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"
#include "../shared/error.h"

// Create a new Program node
PgmAstNode *newPgmNode() {
	PgmAstNode *pgm;
	newAstNode(pgm, PgmAstNode, PgmNode);
	pgm->nodes = newNodes(8);
	return pgm;
}

// Serialize the AST for a program
void pgmPrint(PgmAstNode *pgm) {
	AstNode **nodesp;
	uint32_t cnt;

	astFprint("AST for program %s\n", pgm->lexer->url);
	astPrintIncr();
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		astPrintIndent();
		astPrintNode(*nodesp);
		astPrintNL();
	}
	astPrintDecr();
}

// Check the program's AST
void pgmPass(AstPass *pstate, PgmAstNode *pgm) {
	AstNode **nodesp;
	uint32_t cnt;

	// For global variables and functions, handle all their type info first
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		if ((*nodesp)->asttype == VarNameDclNode) {
			NameDclAstNode *name = (NameDclAstNode*)*nodesp;
			astPass(pstate, (AstNode*)name->perm);
			astPass(pstate, name->vtype);
		}
	}
	if (errors)
		return;

	// Now we can process the full node info
	for (nodesFor(pgm->nodes, cnt, nodesp)) {
		astPass(pstate, *nodesp);
	}
}

// Create a new block node
BlockAstNode *newBlockNode() {
	BlockAstNode *blk;
	newAstNode(blk, BlockAstNode, BlockNode);
	blk->vtype = voidType;
	blk->locals = newInodes(8);
	blk->stmts = newNodes(8);
	return blk;
}

// Serialize the AST for a block
void blockPrint(BlockAstNode *blk) {
	AstNode **nodesp;
	uint32_t cnt;

	if (blk->stmts) {
		astPrintIncr();
		for (nodesFor(blk->stmts, cnt, nodesp)) {
			astPrintIndent();
			astPrintNode(*nodesp);
			astPrintNL();
		}
		astPrintDecr();
	}
}

// Check the block's AST
void blockPass(AstPass *pstate, BlockAstNode *blk) {
	int16_t oldscope = pstate->scope;
	blk->scope = ++pstate->scope; // Increment scope counter
	BlockAstNode *oldblk = pstate->blk;
	pstate->blk = blk;

	// Traverse block info
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(blk->stmts, cnt, nodesp))
		astPass(pstate, *nodesp);

	switch (pstate->pass) {
	case NameResolution:
		// Unhook local variables that hooked themselves
		inodesUnhook(blk->locals); break;

	case TypeCheck:
		// Type of the block is the type of its last statement
		blk->vtype = ((TypedAstNode*)nodesLast(blk->stmts))->vtype; break;
	}

	pstate->blk = oldblk;
	pstate->scope = oldscope;
}

// Create a new If node
IfAstNode *newIfNode() {
	IfAstNode *ifnode;
	newAstNode(ifnode, IfAstNode, IfNode);
	ifnode->condblk = newNodes(4);
	ifnode->vtype = voidType;
	return ifnode;
}

// Serialize the AST for an if statement
void ifPrint(IfAstNode *ifnode) {
	AstNode **nodesp;
	uint32_t cnt;
	int firstflag = 1;

	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		if (firstflag) {
			astFprint("if ");
			firstflag = 0;
			astPrintNode(*nodesp);
		}
		else {
			astPrintIndent();
			if (*nodesp == voidType)
				astFprint("else");
			else {
				astFprint("elif ");
				astPrintNode(*nodesp);
			}
		}
		astPrintNL();
		astPrintNode(*(++nodesp));
		cnt--;
	}
}

// Check the if statement's AST
void ifPass(AstPass *pstate, IfAstNode *ifnode) {
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		astPass(pstate, *nodesp);

		if (pstate->pass == TypeCheck) {
			if (cnt & 1) {
				// vtype of 'if' node is that of all blocks, if they are the same
				// Otherwise it is voidType
				if (cnt == ifnode->condblk->used - 1)
					ifnode->vtype = ((TypedAstNode*)*nodesp)->vtype;
				if (!typeIsSame(ifnode->vtype, ((TypedAstNode*)*nodesp)->vtype))
					ifnode->vtype = voidType;
			}
			else {
				// Conditional expression should be a boolean
				if (*nodesp)
					typeCoerces((AstNode*)boolType, nodesp);
			}
		}
	}
}

// Create a new op code node
OpCodeAstNode *newOpCodeNode(int16_t opcode) {
	OpCodeAstNode *op;
	newAstNode(op, OpCodeAstNode, OpCodeNode);
	op->opcode = opcode;
	return op;
}

// Serialize the AST for an opcode
void opCodePrint(OpCodeAstNode *op) {
	astFprint("op code");
}

// Check the op code's AST
void opCodePass(AstPass *pstate, OpCodeAstNode *op) {
}

// Create a new return statement node
ReturnAstNode *newReturnNode() {
	ReturnAstNode *node;
	newAstNode(node, ReturnAstNode, ReturnNode);
	node->exp = voidType;
	return node;
}

// Serialize the AST for a return statement
void returnPrint(ReturnAstNode *node) {
	astFprint("return ");
	astPrintNode(node->exp);
}

// Check the AST for a return statement
void returnPass(AstPass *pstate, ReturnAstNode *node) {
	astPass(pstate, node->exp);
	// Ensure the vtype of the expression can be coerced to the function's declared return type
	if (pstate->pass == TypeCheck) {
		if (!typeCoerces(pstate->fnsig->rettype, &node->exp)) {
			errorMsgNode(node->exp, ErrorInvType, "Return expression type does not match return type on function");
			errorMsgNode((AstNode*)pstate->fnsig->rettype, ErrorInvType, "This is the declared function's return type");
		}
	}
}
