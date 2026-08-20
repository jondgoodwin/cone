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

void macroPrint(MacroDclNode *fn);

// Name resolution
void macroNameRes(NameResState *pstate, MacroDclNode *node);

// Type check generic
void macroTypeCheck(TypeCheckState *pstate, MacroDclNode *node);

// Type check generic name use
void macroNameTypeCheck(TypeCheckState *pstate, NameUseNode **macro);

// Instantiate a generic using passed arguments
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep);

#endif
