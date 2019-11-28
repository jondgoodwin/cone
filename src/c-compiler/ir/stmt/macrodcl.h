/** Handling for macro declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef macrodcl_h
#define macrodcl_h

// Macro declaration node
typedef struct MacroDclNode {
    IExpNodeHdr;             // 'vtype': type of this name's value
    Name *namesym;
    Nodes *parms;            // Declared parameter nodes w/ defaults (VarDclTag)
    INode *body;             // The body of the macro (a block node)
} MacroDclNode;

MacroDclNode *newMacroDclNode(Name *namesym);

void macroDclPrint(MacroDclNode *fn);

// Name resolution
void macroDclNameRes(NameResState *pstate, MacroDclNode *node);

// Type check macro declaration
void macroDclTypeCheck(TypeCheckState *pstate, MacroDclNode *node);

// Type check macro name use
void macroNameTypeCheck(TypeCheckState *pstate, NameUseNode **macro);

// Instantiate a macro using passed arguments
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep);

#endif