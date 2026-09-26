/** Name and Member Use Nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef nameuse_h
#define nameuse_h

// Name use node: every appearance of a name, which name resolution points at
// the declaration it names. One tag, NameUseTag, throughout; whether the use is
// a type, a value or a macro is asked of that declaration (nameUseGroup). A
// member name -- a field, method or operator applied to a value -- is the same
// node, held in a call's member slot and bound only when type check selects the
// member against the receiver's type.
//
// A name reached through a namespace -- 'math3d.Point3' -- is parsed as a
// member access and collapsed into one of these by fnCallNameRes, which stamps
// FlagQualified on it. Nothing after that needs to know which namespace it came
// through, only that it was not written bare: a bare method or field name is
// lowered to 'self.name' and a qualified one never is.
typedef struct NameUseNode {
    IExpNodeHdr;
    Name *namesym;          // Pointer to the global name table entry
    INode *dclnode;         // Node that declares this name (NULL until names are resolved)
} NameUseNode;

NameUseNode *newNameUseNode(Name *name);
NameUseNode *newNameUseFromLex(Name *name, INode *lexnode);

// Create a working variable for a value we intend to reuse later
// The vardcl is appended to a list of nodes, and the nameuse node to it is returned
INode *newNameUseAndDcl(Nodes **nodesp, INode *val, uint16_t scope);

// Create a new nameuse node pointing to an existing dclnode
INode *newNameUseFromDclNode(INode *dclnode, INode *lexnode);

// Clone NameUse
INode *cloneNameUseNode(CloneState *cstate, NameUseNode *node);

// The declaration a name use names, at the end of its chain of names,
// or NULL while it is unresolved
INode *nameUseGetDcl(NameUseNode *name);

// The group a name use belongs to (ExpGroup, TypeGroup or MetaGroup),
// asked of the declaration it names
NodeGroup nameUseGroup(NameUseNode *name);

// Does this node name a declaration with the given tag? No for a node that is
// not a name use, and for a name use bound to nothing yet
int nameUseNames(INode *node, uint16_t dcltag);

// Create a member name, to be applied to a value and bound by type check
NameUseNode *newMemberUseNode(Name *namesym);
void nameUsePrint(NameUseNode *name);
// Handle name resolution for name use references: bind dclnode to the
// declaration the name refers to, in this module or another
void nameUseNameRes(NameResState *pstate, NameUseNode **namep);
// Inside a body an importer expands (NameResState.expander), mark what a bound
// name use names DclExpandReached, for a library compile to export
void nameUseMarkExpandReached(NameResState *pstate, NameUseNode *name);

// Point a bare use of a generic base's member, made inside an extension's braces,
// at the member of the base instance the checked function's extension stands on
void nameUseBaseInstanceMember(TypeCheckState *pstate, NameUseNode *name);

// Report a use of a generic type's own member, which only its instances have,
// at the use 'name', and return 1 if 'dcl' is one
int nameUseTemplateMember(NameUseNode *name, INode *dcl);

// Handle type check for variable/function name use references
void nameUseTypeCheck(TypeCheckState *pstate, NameUseNode **name);

// Handle type check for type name use references
void nameUseTypeCheckType(TypeCheckState *pstate, NameUseNode **name);

// Handle flow checking for nameuse
void nameuseFlow(FlowState *fstate, NameUseNode **nodep);
// Handle flow checking for the variable at the root of a borrowed place
void nameuseFlowBorrowed(FlowState *fstate, NameUseNode **nodep);

#endif
