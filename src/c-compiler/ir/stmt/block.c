/** AST handling for block nodes: Program, Block, etc.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"
#include "../../shared/error.h"

// Create a new block node
BlockAstNode *newBlockNode() {
	BlockAstNode *blk;
	newNode(blk, BlockAstNode, BlockNode);
	blk->vtype = voidType;
	blk->stmts = newNodes(8);
	return blk;
}

// Serialize the AST for a block
void blockPrint(BlockAstNode *blk) {
	INode **nodesp;
	uint32_t cnt;

	if (blk->stmts) {
		inodePrintIncr();
		for (nodesFor(blk->stmts, cnt, nodesp)) {
			inodePrintIndent();
			inodePrintNode(*nodesp);
			inodePrintNL();
		}
		inodePrintDecr();
	}
}

// Handle name resolution and control structure compliance for a block
// - push and pop a namespace context for hooking local vars in global name table
// - Ensure return/continue/break only appear as last statement in block
void blockResolvePass(PassState *pstate, BlockAstNode *blk) {
    int16_t oldscope = pstate->scope;
    blk->scope = ++pstate->scope; // Increment scope counter

    nametblHookPush(); // Ensure block's local variable declarations are hooked

    INode **nodesp;
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
        inodeWalk(pstate, nodesp);
    }

    nametblHookPop();  // Unhook local variables from global name table
    pstate->scope = oldscope;
}

// Handle type-checking for a block. 
// Mostly this is a pass-through to type-check the block's statements.
// Note: When coerced by typeCoerces, vtype of the block will be specified
void blockTypePass(PassState *pstate, BlockAstNode *blk) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        inodeWalk(pstate, nodesp);
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
	newNode(ifnode, IfAstNode, IfNode);
	ifnode->condblk = newNodes(4);
	ifnode->vtype = voidType;
	return ifnode;
}

// Serialize the AST for an if statement
void ifPrint(IfAstNode *ifnode) {
	INode **nodesp;
	uint32_t cnt;
	int firstflag = 1;

	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		if (firstflag) {
			inodeFprint("if ");
			firstflag = 0;
			inodePrintNode(*nodesp);
		}
		else {
			inodePrintIndent();
			if (*nodesp == voidType)
				inodeFprint("else");
			else {
				inodeFprint("elif ");
				inodePrintNode(*nodesp);
			}
		}
		inodePrintNL();
		inodePrintNode(*(++nodesp));
		cnt--;
	}
}

// Recursively strip 'returns' out of all block-ends in 'if' (see returnPass)
void ifRemoveReturns(IfAstNode *ifnode) {
	INode **nodesp;
	int16_t cnt;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		INode **laststmt;
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
	INode **nodesp;
	uint32_t cnt;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		inodeWalk(pstate, nodesp);

		// Type check the 'if':
		// - conditional must be a Bool
		// - if's vtype is specified/checked only when coerced by typeCoerces
		if (pstate->pass == TypeCheck) {
			if ((cnt & 1)==0 && *nodesp)
				typeCoerces((INode*)boolType, nodesp); // Conditional exp
		}
	}
}

// Create a new While node
WhileAstNode *newWhileNode() {
	WhileAstNode *node;
	newNode(node, WhileAstNode, WhileNode);
	node->blk = NULL;
	return node;
}

// Serialize the AST for an while block
void whilePrint(WhileAstNode *node) {
	inodeFprint("while ");
	inodePrintNode(node->condexp);
	inodePrintNL();
	inodePrintNode(node->blk);
}

// Semantic pass on the while block
void whilePass(PassState *pstate, WhileAstNode *node) {
	uint16_t svflags = pstate->flags;
	pstate->flags |= PassWithinWhile;

	inodeWalk(pstate, &node->condexp);
	inodeWalk(pstate, &node->blk);

	if (pstate->pass == TypeCheck)
		typeCoerces((INode*)boolType, &node->condexp);

	pstate->flags = svflags;
}

// Semantic pass on break or continue
void breakPass(PassState *pstate, INode *node) {
	if (pstate->pass==NameResolution && !(pstate->flags & PassWithinWhile))
		errorMsgNode(node, ErrorNoWhile, "break/continue may only be used within a while/each block");
}

// Create a new intrinsic node
IntrinsicAstNode *newIntrinsicNode(int16_t intrinsic) {
	IntrinsicAstNode *intrinsicNode;
	newNode(intrinsicNode, IntrinsicAstNode, IntrinsicNode);
	intrinsicNode->intrinsicFn = intrinsic;
	return intrinsicNode;
}

// Serialize the AST for an intrinsic
void intrinsicPrint(IntrinsicAstNode *intrinsicNode) {
	inodeFprint("intrinsic function");
}

// Check the intrinsic node's AST
void intrinsicPass(PassState *pstate, IntrinsicAstNode *intrinsicNode) {
}

// Create a new return statement node
ReturnAstNode *newReturnNode() {
	ReturnAstNode *node;
	newNode(node, ReturnAstNode, ReturnNode);
	node->exp = voidType;
	return node;
}

// Serialize the AST for a return statement
void returnPrint(ReturnAstNode *node) {
	inodeFprint("return ");
	inodePrintNode(node->exp);
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
	inodeWalk(pstate, &node->exp);

	// Ensure the vtype of the expression can be coerced to the function's declared return type
	if (pstate->pass == TypeCheck) {
		if (!typeCoerces(pstate->fnsig->rettype, &node->exp)) {
			errorMsgNode(node->exp, ErrorInvType, "Return expression type does not match return type on function");
			errorMsgNode((INode*)pstate->fnsig->rettype, ErrorInvType, "This is the declared function's return type");
		}
	}
}
