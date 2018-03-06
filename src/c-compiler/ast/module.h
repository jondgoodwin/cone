/** Module and import structures and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef module_h
#define module_h

// Module
typedef struct ModuleAstNode {
	NamedAstHdr;
	Nodes *nodes;
} ModuleAstNode;

ModuleAstNode *newModuleNode();
void modPrint(ModuleAstNode *mod);
void modHook(ModuleAstNode *oldmod, ModuleAstNode *newmod);
void modPass(PassState *pstate, ModuleAstNode *mod);

#endif