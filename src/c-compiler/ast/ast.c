/** AST structure handlers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../parser/lexer.h"
#include "../shared/fileio.h"
#include "../shared/error.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

// State for astPrint
FILE *astfile;
int astIndent=0;
int astisNL = 1;

// Output a string to astfile
void astFprint(char *str, ...) {
	va_list argptr;
	va_start(argptr, str);
	vfprintf(astfile, str, argptr);
	va_end(argptr);
	astisNL = 0;
}

// Print new line character
void astPrintNL() {
	if (!astisNL)
		fputc('\n', astfile);
	astisNL = 1;
}

// Output a line's beginning indentation
void astPrintIndent() {
	int cnt;
	for (cnt = 0; cnt<astIndent; cnt++)
		fprintf(astfile, (cnt & 3) == 0 ? "| " : "  ");
	astisNL = 0;
}

// Increment indentation
void astPrintIncr() {
	astIndent++;
}

// Decrement indentation
void astPrintDecr() {
	astIndent--;
}

// Serialize a specific AST node
void astPrintNode(AstNode *node) {
	switch (node->asttype) {
	case ModuleNode:
		modPrint((ModuleAstNode *)node); break;
	case NameUseNode:
	case MemberUseNode:
		nameUsePrint((NameUseAstNode *)node); break;
	case VarNameDclNode: case VtypeNameDclNode: case PermNameDclNode: case AllocNameDclNode:
		nameDclPrint((NameDclAstNode *)node); break;
	case BlockNode:
		blockPrint((BlockAstNode *)node); break;
	case IfNode:
		ifPrint((IfAstNode *)node); break;
	case WhileNode:
		whilePrint((WhileAstNode *)node); break;
	case BreakNode:
		astFprint("break"); break;
	case ContinueNode:
		astFprint("continue"); break;
	case ReturnNode:
		returnPrint((ReturnAstNode *)node); break;
	case AssignNode:
		assignPrint((AssignAstNode *)node); break;
	case FnCallNode:
		fnCallPrint((FnCallAstNode *)node); break;
	case SizeofNode:
		sizeofPrint((SizeofAstNode *)node); break;
	case CastNode:
		castPrint((CastAstNode *)node); break;
	case DerefNode:
		derefPrint((DerefAstNode *)node); break;
	case ElementNode:
		elementPrint((ElementAstNode *)node); break;
	case AddrNode:
		addrPrint((AddrAstNode *)node); break;
	case NotLogicNode: case OrLogicNode: case AndLogicNode:
		logicPrint((LogicAstNode *)node); break;
	case ULitNode:
		ulitPrint((ULitAstNode *)node); break;
	case FLitNode:
		flitPrint((FLitAstNode *)node); break;
	case SLitNode:
		slitPrint((SLitAstNode *)node); break;
	case FnSig:
		fnSigPrint((FnSigAstNode *)node); break;
	case RefType: case PtrType:
		ptrTypePrint((PtrAstNode *)node); break;
	case StructType:
	case AllocType:
		structPrint((StructAstNode *)node); break;
	case ArrayType:
		arrayPrint((ArrayAstNode *)node); break;
	case IntNbrType: case UintNbrType: case FloatNbrType:
		nbrTypePrint((NbrAstNode *)node); break;
	case PermType:
		permPrint((PermAstNode *)node); break;
	case VoidType:
		voidPrint((VoidTypeAstNode *)node); break;
	default:
		astFprint("**** UNKNOWN NODE ****");
	}
}

// Serialize the program's AST to dir+srcfn
void astPrint(char *dir, char *srcfn, AstNode *pgmast) {
	astfile = fopen(fileMakePath(dir, pgmast->lexer->fname, "ast"), "wb");
	astPrintNode(pgmast);
	fclose(astfile);
}

// Dispatch a pass to a node
// Syntactic sugar, name resolution, type inference and type checking
void astPass(PassState *pstate, AstNode *node) {
	switch (node->asttype) {
	case ModuleNode:
		modPass(pstate, (ModuleAstNode*)node); break;
	case VarNameDclNode:
		nameDclPass(pstate, (NameDclAstNode *)node); break;
	case VtypeNameDclNode:
	case AllocNameDclNode:
		nameVtypeDclPass(pstate, (NameDclAstNode *)node); break;
	case NameUseNode:
		nameUsePass(pstate, (NameUseAstNode *)node); break;
	case BlockNode:
		blockPass(pstate, (BlockAstNode *)node); break;
	case IfNode:
		ifPass(pstate, (IfAstNode *)node); break;
	case WhileNode:
		whilePass(pstate, (WhileAstNode *)node); break;
	case BreakNode:
	case ContinueNode:
		breakPass(pstate, node); break;
	case ReturnNode:
		returnPass(pstate, (ReturnAstNode *)node); break;
	case AssignNode:
		assignPass(pstate, (AssignAstNode *)node); break;
	case FnCallNode:
		fnCallPass(pstate, (FnCallAstNode *)node); break;
	case SizeofNode:
		sizeofPass(pstate, (SizeofAstNode *)node); break;
	case CastNode:
		castPass(pstate, (CastAstNode *)node); break;
	case DerefNode:
		derefPass(pstate, (DerefAstNode *)node); break;
	case ElementNode:
		elementPass(pstate, (ElementAstNode *)node); break;
	case AddrNode:
		addrPass(pstate, (AddrAstNode *)node); break;
	case NotLogicNode:
		logicNotPass(pstate, (LogicAstNode *)node); break;
	case OrLogicNode: case AndLogicNode:
		logicPass(pstate, (LogicAstNode *)node); break;
	case FnSig:
		fnSigPass(pstate, (FnSigAstNode *)node); break;
	case RefType: case PtrType:
		ptrTypePass(pstate, (PtrAstNode *)node); break;
	case StructType:
	case AllocType:
		structPass(pstate, (StructAstNode *)node); break;
	case ArrayType:
		arrayPass(pstate, (ArrayAstNode *)node); break;

	case MemberUseNode:
	case ULitNode:
	case FLitNode:
	case SLitNode:
	case IntNbrType: case UintNbrType: case FloatNbrType:
	case PermType:
	case VoidType:
		break;
	default:
		assert(0 && "**** ERROR **** Attempting to check an unknown node");
	}
}

// Run all passes against the AST (after parse and before gen)
void astPasses(ModuleAstNode *mod) {
	PassState pstate;
	pstate.mod = mod;
	pstate.fnsig = NULL;
	pstate.blk = NULL;
	pstate.scope = 0;
	pstate.flags = 0;

	// Resolve all name uses to their appropriate declaration
	pstate.pass = NameResolution;
	astPass(&pstate, (AstNode*) mod);
	if (errors)
		return;

	// Apply syntactic sugar, and perform type inference/check
	pstate.pass = TypeCheck;
	astPass(&pstate, (AstNode*)mod);
}