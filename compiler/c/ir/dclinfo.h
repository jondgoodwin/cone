/** Declaration facts: what a symbol-declaring node records about where it lives
 *
 * Embedded by value in the node kinds that declare a name the object file can
 * carry: fn, global variable, struct and module. inodeGetDclInfo (inode.c) is
 * the one switch that knows which kinds those are.
 *
 * Generation derives every declared symbol from this record and the node:
 * nameSymbol (name.c) spells it, walking the owner chain, and genlLinkage
 * (genllvm/genllvm.c) sets its linkage, visibility and calling convention.
 * Nothing else spells a symbol.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef dclinfo_h
#define dclinfo_h

typedef struct ModuleNode ModuleNode;

typedef struct DclInfo {
    INode *owner;       // Enclosing module or type node. NULL for the root module,
                        // and for a declaration no namespace owns (a local, a parameter)
    uint16_t facts;     // DclFacts bits
} DclInfo;

enum DclFacts {
    DclPrivate    = 0x0001,   // Not declared 'pub': visible only within its owner
    DclExternal   = 0x0002,   // Externally supplied: this compile emits no definition
    DclCName      = 0x0004,   // C-style name: no owner prefix, never mangled
    DclSystemCC   = 0x0008,   // System calling convention (stdcall and dllimport today)
    DclNamesChain = 0x0010    // Module only: contributes its name to the owner chain
};

#define dclInfoInit(dclinfo) ((dclinfo)->owner = NULL, (dclinfo)->facts = 0)

// Record that a declaration has joined the namespace of 'owner': set its owner
// and write its facts from its parser flags. No-op for a node without DclInfo.
// Modules are not joined this way; their facts are set where they are created (parsemod.c).
void dclInfoJoin(INode *node, INode *owner);

// The nearest module enclosing a declaration (the node itself, if a module), or NULL
ModuleNode *dclInfoGetModule(INode *node);

// Serialize a declaration's owner chain and facts, for the IR dump
void dclInfoPrint(INode *node);

#endif
