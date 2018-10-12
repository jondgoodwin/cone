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
	case VarDclTag:
		varDclPrint((VarDclNode *)node); break;
    case FnDclTag:
        fnDclPrint((FnDclNode *)node); break;
    case BlockTag:
		blockPrint((BlockNode *)node); break;
	case IfTag:
		ifPrint((IfNode *)node); break;
	case WhileTag:
		whilePrint((WhileNode *)node); break;
	case BreakTag:
		inodeFprint("break"); break;
	case ContinueTag:
		inodeFprint("continue"); break;
	case ReturnTag:
		returnPrint((ReturnNode *)node); break;
	case AssignTag:
		assignPrint((AssignNode *)node); break;
    case VTupleTag:
        vtuplePrint((VTupleNode *)node); break;
	case FnCallTag:
		fnCallPrint((FnCallNode *)node); break;
	case SizeofTag:
		sizeofPrint((SizeofNode *)node); break;
	case CastTag:
		castPrint((CastNode *)node); break;
	case DerefTag:
		derefPrint((DerefNode *)node); break;
	case AddrTag:
		addrPrint((AddrNode *)node); break;
	case NotLogicTag: case OrLogicTag: case AndLogicTag:
		logicPrint((LogicNode *)node); break;
	case ULitTag:
		ulitPrint((ULitNode *)node); break;
	case FLitTag:
		flitPrint((FLitNode *)node); break;
    case ArrLitTag:
        arrLitPrint((ArrLitNode *)node); break;
    case StrLitTag:
		slitPrint((SLitNode *)node); break;
	case FnSigTag:
		fnSigPrint((FnSigNode *)node); break;
    case RefTag:
        refPrint((RefNode *)node); break;
    case PtrTag:
		ptrPrint((PtrNode *)node); break;
	case StructTag:
	case AllocTag:
		structPrint((StructNode *)node); break;
	case ArrayTag:
		arrayPrint((ArrayNode *)node); break;
	case IntNbrTag: case UintNbrTag: case FloatNbrTag:
		nbrTypePrint((NbrNode *)node); break;
	case PermTag:
		permPrint((PermNode *)node); break;
    case TTupleTag:
        ttuplePrint((TTupleNode *)node); break;
	case VoidTag:
		voidPrint((VoidTypeNode *)node); break;
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
void inodeWalk(PassState *pstate, INode **node) {
	switch ((*node)->tag) {
	case ModuleTag:
		modPass(pstate, (ModuleNode*)*node); break;
	case VarDclTag:
		varDclPass(pstate, (VarDclNode *)*node); break;
    case FnDclTag:
        fnDclPass(pstate, (FnDclNode *)*node); break;
    case NameUseTag:
    case VarNameUseTag:
    case TypeNameUseTag:
		nameUseWalk(pstate, (NameUseNode **)node); break;
    case ArrLitTag:
        arrLitWalk(pstate, (ArrLitNode *)*node); break;
	case BlockTag:
		blockPass(pstate, (BlockNode *)*node); break;
	case IfTag:
		ifPass(pstate, (IfNode *)*node); break;
	case WhileTag:
		whilePass(pstate, (WhileNode *)*node); break;
	case BreakTag:
	case ContinueTag:
		breakPass(pstate, *node); break;
	case ReturnTag:
		returnPass(pstate, (ReturnNode *)*node); break;
	case AssignTag:
		assignPass(pstate, (AssignNode *)*node); break;
    case VTupleTag:
        vtupleWalk(pstate, (VTupleNode *)*node); break;
	case FnCallTag:
		fnCallPass(pstate, (FnCallNode *)*node); break;
	case SizeofTag:
		sizeofPass(pstate, (SizeofNode *)*node); break;
	case CastTag:
		castPass(pstate, (CastNode *)*node); break;
	case DerefTag:
		derefPass(pstate, (DerefNode *)*node); break;
	case AddrTag:
		addrPass(pstate, (AddrNode *)*node); break;
	case NotLogicTag:
		logicNotPass(pstate, (LogicNode *)*node); break;
	case OrLogicTag: case AndLogicTag:
		logicPass(pstate, (LogicNode *)*node); break;
	case FnSigTag:
		fnSigPass(pstate, (FnSigNode *)*node); break;
    case RefTag:
        refPass(pstate, (RefNode *)*node); break;
    case PtrTag:
		ptrPass(pstate, (PtrNode *)*node); break;
	case StructTag:
		structPass(pstate, (StructNode *)*node); break;
	case ArrayTag:
		arrayPass(pstate, (ArrayNode *)*node); break;
    case TTupleTag:
        ttupleWalk(pstate, (TTupleNode *)*node); break;
    case AllocTag:
        break;

    case ULitTag:
    case FLitTag:
        inodeWalk(pstate, &((ITypedNode*)*node)->vtype);
        break;

    case MbrNameUseTag:
	case StrLitTag:
	case IntNbrTag: case UintNbrTag: case FloatNbrTag:
	case PermTag:
	case VoidTag:
		break;
	default:
		assert(0 && "**** ERROR **** Attempting to check an unknown node");
	}
}
