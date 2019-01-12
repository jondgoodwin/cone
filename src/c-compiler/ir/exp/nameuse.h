/** Name and Member Use Nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef nameuse_h
#define nameuse_h

typedef struct NameList NameList;

// Name use node, which ultimately points to the applicable declaration for the name
// The node's name may optionally include module name qualifiers. Used by:
// - NameUseTag. A name token prior to name resolution pass
// - VarNameUseTag. A name use node resolved to a variable or function declaration
// - TypeNameUseTag. A name use node resolved to a type declaration
// - MbrNameUseTag. A method or property name being applied to some value
typedef struct NameUseNode {
    ITypedNodeHdr;
    Name *namesym;            // Pointer to the global name table entry
    INamedNode *dclnode;    // Node that declares this name (NULL until names are resolved)
    NameList *qualNames;    // Pointer to list of module qualifiers (NULL if none)
} NameUseNode;

NameUseNode *newNameUseNode(Name *name);
void nameUseBaseMod(NameUseNode *node, ModuleNode *basemod);
void nameUseAddQual(NameUseNode *node, Name *name);
NameUseNode *newMemberUseNode(Name *namesym);
void nameUsePrint(NameUseNode *name);
void nameUseWalk(PassState *pstate, NameUseNode **name);

#endif