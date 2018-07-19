/** AST handling for method tuples (multiple methods with the same name)
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fntuple_h
#define fntuple_h

// Node that lists (in order) all methods having the same name
typedef struct FnTupleAstNode {
	NamedAstHdr;
	Nodes *methods;
} FnTupleAstNode;

FnTupleAstNode *newFnTupleNode(Name *name);
void fnTuplePrint(FnTupleAstNode *node);
void fnTuplePass(PassState *pstate, FnTupleAstNode *name);

#endif