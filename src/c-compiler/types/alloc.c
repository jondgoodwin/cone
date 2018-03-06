/** Allocate Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../parser/lexer.h"
#include "../shared/nametbl.h"

#include <assert.h>

// Type check and expand code for allocator reference creator
// This actually generates additional code that replaces anode->exp
void allocAllocate(AddrAstNode *anode, PtrAstNode *ptype) {
	BlockAstNode *blknode = newBlockNode();

	// Find 'allocate' method in alloc
	Name *symalloc = nameFind("allocate", 8);
	TypeAstNode *alloctype = (TypeAstNode*)ptype->alloc;
	int32_t cnt;
	AstNode **nodesp;
	NameDclAstNode *allocmeth = NULL;
	for (nodesFor(alloctype->methods, cnt, nodesp)) {
		NameDclAstNode *meth = (NameDclAstNode*)*nodesp;
		if (meth->namesym == symalloc) {
			allocmeth = meth;
			break;
		}
	}
	if (allocmeth == NULL || ((FnSigAstNode*)allocmeth->vtype)->parms->used != 1) {
		errorMsgNode((AstNode*)ptype, ErrorBadAlloc, "Allocator is missing valid allocate method");
		return;
	}

	// szvtype = sizeof(vtype)
	Name *szvtsym = nameFind("szvtype", 7);
	SizeofAstNode *szvtval = newSizeofAstNode();
	szvtval->type = ptype->pvtype;
	NameDclAstNode *szvtype = newNameDclNode(szvtsym, VarNameDclNode, (AstNode*)usizeType, immPerm, (AstNode*)szvtval);
	nodesAdd(&blknode->stmts, (AstNode*)szvtype);
	NameUseAstNode *szvtuse = newNameUseNode(szvtsym);
	szvtuse->asttype = NameUseNode;
	szvtuse->dclnode = szvtype;
	szvtuse->vtype = szvtype->vtype;

	// p1 = alloc.allocate(szvtype)
	NameUseAstNode *usealloc = newNameUseNode(symalloc);
	usealloc->asttype = NameUseNode;
	usealloc->dclnode = allocmeth;
	usealloc->vtype = allocmeth->vtype;
	FnCallAstNode *callalloc = newFnCallAstNode((AstNode*)usealloc, 1);
	callalloc->vtype = allocmeth->vtype;
	nodesAdd(&callalloc->parms, (AstNode*)szvtuse);
	// ---
	Name *pT = nameFind("pT", 2);
	CastAstNode *castvt = newCastAstNode((AstNode*)callalloc, (AstNode*)ptype);
	NameDclAstNode *p1dcl = newNameDclNode(pT, VarNameDclNode, (AstNode*)ptype, immPerm, (AstNode*)castvt);
	nodesAdd(&blknode->stmts, (AstNode*)p1dcl);
	NameUseAstNode *p1use = newNameUseNode(pT);
	p1use->asttype = NameUseNode;
	p1use->dclnode = p1dcl;
	p1use->vtype = p1dcl->vtype;

	// *p1 = value
	DerefAstNode *derefp1 = newDerefAstNode();
	derefp1->exp = (AstNode*)p1use;
	derefp1->vtype = ptype->pvtype;
	AssignAstNode *copynode = newAssignAstNode(AssignNode, (AstNode*)derefp1, anode->exp);
	nodesAdd(&blknode->stmts, (AstNode*)copynode);

	// return p1
	nodesAdd(&blknode->stmts, (AstNode*)p1use);

	anode->exp = (AstNode*)blknode;
}