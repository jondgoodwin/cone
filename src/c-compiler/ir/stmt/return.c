/** Handling for return nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new return statement node
ReturnNode *newReturnNode() {
	ReturnNode *node;
	newNode(node, ReturnNode, ReturnTag);
	node->exp = voidType;
    node->dealias = NULL;
	return node;
}

// Serialize a return statement
void returnPrint(ReturnNode *node) {
	inodeFprint(node->tag == BlockRetTag? "blockret " : "return ");
	inodePrintNode(node->exp);
}

// Semantic analysis for return statements
// Related analysis for return elsewhere:
// - Block ensures that return can only appear at end of block
// - NameDcl turns fn block's final expression into an implicit return
void returnPass(PassState *pstate, ReturnNode *node) {
	// If we are returning the value from an 'if', recursively strip out any of its path's redudant 'return's
	if (pstate->pass == TypeCheck && node->exp->tag == IfTag)
		ifRemoveReturns((IfNode*)(node->exp));

	// Process the return's expression
	inodeWalk(pstate, &node->exp);

	// Ensure the vtype of the expression can be coerced to the function's declared return type
	if (pstate->pass == TypeCheck) {
        if (pstate->fnsig->rettype->tag == TTupleTag) {
            if (node->exp->tag != VTupleTag) {
                errorMsgNode(node->exp, ErrorBadTerm, "Not enough return values");
                return;
            }
            Nodes *retnodes = ((VTupleNode*)node->exp)->values;
            Nodes *rettypes = ((TTupleNode*)pstate->fnsig->rettype)->types;
            if (rettypes->used > retnodes->used) {
                errorMsgNode(node->exp, ErrorBadTerm, "Not enough return values");
                return;
            }
            int16_t retcnt;
            INode **rettypesp;
            INode **retnodesp = &nodesGet(retnodes, 0);
            for (nodesFor(rettypes, retcnt, rettypesp)) {
                if (iexpCoerces(*rettypesp, retnodesp++) == 0)
                    errorMsgNode(*(retnodesp-1), ErrorInvType, "Return value's type does not match fn return type");
            }

        }
        else {
            if (!iexpCoerces(pstate->fnsig->rettype, &node->exp)) {
                errorMsgNode(node->exp, ErrorInvType, "Return expression type does not match return type on function");
                errorMsgNode((INode*)pstate->fnsig->rettype, ErrorInvType, "This is the declared function's return type");
            }
        }
	}
}
