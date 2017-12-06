/** AST structure handlers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../parser/lexer.h"
#include "../shared/fileio.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

FILE *astfile;

// Output an indented, serialized line to astfile
void astPrintLn(int indent, char *str, ...) {
	int cnt;
	va_list argptr;
	for (cnt=0; cnt<indent; cnt++)
		fprintf(astfile, (cnt&3)==0? "| " : "  ");
	va_start(argptr, str);
	vfprintf(astfile, str, argptr);
	va_end(argptr);
	fprintf(astfile, "\n");
}

// Serialize a specific AST node
void astPrintNode(int indent, AstNode *node, char *prefix) {
	switch (node->asttype) {
	case PgmNode:
		pgmPrint(indent, (PgmAstNode *)node); break;
	case VarNode:
		varPrint(indent, (VarAstNode *)node); break;
	case BlockNode:
		blockPrint(indent, (BlockAstNode *)node); break;
	case StmtExpNode:
		stmtExpPrint(indent, (StmtExpAstNode *)node); break;
	case ReturnNode:
		returnPrint(indent, (StmtExpAstNode *)node); break;
	case ULitNode:
		ulitPrint(indent, (ULitAstNode *)node); break;
	case FLitNode:
		flitPrint(indent, (FLitAstNode *)node); break;
	case FnSig:
		fnsigPrint(indent, (FnSigAstNode *)node, prefix); break;
	case IntType: case UintType: case FloatType:
		nbrTypePrint(indent, (NbrTypeAstNode *)node, prefix); break;
	case PermType:
		permTypePrint(indent, (PermTypeAstNode *)node, prefix); break;
	case VoidType:
		voidPrint(indent, (VoidTypeAstNode *)node, prefix); break;
	default:
		astPrintLn(indent, "%s **** UNKNOWN NODE ****", prefix);
	}
}

// Serialize the program's AST to dir+srcfn
void astPrint(char *dir, char *srcfn, AstNode *pgmast) {
	astfile = fopen(fileMakePath(dir, pgmast->lexer->fname, "ast"), "wb");
	astPrintNode(0, pgmast, "");
	fclose(astfile);
}

// Dispatch a pass to a node
// Syntactic sugar, name resolution, type inference and type checking
void astPass(AstPass *pstate, AstNode *node) {
	switch (node->asttype) {
	case PgmNode:
		pgmPass(pstate, (PgmAstNode*)node); break;
	case VarNode:
		varPass(pstate, (VarAstNode *)node); break;
	case BlockNode:
		blockPass(pstate, (BlockAstNode *)node); break;
	case StmtExpNode:
		stmtExpPass(pstate, (StmtExpAstNode *)node); break;
	case ReturnNode:
		returnPass(pstate, (StmtExpAstNode *)node); break;
	case ULitNode:
	case FLitNode:
	case FnSig:
	case IntType: case UintType: case FloatType:
	case PermType:
	case VoidType:
		break;
	default:
		puts("**** ERROR **** Attempting to check an unknown node");
	}
}

// Run all passes against the AST (after parse and before gen)
void astPasses(PgmAstNode *pgm) {
	AstPass pstate;
	pstate.pass = GlobalPass;
	astPass(&pstate, (AstNode*) pgm);
}