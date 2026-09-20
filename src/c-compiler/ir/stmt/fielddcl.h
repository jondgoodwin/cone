/** Handling for field declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fielddcl_h
#define fielddcl_h

// The fold clause a field declaration may carry -- 'use a, b as c' or
// 'use * but d' -- saying which names of the field's type become names of the
// type that declares the field, under what spelling. Expanded into that type's
// namespace by structFoldExpand, which is what binds each item's target.
typedef struct FoldClause {
    INode *at;          // A node positioned at the clause's 'use': where its diagnostics land, and where a star-made alias is placed
    Nodes *items;       // An AliasDclNode per admitted name: the local spelling, targeting the spelling in the field's type
    Nodes *excludes;    // A member name use per name after 'but' (NULL when none)
    uint16_t star;      // 'use *': every public member not excluded; the items are made at expansion
    uint16_t expanded;  // Expansion has run on this node (a clone starts over)
} FoldClause;

// Field declaration node. Two of its slots serve name folding, and each is
// empty when it does not apply: a declared field may carry a fold clause; a
// folded copy carries a hop. A folded copy is a copy of the field it stands
// for -- its own type, permission and index within its own type -- reached
// through the field of this type that 'hop' names, which is a declared field
// or another copy, so the chain always ends at a declared field. A copy lives
// in the type's namespace only, never in its field list.
typedef struct FieldDclNode {
    IExpNodeHdr;               // 'vtype': field's type
    Name *namesym;
    INode *value;              // Default value (NULL if not initialized)
    INode *perm;               // Permission type (often mut or imm)
    FoldClause *fold;          // The field's fold clause, or NULL
    struct FieldDclNode *hop;  // On a folded copy: the field of this type it is reached through; NULL on a declared field
    uint16_t index;            // field's index within the type
    uint16_t vtblidx;          // field's index within the type's vtable
} FieldDclNode;


FieldDclNode *newFieldDclNode(Name *namesym, INode *perm);

// Create an empty fold clause, positioned where the lexer is (the 'use')
FoldClause *newFoldClause();

// Create a new field node that is a copy of an existing one
INode *cloneFieldDclNode(CloneState *cstate, FieldDclNode *node);

void fieldDclPrint(FieldDclNode *fn);

// Name resolution of field declaration
void fieldDclNameRes(NameResState *pstate, FieldDclNode *node);

// Type check field declaration
void fieldDclTypeCheck(TypeCheckState *pstate, FieldDclNode *node);

#endif
