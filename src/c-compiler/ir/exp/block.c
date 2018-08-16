/** Handling for block nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new block node
BlockNode *newBlockNode() {
	BlockNode *blk;
	newNode(blk, BlockNode, BlockTag);
	blk->vtype = voidType;
	blk->stmts = newNodes(8);
	return blk;
}

// Serialize a block node
void blockPrint(BlockNode *blk) {
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
void blockResolvePass(PassState *pstate, BlockNode *blk) {
    int16_t oldscope = pstate->scope;
    blk->scope = ++pstate->scope; // Increment scope counter

    nametblHookPush(); // Ensure block's local variable declarations are hooked

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        // Ensure 'return', 'break', 'continue' only appear as last statement in a block
        if (cnt > 1) {
            switch ((*nodesp)->tag) {
            case ReturnTag:
                errorMsgNode(*nodesp, ErrorRetNotLast, "return may only appear as the last statement in a block"); break;
            case BreakTag:
                errorMsgNode(*nodesp, ErrorRetNotLast, "break may only appear as the last statement in a block"); break;
            case ContinueTag:
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
void blockTypePass(PassState *pstate, BlockNode *blk) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(blk->stmts, cnt, nodesp)) {
        inodeWalk(pstate, nodesp);
    }
}

// Check the block node
void blockPass(PassState *pstate, BlockNode *blk) {
	switch (pstate->pass) {
	case NameResolution:
		blockResolvePass(pstate, blk); break;
	case TypeCheck:
        blockTypePass(pstate, blk); break;
	}
}
