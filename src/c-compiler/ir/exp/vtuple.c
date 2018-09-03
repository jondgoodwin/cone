/** Handling for value tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new value tuple node
VTupleNode *newVTupleNode() {
	VTupleNode *tuple;
	newNode(tuple, VTupleNode, VTupleTag);
	tuple->vtype = voidType;
	tuple->values = newNodes(4);
	return tuple;
}

// Serialize a value tuple node
void vtuplePrint(VTupleNode *tuple) {
	INode **nodesp;
	uint32_t cnt;

	for (nodesFor(tuple->values, cnt, nodesp)) {
		inodePrintNode(*nodesp);
        if (cnt)
		    inodeFprint(",");
	}
}

// Check the value tuple node
void vtupleWalk(PassState *pstate, VTupleNode *tuple) {
	switch (pstate->pass) {
	case NameResolution:
		; break;
	case TypeCheck:
        ; break;
	}
}
