/** AST structure handlers
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
FILE *astfile;
int astIndent=0;
int astisNL = 1;

// Output a string to astfile
void inodeFprint(char *str, ...) {
	va_list argptr;
	va_start(argptr, str);
	vfprintf(astfile, str, argptr);
	va_end(argptr);
	astisNL = 0;
}

// Print new line character
void inodePrintNL() {
	if (!astisNL)
		fputc('\n', astfile);
	astisNL = 1;
}

// Output a line's beginning indentation
void inodePrintIndent() {
	int cnt;
	for (cnt = 0; cnt<astIndent; cnt++)
		fprintf(astfile, (cnt & 3) == 0 ? "| " : "  ");
	astisNL = 0;
}

// Increment indentation
void inodePrintIncr() {
	astIndent++;
}

// Decrement indentation
void inodePrintDecr() {
	astIndent--;
}

// Serialize a specific AST node
void inodePrintNode(INode *node) {
	switch (node->asttype) {
	case ModuleTag:
		modPrint((ModuleAstNode *)node); break;
	case NameUseTag:
    case VarNameUseTag:
    case TypeNameUseTag:
	case MbrNameUseTag:
		nameUsePrint((NameUseAstNode *)node); break;
	case VarDclTag:
		varDclPrint((VarDclAstNode *)node); break;
    case FnDclTag:
        fnDclPrint((FnDclAstNode *)node); break;
    case BlockTag:
		blockPrint((BlockAstNode *)node); break;
	case IfTag:
		ifPrint((IfAstNode *)node); break;
	case WhileTag:
		whilePrint((WhileAstNode *)node); break;
	case BreakTag:
		inodeFprint("break"); break;
	case ContinueTag:
		inodeFprint("continue"); break;
	case ReturnTag:
		returnPrint((ReturnAstNode *)node); break;
	case AssignTag:
		assignPrint((AssignAstNode *)node); break;
	case FnCallTag:
		fnCallPrint((FnCallAstNode *)node); break;
	case SizeofTag:
		sizeofPrint((SizeofAstNode *)node); break;
	case CastTag:
		castPrint((CastAstNode *)node); break;
	case DerefTag:
		derefPrint((DerefAstNode *)node); break;
	case AddrTag:
		addrPrint((AddrAstNode *)node); break;
	case NotLogicTag: case OrLogicTag: case AndLogicTag:
		logicPrint((LogicAstNode *)node); break;
	case ULitTag:
		ulitPrint((ULitAstNode *)node); break;
	case FLitTag:
		flitPrint((FLitAstNode *)node); break;
	case StrLitTag:
		slitPrint((SLitAstNode *)node); break;
	case FnSigTag:
		fnSigPrint((FnSigAstNode *)node); break;
	case RefTag: case PtrTag:
		ptrTypePrint((PtrAstNode *)node); break;
	case StructTag:
	case AllocTag:
		structPrint((StructAstNode *)node); break;
	case ArrayTag:
		arrayPrint((ArrayAstNode *)node); break;
	case IntNbrTag: case UintNbrTag: case FloatNbrTag:
		nbrTypePrint((NbrAstNode *)node); break;
	case PermTag:
		permPrint((PermAstNode *)node); break;
	case VoidTag:
		voidPrint((VoidTypeAstNode *)node); break;
	default:
		inodeFprint("**** UNKNOWN NODE ****");
	}
}

// Serialize the program's AST to dir+srcfn
void inodePrint(char *dir, char *srcfn, INode *pgmast) {
	astfile = fopen(fileMakePath(dir, pgmast->lexer->fname, "ast"), "wb");
	inodePrintNode(pgmast);
	fclose(astfile);
}

// Dispatch a node walk for the current semantic analysis pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeWalk(PassState *pstate, INode **node) {
	switch ((*node)->asttype) {
	case ModuleTag:
		modPass(pstate, (ModuleAstNode*)*node); break;
	case VarDclTag:
		varDclPass(pstate, (VarDclAstNode *)*node); break;
    case FnDclTag:
        fnDclPass(pstate, (FnDclAstNode *)*node); break;
    case NameUseTag:
    case VarNameUseTag:
    case TypeNameUseTag:
		nameUseWalk(pstate, (NameUseAstNode **)node); break;
	case BlockTag:
		blockPass(pstate, (BlockAstNode *)*node); break;
	case IfTag:
		ifPass(pstate, (IfAstNode *)*node); break;
	case WhileTag:
		whilePass(pstate, (WhileAstNode *)*node); break;
	case BreakTag:
	case ContinueTag:
		breakPass(pstate, *node); break;
	case ReturnTag:
		returnPass(pstate, (ReturnAstNode *)*node); break;
	case AssignTag:
		assignPass(pstate, (AssignAstNode *)*node); break;
	case FnCallTag:
		fnCallPass(pstate, (FnCallAstNode *)*node); break;
	case SizeofTag:
		sizeofPass(pstate, (SizeofAstNode *)*node); break;
	case CastTag:
		castPass(pstate, (CastAstNode *)*node); break;
	case DerefTag:
		derefPass(pstate, (DerefAstNode *)*node); break;
	case AddrTag:
		addrPass(pstate, (AddrAstNode *)*node); break;
	case NotLogicTag:
		logicNotPass(pstate, (LogicAstNode *)*node); break;
	case OrLogicTag: case AndLogicTag:
		logicPass(pstate, (LogicAstNode *)*node); break;
	case FnSigTag:
		fnSigPass(pstate, (FnSigAstNode *)*node); break;
	case RefTag: case PtrTag:
		ptrTypePass(pstate, (PtrAstNode *)*node); break;
	case StructTag:
	case AllocTag:
		structPass(pstate, (StructAstNode *)*node); break;
	case ArrayTag:
		arrayPass(pstate, (ArrayAstNode *)*node); break;

	case MbrNameUseTag:
	case ULitTag:
	case FLitTag:
	case StrLitTag:
	case IntNbrTag: case UintNbrTag: case FloatNbrTag:
	case PermTag:
	case VoidTag:
		break;
	default:
		assert(0 && "**** ERROR **** Attempting to check an unknown node");
	}
}
