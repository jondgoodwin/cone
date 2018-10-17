/** Handling for if nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new If node
IfNode *newIfNode() {
	IfNode *ifnode;
	newNode(ifnode, IfNode, IfTag);
	ifnode->condblk = newNodes(4);
	ifnode->vtype = voidType;
	return ifnode;
}

// Serialize an if statement
void ifPrint(IfNode *ifnode) {
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
void ifRemoveReturns(IfNode *ifnode) {
	INode **nodesp;
	int16_t cnt;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		INode **laststmt;
		cnt--; nodesp++;
		laststmt = &nodesLast(((BlockNode*)*nodesp)->stmts);
		if ((*laststmt)->tag == ReturnTag)
			*laststmt = ((ReturnNode*)*laststmt)->exp;
		if ((*laststmt)->tag == IfTag)
			ifRemoveReturns((IfNode*)*laststmt);
	}
}

// Check the if statement node
void ifPass(PassState *pstate, IfNode *ifnode) {
	INode **nodesp;
	uint32_t cnt;
	for (nodesFor(ifnode->condblk, cnt, nodesp)) {
		inodeWalk(pstate, nodesp);

		// Type check the 'if':
		// - conditional must be a Bool
		// - if's vtype is specified/checked only when coerced by iexpCoerces
		if (pstate->pass == TypeCheck) {
			if ((cnt & 1)==0 && *nodesp)
				iexpCoerces((INode*)boolType, nodesp); // Conditional exp
		}
	}
}

// Perform data flow analysis on an if expression
void ifFlow(FlowState *fstate, IfNode **ifnodep) {
    IfNode *ifnode = *ifnodep;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(ifnode->condblk, cnt, nodesp)) {
        if (*nodesp != voidType)
            flowLoadValue(fstate, nodesp);
        nodesp++; cnt--;
        blockFlow(fstate, (BlockNode**)nodesp);
        flowAliasReset();
    }
}