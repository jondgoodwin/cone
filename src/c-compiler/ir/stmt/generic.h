/** Handling for generic nodes (also used for macros)
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef generic_h
#define generic_h

// Generic declaration node
typedef struct GenericNode {
    IExpNodeHdr;             // 'vtype': type of this name's value
    Name *namesym;
    Nodes *parms;            // Declared parameter nodes w/ defaults (GenVarTag)
    INode *body;             // The body of the generic
    Nodes *memonodes;        // Pairs of memoized generic calls and cloned bodies
} GenericNode;

GenericNode *newGenericNode(Name *namesym);

void genericPrint(GenericNode *fn);

// Name resolution
void genericNameRes(NameResState *pstate, GenericNode *node);

// Type check generic
void genericTypeCheck(TypeCheckState *pstate, GenericNode *node);

// Type check generic name use
void genericNameTypeCheck(TypeCheckState *pstate, NameUseNode **macro);

// Instantiate a generic using passed arguments
void genericCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep);

#endif