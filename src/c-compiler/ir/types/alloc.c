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
void allocAllocate(AddrAstNode *anode, PtrAstNode *ptype) {
	BlockAstNode *blknode = newBlockNode();

	// Find 'allocate' method in alloc
	Name *symalloc = nametblFind("allocate", 8);
	StructAstNode *alloctype = (StructAstNode*)ptype->alloc;
    VarDclAstNode *allocmeth = (VarDclAstNode*)methnodesFind(&alloctype->methprops, symalloc);
	if (allocmeth == NULL || ((FnSigAstNode*)allocmeth->vtype)->parms->used != 1) {
		errorMsgNode((INode*)ptype, ErrorBadAlloc, "Allocator is missing valid allocate method");
		return;
	}

	// szvtype = sizeof(vtype)
	Name *szvtsym = nametblFind("szvtype", 7);
	SizeofAstNode *szvtval = newSizeofAstNode();
	szvtval->type = ptype->pvtype;
	VarDclAstNode *szvtype = newVarDclNode(szvtsym, VarDclTag, (INode*)usizeType, immPerm, (INode*)szvtval);
	nodesAdd(&blknode->stmts, (INode*)szvtype);
	NameUseAstNode *szvtuse = newNameUseNode(szvtsym);
	szvtuse->asttype = VarNameUseTag;
	szvtuse->dclnode = (NamedAstNode*)szvtype;
	szvtuse->vtype = szvtype->vtype;

	// p1 = alloc.allocate(szvtype)
	NameUseAstNode *usealloc = newNameUseNode(symalloc);
	usealloc->asttype = TypeNameUseTag;
	usealloc->dclnode = (NamedAstNode*)allocmeth;
	usealloc->vtype = allocmeth->vtype;
	FnCallAstNode *callalloc = newFnCallAstNode((INode*)usealloc, 1);
	callalloc->vtype = allocmeth->vtype;
	nodesAdd(&callalloc->args, (INode*)szvtuse);
	// ---
	Name *pT = nametblFind("pT", 2);
	CastAstNode *castvt = newCastAstNode((INode*)callalloc, (INode*)ptype);
	VarDclAstNode *p1dcl = newVarDclNode(pT, VarDclTag, (INode*)ptype, immPerm, (INode*)castvt);
	nodesAdd(&blknode->stmts, (INode*)p1dcl);
	NameUseAstNode *p1use = newNameUseNode(pT);
	p1use->asttype = VarNameUseTag;
	p1use->dclnode = (NamedAstNode*)p1dcl;
	p1use->vtype = p1dcl->vtype;

	// *p1 = value
	DerefAstNode *derefp1 = newDerefAstNode();
	derefp1->exp = (INode*)p1use;
	derefp1->vtype = ptype->pvtype;
	AssignAstNode *copynode = newAssignAstNode(AssignNode, (INode*)derefp1, anode->exp);
	nodesAdd(&blknode->stmts, (INode*)copynode);

	// return p1
	nodesAdd(&blknode->stmts, (INode*)p1use);

	anode->exp = (INode*)blknode;
}