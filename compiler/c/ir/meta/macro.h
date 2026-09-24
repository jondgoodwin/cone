/** Handling for generic nodes (also used for macros)
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef macro_h
#define macro_h

// Macro declaration node
typedef struct MacroDclNode {
    IExpNodeHdr;             // 'vtype': type of this name's value
    Name *namesym;
    Nodes *parms;            // Declared parameter nodes w/ defaults (GenVarTag)
    INode *body;             // The body of the generic
    Nodes *memonodes;        // Pairs of memoized generic calls and cloned bodies
} MacroDclNode;

// Create a new macro declaraction node
MacroDclNode *newMacroDclNode(Name *namesym);

// Deep copy a macro declaration, for a generic type's instance
INode *cloneMacroDclNode(CloneState *cstate, MacroDclNode *node);

void macroPrint(MacroDclNode *fn);

// Name resolution
void macroNameRes(NameResState *pstate, MacroDclNode *node);

// Type check generic
void macroTypeCheck(TypeCheckState *pstate, MacroDclNode *node);

// Expand a macro named where a value is expected
void macroNameTypeCheck(TypeCheckState *pstate, NameUseNode **macro);

// Expand a macro called by name, substituting the arguments for its parameters
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep);

// Expand a macro method called on a receiver, which stands in for 'self'
void macroMethodTypeCheck(TypeCheckState *pstate, FnCallNode **nodep, MacroDclNode *macro);

#endif
