/** Region type handling - region is a specialized struct
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

int isRegion(INode *region, Name *namesym) {
    region = itypeGetTypeDcl(region);
    if (region->tag == StructTag) {
        StructNode *rnode = (StructNode*)region;
        return rnode->namesym == namesym;
    }
    return 0;
}

int regionIsPtrU8(RefNode *ptrnode) {
    if (ptrnode->tag != PtrTag)
        return 0;
    NbrNode *nbrnode = (NbrNode*)itypeGetTypeDcl(ptrnode->vtexp);
    if (nbrnode != u8Type)
        return 0;
    return 1;
}

// Verify that region allocator exists and it (and init method) are correctly declared
void regionAllocTypeCheck(INode *region) {
    if (region->tag != StructTag) {
        errorMsgNode(region, ErrorInvType, "Not a valid region.");
        return;
    }
    FnDclNode *allocmeth = (FnDclNode*)iTypeFindFnField(region, allocMethodName);
    if (allocmeth == NULL || allocmeth->tag != FnDclTag) {
        errorMsgNode(region, ErrorInvType, "Region does not support allocation as it lacks _alloc static method.");
        return;
    }
    FnSigNode *allocsig = (FnSigNode*)itypeGetTypeDcl(allocmeth->vtype);
    if (allocsig->parms->used != 1) {
        errorMsgNode((INode*)allocmeth, ErrorInvType, "Region _alloc method needs single usize parm.");
        return;
    }
    NbrNode *sizetype = (NbrNode *)itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(allocsig->parms, 0)));
    if (sizetype != usizeType) {
        errorMsgNode((INode*)allocmeth, ErrorInvType, "Region _alloc method needs single usize parm.");
        return;
    }
    RefNode *rettype = (RefNode*)itypeGetTypeDcl(allocsig->rettype);
    if (!regionIsPtrU8(rettype)) {
        errorMsgNode((INode*)allocmeth, ErrorInvType, "Region _alloc method must return *u8.");
        return;
    }

    FnDclNode *initmeth = (FnDclNode*)iTypeFindFnField(region, initMethodName);
    if (initmeth == NULL) {
        return;
    }
    FnSigNode *initsig = (FnSigNode*)itypeGetTypeDcl(initmeth->vtype);
    if (initsig->parms->used != 0) {
        errorMsgNode((INode*)initmeth, ErrorInvType, "Region init method may not have parameters.");
        return;
    }
    INode *initrettype = itypeGetTypeDcl(initsig->rettype);
    if (itypeMatches(initrettype, region, Coercion) != EqMatch) {
        errorMsgNode((INode*)initmeth, ErrorInvType, "Region init method must return initial value.");
        return;
    }

}
