/** Module and import structures and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef module_h
#define module_h

// Module is a global namespace owning (or folding in) many kinds of named nodes:
// - Functions
// - Global variables
// - Type declarations (incl. permissions and allocators)
// - Macro and template definitions
// - Modules nested within this parent module (and names folded in from them)
//
// A module supports forward referencing of its names (except permissions and allocators).
// It also supports name resolution of namespace qualified, public names.
typedef struct ModuleAstNode {
	NamedAstHdr;
    Nodes *nodes;			// All parsed nodes owned by the module
    Inodes *namednodes;		// The module's public, owned named nodes
} ModuleAstNode;

ModuleAstNode *newModuleNode();
void modPrint(ModuleAstNode *mod);
void modAddNode(ModuleAstNode *mod, AstNode *node);
void modHook(ModuleAstNode *oldmod, ModuleAstNode *newmod);
void modPass(PassState *pstate, ModuleAstNode *mod);

#endif