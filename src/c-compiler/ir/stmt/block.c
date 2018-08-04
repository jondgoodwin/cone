/** AST handling for block nodes: Program, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"
#include "../shared/error.h"

// Create a new block node
BlockAstNode *newBlockNode() {
	BlockAstNode *blk;
	newAstNode(blk, BlockAstNode, BlockNode);
	blk->vtype = voidType;
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

// Handle name resolution and control structure compliance for a block
// - push and pop a namespace context for hooking local vars in global name table
// - Ensure return/continue/break only appear as last statement in block
void blockResolvePass(PassState *pstate, BlockAstNode *blk) {
    int16_t oldscope = pstate->scope;
    blk->scope = ++pstate->scope; // Increment scope counter

    nametblHookPush(); // Ensure block's local variable declarations are hooked

    AstNode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        // Ensure 'return', 'break', 'continue' only appear as last statement in a block
        if (cnt > 1) {
            switch ((*nodesp)->asttype) {
            case ReturnNode:
                errorMsgNode(*nodesp, ErrorRetNotLast, "return may only appear as the last statement in a block"); break;
            case BreakNode:
                errorMsgNode(*nodesp, ErrorRetNotLast, "break may only appear as the last statement in a block"); break;
            case ContinueNode:
                errorMsgNode(*nodesp, ErrorRetNotLast, "continue may only appear as the last statement in a block"); break;
            }
        }
        nodeWalk(pstate, nodesp);
    }

    nametblHookPop();  // Unhook local variables from global name table
    pstate->scope = oldscope;
}

// Handle type-checking for a block. 
// Mostly this is a pass-through to type-check the block's statements.
// Note: When coerced by typeCoerces, vtype of the block will be specified
void blockTypePass(PassState *pstate, BlockAstNode *blk) {
    AstNode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        nodeWalk(pstate, nodesp);
    }
}

// Check the block's AST
void blockPass(PassState *pstate, BlockAstNode *blk) {
	switch (pstate->pass) {
	case NameResolution:
		blockResolvePass(pstate, blk); break;
	case TypeCheck:
        blockTypePass(pstate, blk); break;
	}
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

// Recursively strip 'returns' out of all block-ends in 'if' (see returnPass)
void ifRemoveReturns(IfAstNode *ifnode) {
	AstNode **nodesp;
	int16_t cnt;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		AstNode **laststmt;
		cnt--; nodesp++;
		laststmt = &nodesLast(((BlockAstNode*)*nodesp)->stmts);
		if ((*laststmt)->asttype == ReturnNode)
			*laststmt = ((ReturnAstNode*)*laststmt)->exp;
		if ((*laststmt)->asttype == IfNode)
			ifRemoveReturns((IfAstNode*)*laststmt);
	}
}

// Check the if statement's AST
void ifPass(PassState *pstate, IfAstNode *ifnode) {
	AstNode **nodesp;
	uint32_t cnt;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		nodeWalk(pstate, nodesp);

		// Type check the 'if':
		// - conditional must be a Bool
		// - if's vtype is specified/checked only when coerced by typeCoerces
		if (pstate->pass == TypeCheck) {
			if ((cnt & 1)==0 && *nodesp)
				typeCoerces((AstNode*)boolType, nodesp); // Conditional exp
		}
	}
}

// Create a new While node
WhileAstNode *newWhileNode() {
	WhileAstNode *node;
	newAstNode(node, WhileAstNode, WhileNode);
	node->blk = NULL;
	return node;
}

// Serialize the AST for an while block
void whilePrint(WhileAstNode *node) {
	astFprint("while ");
	astPrintNode(node->condexp);
	astPrintNL();
	astPrintNode(node->blk);
}

// Semantic pass on the while block
void whilePass(PassState *pstate, WhileAstNode *node) {
	uint16_t svflags = pstate->flags;
	pstate->flags |= PassWithinWhile;

	nodeWalk(pstate, &node->condexp);
	nodeWalk(pstate, &node->blk);

	if (pstate->pass == TypeCheck)
		typeCoerces((AstNode*)boolType, &node->condexp);

	pstate->flags = svflags;
}

// Semantic pass on break or continue
void breakPass(PassState *pstate, AstNode *node) {
	if (pstate->pass==NameResolution && !(pstate->flags & PassWithinWhile))
		errorMsgNode(node, ErrorNoWhile, "break/continue may only be used within a while/each block");
}

// Create a new intrinsic node
IntrinsicAstNode *newIntrinsicNode(int16_t intrinsic) {
	IntrinsicAstNode *intrinsicNode;
	newAstNode(intrinsicNode, IntrinsicAstNode, IntrinsicNode);
	intrinsicNode->intrinsicFn = intrinsic;
	return intrinsicNode;
}

// Serialize the AST for an intrinsic
void intrinsicPrint(IntrinsicAstNode *intrinsicNode) {
	astFprint("intrinsic function");
}

// Check the intrinsic node's AST
void intrinsicPass(PassState *pstate, IntrinsicAstNode *intrinsicNode) {
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

// Semantic analysis for return statements
// Related analysis for return elsewhere:
// - Block ensures that return can only appear at end of block
// - NameDcl turns fn block's final expression into an implicit return
void returnPass(PassState *pstate, ReturnAstNode *node) {
	// If we are returning the value from an 'if', recursively strip out any of its path's redudant 'return's
	if (pstate->pass == TypeCheck && node->exp->asttype == IfNode)
		ifRemoveReturns((IfAstNode*)(node->exp));

	// Process the return's expression
	nodeWalk(pstate, &node->exp);

	// Ensure the vtype of the expression can be coerced to the function's declared return type
	if (pstate->pass == TypeCheck) {
		if (!typeCoerces(pstate->fnsig->rettype, &node->exp)) {
			errorMsgNode(node->exp, ErrorInvType, "Return expression type does not match return type on function");
			errorMsgNode((AstNode*)pstate->fnsig->rettype, ErrorInvType, "This is the declared function's return type");
		}
	}
}
