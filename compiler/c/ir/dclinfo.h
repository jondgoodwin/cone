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
    char *cname;        // What '@c("...")' stated, or NULL: on a module, the prefix
                        // every C name it gives carries; on a fn, its whole symbol
} DclInfo;

enum DclFacts {
    DclPrivate    = 0x0001,   // Not declared 'pub': visible only within its owner
    DclExternal   = 0x0002,   // 'extern': defined elsewhere, so this compile emits no definition.
                              // Says nothing about the name: that is the module's, or '@c''s
    DclCName      = 0x0004,   // C naming, from '@c': on a module, every function and global
                              // it owns directly takes a C name; on a fn or global, its
                              // symbol is 'cname', or the owning C module's prefix and its
                              // name, never mangled
    DclSystemCC   = 0x0008,   // '@c(system)': the system calling convention (stdcall on
                              // x86 Windows); on a module, for every function it names.
                              // An 'extern' one is also imported from a DLL
    DclNamesChain = 0x0010,   // Module only: contributes its name to the owner chain
    DclExpandReached = 0x0020 // Named by a body an importer expands in its own object: an
                              // inline, generic or macro body, a trait default, a
                              // generic type's method. Written by name resolution
                              // (nameUseNameRes); a library compile exports such a
                              // definition, and a type's reachable functions
                              // (genlIsExported)
};

#define dclInfoInit(dclinfo) ((dclinfo)->owner = NULL, (dclinfo)->facts = 0, (dclinfo)->cname = NULL)

// The facts the parser writes straight onto a declaration from its '@c', before
// it joins anything, and which joining keeps
#define DclStated (DclCName | DclSystemCC)

// Record that a declaration has joined the namespace of 'owner': set its owner
// and write its facts from its parser flags, keeping what its own '@c' stated.
// A function or global a C-named module owns directly takes the module's C
// naming. No-op for a node without DclInfo.
void dclInfoJoin(INode *node, INode *owner);

// The nearest module enclosing a declaration (the node itself, if a module), or NULL
ModuleNode *dclInfoGetModule(INode *node);

// Serialize a declaration's owner chain and facts, for the IR dump
void dclInfoPrint(INode *node);

#endif
