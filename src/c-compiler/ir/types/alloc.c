/** Allocate Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/memory.h"
#include "../../shared/error.h"
#include "../../parser/lexer.h"
#include "../nametbl.h"

#include <assert.h>

// Type check and expand code for allocator reference creator
// This actually generates additional code that replaces anode->exp
void allocAllocate(AddrNode *anode, PtrNode *ptype) {
	BlockNode *blknode = newBlockNode();

	// Find 'allocate' method in alloc
	Name *symalloc = nametblFind("allocate", 8);
	StructNode *alloctype = (StructNode*)ptype->alloc;
    VarDclNode *allocmeth = (VarDclNode*)methnodesFind(&alloctype->methprops, symalloc);
	if (allocmeth == NULL || ((FnSigNode*)allocmeth->vtype)->parms->used != 1) {
		errorMsgNode((INode*)ptype, ErrorBadAlloc, "Allocator is missing valid allocate method");
		return;
	}

	// szvtype = sizeof(vtype)
	Name *szvtsym = nametblFind("szvtype", 7);
	SizeofNode *szvtval = newSizeofNode();
	szvtval->type = ptype->pvtype;
	VarDclNode *szvtype = newVarDclNode(szvtsym, VarDclTag, (INode*)usizeType, immPerm, (INode*)szvtval);
	nodesAdd(&blknode->stmts, (INode*)szvtype);
	NameUseNode *szvtuse = newNameUseNode(szvtsym);
	szvtuse->asttype = VarNameUseTag;
	szvtuse->dclnode = (INamedNode*)szvtype;
	szvtuse->vtype = szvtype->vtype;

	// p1 = alloc.allocate(szvtype)
	NameUseNode *usealloc = newNameUseNode(symalloc);
	usealloc->asttype = TypeNameUseTag;
	usealloc->dclnode = (INamedNode*)allocmeth;
	usealloc->vtype = allocmeth->vtype;
	FnCallNode *callalloc = newFnCallNode((INode*)usealloc, 1);
	callalloc->vtype = allocmeth->vtype;
	nodesAdd(&callalloc->args, (INode*)szvtuse);
	// ---
	Name *pT = nametblFind("pT", 2);
	CastNode *castvt = newCastNode((INode*)callalloc, (INode*)ptype);
	VarDclNode *p1dcl = newVarDclNode(pT, VarDclTag, (INode*)ptype, immPerm, (INode*)castvt);
	nodesAdd(&blknode->stmts, (INode*)p1dcl);
	NameUseNode *p1use = newNameUseNode(pT);
	p1use->asttype = VarNameUseTag;
	p1use->dclnode = (INamedNode*)p1dcl;
	p1use->vtype = p1dcl->vtype;

	// *p1 = value
	DerefNode *derefp1 = newDerefNode();
	derefp1->exp = (INode*)p1use;
	derefp1->vtype = ptype->pvtype;
	AssignNode *copynode = newAssignNode(AssignTag, (INode*)derefp1, anode->exp);
	nodesAdd(&blknode->stmts, (INode*)copynode);

	// return p1
	nodesAdd(&blknode->stmts, (INode*)p1use);

	anode->exp = (INode*)blknode;
}