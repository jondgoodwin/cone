/** Namespace structures and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef module_h
#define module_h

ModuleAstNode *newModuleNode();
void modPrint(ModuleAstNode *mod);
void modPass(PassState *pstate, ModuleAstNode *mod);

#endif