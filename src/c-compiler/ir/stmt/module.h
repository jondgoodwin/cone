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
typedef struct ModuleNode {
    IExpNodeHdr;
    Name *namesym;           // The module's name: its folder's for a module folder, else its file's
    Name *filesym;           // The name derived from the module's filename
    Name *foldersym;         // The module's folder, when its designated file drew it; else NULL
    Nodes *imports;          // All import nodes
    Nodes *nodes;            // All parsed nodes owned by the module
    Namespace namespace;     // The module's named nodes, owned or "used"
    DclInfo dclinfo;         // Owner and the facts that decide the linker symbols it prefixes
    uint16_t foldstate;      // How far modFoldNames has got: 0 not begun, 1 running, 2 done
} ModuleNode;

ModuleNode *newModuleNode();
void modPrint(ModuleNode *mod);
void modAddNode(ModuleNode *mod, Name *name, INode *node);
void modAddNamedNode(ModuleNode *mod, Name *name, INode *node);

// Add a parsed function to the module, binding its unique concrete name and,
// when it declares an overload name, that name's separate FnOverloadDclNode
void modAddFn(ModuleNode *mod, FnDclNode *fnnode);

void modHook(ModuleNode *oldmod, ModuleNode *newmod);

// Put every name this module holds by FOLDING into its namespace: what its
// imports admit, and what its globals' 'use' clauses do. Ahead of any module's
// name resolution, and dependency-first, so that what a name reaches through a
// qualifier does not depend on the order the files were loaded in
void modFoldNames(NameResState *pstate, ModuleNode *mod);

void modNameRes(NameResState *pstate, ModuleNode *mod);
void modTypeCheck(TypeCheckState *pstate, ModuleNode *mod);

#endif
