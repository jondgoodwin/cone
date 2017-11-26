/** AST structure handlers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../parser/lexer.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// Allocate and initialize a new nodes block
Nodes *nodesNew(int size) {
	Nodes *nodes;
	nodes = (Nodes*) memAllocBlk(sizeof(Nodes) + size*sizeof(AstNode*));
	nodes->avail = size;
	nodes->used = 0;
	return nodes;
}

// Add an AstNode to the end of a Nodes, growing it if full (changing its memory location)
// This assumes a nodes can only have a single parent, whose address we point at
void nodesAdd(Nodes **nodesp, AstNode *node) {
	Nodes *nodes = *nodesp;
	// If full, double its size
	if (nodes->used >= nodes->avail) {
		Nodes *oldnodes;
		AstNode **op, **np;
		oldnodes = nodes;
		nodes = nodesNew(oldnodes->avail << 1);
		op = (AstNode **)(oldnodes+1);
		np = (AstNode **)(nodes+1);
		memcpy(np, op, (nodes->used = oldnodes->used) * sizeof(AstNode*));
		*nodesp = nodes;
	}
	*((AstNode**)(nodes+1)+nodes->used) = node;
	nodes->used++;
}

FILE *astfile;

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

void astPrintNode(int indent, AstNode *node) {
	switch (node->asttype) {
	case GlobalNode:
		astPrintLn(indent, "AST for program %s", node->lexer->url); break;
	default:
		astPrintLn(indent, "**** UNKNOWN NODE ****");
	}
}

void astPrint(AstNode *pgm) {
	astfile = fopen("program.ast", "wb");
	astPrintNode(0, pgm);
	fclose(astfile);
}