/** The Type Pass
*
* The Type pass validates all named type declarations and fills in information about these types.
* It happens after name resolution and before type check/inference of functions and variables.
* It follows types down recursively, avoiding cycles through the use of status flags.
* 
* - A type may not be composed of a zero-size type
* - Pointers allow for recursive types, but otherwise, types must form a directed acyclic graph
* - Infectiousness of types is handled (move semantics, lifetimes, sync, etc.)
* - Subtype and inheritance relationships are filled out
* - The binary encoding is sorted (e.g., ensuring variant types are same size)
*
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef typepass_h
#define typepass_h

typedef struct {
    int a;
} TypePass;

typedef struct ModuleNode ModuleNode;
void doTypePass(ModuleNode *mod);

// Dispatch a node walk for the Type pass
void typePass(TypePass *pstate, INode *node);

#endif