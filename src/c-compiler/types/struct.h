/** AST handling for struct types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef struct_h
#define struct_h

// For pointers
typedef struct StructAstNode {
	TypeAstHdr;
	Inodes *fields;
} StructAstNode;

StructAstNode *newStructNode();
void structPrint(StructAstNode *node);
void structPass(AstPass *pstate, StructAstNode *name);
int structEqual(StructAstNode *node1, StructAstNode *node2);
int structCoerces(StructAstNode *to, StructAstNode *from);

#endif