/** Handling for array literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef arraylit_h
#define arraylit_h

// Type check an array literal (used by region allocation only)
void arrayLitTypeCheckDimExp(AnalysisState *pstate, ArrayNode *arrlit);

// Type check an array literal
void arrayLitTypeCheck(AnalysisState *pstate, ArrayNode *arrlit);

// Perform data flow analysis on an array literal's element values
void arrayLitFlow(FlowState *fstate, ArrayNode **nodep);

// Is an array actually a literal?
int arrayLitIsLiteral(ArrayNode *node);

#endif
