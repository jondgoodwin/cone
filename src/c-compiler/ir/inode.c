/** Generic node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "../parser/lexer.h"
#include "../shared/fileio.h"
#include "../shared/error.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

// State for inodePrint
FILE *irfile;
int irIndent=0;
int irIsNL = 1;

// Output a string to irfile
void inodeFprint(char *str, ...) {
    va_list argptr;
    va_start(argptr, str);
    vfprintf(irfile, str, argptr);
    va_end(argptr);
    irIsNL = 0;
}

// Print new line character
void inodePrintNL() {
    if (!irIsNL)
        fputc('\n', irfile);
    irIsNL = 1;
}

// Output a line's beginning indentation
void inodePrintIndent() {
    int cnt;
    for (cnt = 0; cnt<irIndent; cnt++)
        fprintf(irfile, (cnt & 3) == 0 ? "| " : "  ");
    irIsNL = 0;
}

// Increment indentation
void inodePrintIncr() {
    irIndent++;
}

// Decrement indentation
void inodePrintDecr() {
    irIndent--;
}

// Serialize a specific node
void inodePrintNode(INode *node) {
    switch (node->tag) {
    case ModuleTag:
        modPrint((ModuleNode *)node); break;
    case NameUseTag:
    case VarNameUseTag:
    case TypeNameUseTag:
    case MbrNameUseTag:
        nameUsePrint((NameUseNode *)node); break;
    case FnDclTag:
        fnDclPrint((FnDclNode *)node); break;
    case VarDclTag:
        varDclPrint((VarDclNode *)node); break;
    case FieldDclTag:
        fieldDclPrint((FieldDclNode *)node); break;
    case BlockTag:
        blockPrint((BlockNode *)node); break;
    case IfTag:
        ifPrint((IfNode *)node); break;
    case LoopTag:
        loopPrint((LoopNode *)node); break;
    case BreakTag:
        inodeFprint("break"); break;
    case ContinueTag:
        inodeFprint("continue"); break;
    case BlockRetTag:
    case ReturnTag:
        returnPrint((ReturnNode *)node); break;
    case AssignTag:
        assignPrint((AssignNode *)node); break;
    case VTupleTag:
        vtuplePrint((VTupleNode *)node); break;
    case FnCallTag:
    case StrFieldTag:
    case ArrIndexTag:
        fnCallPrint((FnCallNode *)node); break;
    case SizeofTag:
        sizeofPrint((SizeofNode *)node); break;
    case CastTag:
        castPrint((CastNode *)node); break;
    case DerefTag:
        derefPrint((DerefNode *)node); break;
    case BorrowTag:
        borrowPrint((BorrowNode *)node); break;
    case AllocateTag:
        allocatePrint((AllocateNode *)node); break;
    case NotLogicTag: case OrLogicTag: case AndLogicTag:
        logicPrint((LogicNode *)node); break;
    case ULitTag:
        ulitPrint((ULitNode *)node); break;
    case FLitTag:
        flitPrint((FLitNode *)node); break;
    case TypeLitTag:
        typeLitPrint((FnCallNode *)node); break;
    case StrLitTag:
        slitPrint((SLitNode *)node); break;
    case FnSigTag:
        fnSigPrint((FnSigNode *)node); break;
    case RefTag:
        refPrint((RefNode *)node); break;
    case ArrayRefTag:
        arrayRefPrint((RefNode *)node); break;
    case PtrTag:
        ptrPrint((PtrNode *)node); break;
    case StructTag:
    case AllocTag:
        structPrint((StructNode *)node); break;
    case EnumTag:
        enumPrint((EnumNode *)node); break;
    case ArrayTag:
        arrayPrint((ArrayNode *)node); break;
    case IntNbrTag: case UintNbrTag: case FloatNbrTag:
        nbrTypePrint((NbrNode *)node); break;
    case PermTag:
        permPrint((PermNode *)node); break;
    case LifetimeTag:
        lifePrint((LifetimeNode *)node); break;
    case TTupleTag:
        ttuplePrint((TTupleNode *)node); break;
    case VoidTag:
        voidPrint((VoidTypeNode *)node); break;
    case NamedValTag:
        namedValPrint((NamedValNode *)node); break;
    case AliasTag:
    {
        AliasNode *anode = (AliasNode *)node;
        inodeFprint("(alias ");
        if (anode->counts == NULL)
            inodeFprint("%d ", (int)anode->aliasamt);
        else {
            int16_t count = anode->aliasamt;
            int16_t *countp = anode->counts;
            while (count--)
                inodeFprint("%d ", (int)*countp++);
        }
        inodePrintNode(anode->exp);
        inodeFprint(")");
        break;
    }
    case NullTag:
        inodeFprint("null");
        break;
    default:
        inodeFprint("**** UNKNOWN NODE ****");
    }
}

// Serialize the program's IR to dir+srcfn
void inodePrint(char *dir, char *srcfn, INode *pgmnode) {
    irfile = fopen(fileMakePath(dir, pgmnode->lexer->fname, "ast"), "wb");
    inodePrintNode(pgmnode);
    fclose(irfile);
}

// Dispatch a node walk for the current semantic analysis pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeNameRes(NameResState *pstate, INode **node) {
    switch ((*node)->tag) {
    case ModuleTag:
        modNameRes(pstate, (ModuleNode*)*node); break;
    case FnDclTag:
        fnDclNameRes(pstate, (FnDclNode *)*node); break;
    case VarDclTag:
        varDclNameRes(pstate, (VarDclNode *)*node); break;
    case FieldDclTag:
        fieldDclNameRes(pstate, (FieldDclNode *)*node); break;
    case NameUseTag:
    case VarNameUseTag:
    case TypeNameUseTag:
        nameUseNameRes(pstate, (NameUseNode **)node); break;
    case TypeLitTag:
        typeLitNameRes(pstate, (FnCallNode *)*node); break;
    case BlockTag:
        blockNameRes(pstate, (BlockNode *)*node); break;
    case IfTag:
        ifNameRes(pstate, (IfNode *)*node); break;
    case LoopTag:
        loopNameRes(pstate, (LoopNode *)*node); break;
    case BreakTag:
        breakNameRes(pstate, (BreakNode *)*node); break;
    case ContinueTag:
        continueNameRes(pstate, (ContinueNode *)*node); break;
    case ReturnTag:
        returnNameRes(pstate, (ReturnNode *)*node); break;
    case AssignTag:
        assignNameRes(pstate, (AssignNode *)*node); break;
    case VTupleTag:
        vtupleNameRes(pstate, (VTupleNode *)*node); break;
    case FnCallTag:
        fnCallNameRes(pstate, (FnCallNode **)node); break;
    case SizeofTag:
        sizeofNameRes(pstate, (SizeofNode *)*node); break;
    case CastTag:
        castNameRes(pstate, (CastNode *)*node); break;
    case DerefTag:
        derefNameRes(pstate, (DerefNode *)*node); break;
    case BorrowTag:
        borrowNameRes(pstate, (BorrowNode **)node); break;
    case AllocateTag:
        allocateNameRes(pstate, (AllocateNode **)node); break;
    case NotLogicTag:
        logicNotNameRes(pstate, (LogicNode *)*node); break;
    case OrLogicTag: case AndLogicTag:
        logicNameRes(pstate, (LogicNode *)*node); break;
    case NamedValTag:
        namedValNameRes(pstate, (NamedValNode *)*node); break;

    case FnSigTag:
        fnSigNameRes(pstate, (FnSigNode *)*node); break;
    case RefTag:
        refNameRes(pstate, (RefNode *)*node); break;
    case ArrayRefTag:
        arrayRefNameRes(pstate, (RefNode *)*node); break;
    case PtrTag:
        ptrNameRes(pstate, (PtrNode *)*node); break;
    case StructTag:
        structNameRes(pstate, (StructNode *)*node); break;
    case EnumTag:
        enumNameRes(pstate, (EnumNode *)*node); break;
    case ArrayTag:
        arrayNameRes(pstate, (ArrayNode *)*node); break;
    case TTupleTag:
        ttupleNameRes(pstate, (TTupleNode *)*node); break;
    case AllocTag:
        break;

    case MbrNameUseTag:
    case ULitTag:
    case FLitTag:
    case StrLitTag:
    case IntNbrTag: case UintNbrTag: case FloatNbrTag:
    case PermTag:
    case VoidTag:
    case NullTag:
        break;
    default:
        assert(0 && "**** ERROR **** Attempting to check an unknown node");
    }
}

// Dispatch a node walk for the type check pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeTypeCheck(TypeCheckState *pstate, INode **node) {

    // Type nodes are fully checked the first time they are referenced,
    // so that we know everything we need to know about correctly managing their values
    // (because of needing to infer infectious type constraints based on their composed fields).
    // However, type checking of a type node should only be done once
    if (isTypeNode(*node)) {
        if ((*node)->flags & TypeChecked)
            return;
        if ((*node)->flags & TypeChecking) {
            errorMsgNode(*node, ErrorRecurse, "Recursive types are not supported for now.");
            return;
        }
        else
            (*node)->flags |= TypeChecking;
    }

    switch ((*node)->tag) {
    case ModuleTag:
        modTypeCheck(pstate, (ModuleNode*)*node); break;
    case FnDclTag:
        fnDclTypeCheck(pstate, (FnDclNode *)*node); break;
    case VarDclTag:
        varDclTypeCheck(pstate, (VarDclNode *)*node); break;
    case FieldDclTag:
        fieldDclTypeCheck(pstate, (FieldDclNode *)*node); break;
    case VarNameUseTag:
        nameUseTypeCheck(pstate, (NameUseNode **)node); break;
    case TypeLitTag:
        typeLitTypeCheck(pstate, (FnCallNode *)*node); break;
    case BlockTag:
        blockTypeCheck(pstate, (BlockNode *)*node); break;
    case IfTag:
        ifTypeCheck(pstate, (IfNode *)*node); break;
    case LoopTag:
        loopTypeCheck(pstate, (LoopNode *)*node); break;
    case BreakTag:
        breakTypeCheck(pstate, (BreakNode *)*node); break;
    case ContinueTag:
        continueTypeCheck(pstate, (ContinueNode *)*node); break;
    case ReturnTag:
        returnTypeCheck(pstate, (ReturnNode *)*node); break;
    case AssignTag:
        assignTypeCheck(pstate, (AssignNode *)*node); break;
    case VTupleTag:
        vtupleTypeCheck(pstate, (VTupleNode *)*node); break;
    case FnCallTag:
        fnCallTypeCheck(pstate, (FnCallNode **)node); break;
    case SizeofTag:
        sizeofTypeCheck(pstate, (SizeofNode *)*node); break;
    case CastTag:
        castTypeCheck(pstate, (CastNode *)*node); break;
    case DerefTag:
        derefTypeCheck(pstate, (DerefNode *)*node); break;
    case BorrowTag:
        borrowTypeCheck(pstate, (BorrowNode **)node); break;
    case AllocateTag:
        allocateTypeCheck(pstate, (AllocateNode **)node); break;
    case NotLogicTag:
        logicNotTypeCheck(pstate, (LogicNode *)*node); break;
    case OrLogicTag: case AndLogicTag:
        logicTypeCheck(pstate, (LogicNode *)*node); break;
    case NamedValTag:
        namedValTypeCheck(pstate, (NamedValNode *)*node); break;
    case ULitTag:
    case FLitTag:
        litTypeCheck(pstate, (IExpNode*)*node); break;

    case TypeNameUseTag:
        nameUseTypeCheckType(pstate, (NameUseNode **)node); break;
    case FnSigTag:
        fnSigTypeCheck(pstate, (FnSigNode *)*node); break;
    case RefTag:
        refTypeCheck(pstate, (RefNode *)*node); break;
    case ArrayRefTag:
        arrayRefTypeCheck(pstate, (RefNode *)*node); break;
    case PtrTag:
        ptrTypeCheck(pstate, (PtrNode *)*node); break;
    case StructTag:
        structTypeCheck(pstate, (StructNode *)*node); break;
    case EnumTag:
        enumTypeCheck(pstate, (EnumNode *)*node); break;
    case ArrayTag:
        arrayTypeCheck(pstate, (ArrayNode *)*node); break;
    case TTupleTag:
        ttupleTypeCheck(pstate, (TTupleNode *)*node); break;
    case AllocTag:
    case PermTag:
        break;

    case MbrNameUseTag:
    case StrLitTag:
    case IntNbrTag: case UintNbrTag: case FloatNbrTag:
    case VoidTag:
    case NullTag:
        break;
    default:
        assert(0 && "**** ERROR **** Attempting to check an unknown node");
    }

    // Confirm when a type node has been checked
    if (isTypeNode(*node)) {
        (*node)->flags |= TypeChecked;
    }
}
