/** Hashed symbol table
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef symbol_h
#define symbol_h

#include "ast.h"

#include <stdlib.h>

/* *****************************************************
 * Symbols and Symbol Table - symbol.c
 * *****************************************************/

// Configurable size for symbol table and % utilization trigger
size_t gSymTblSlots;		// Initial maximum number of unique symbols (must be power of 2)
unsigned int gSymTblUtil;	// % utilization that triggers doubling of table

// A symbol's information in its allocated block
typedef struct SymId {
	AstNode *val;			// Pointer to symbol's AST-node
	// char str[?];			// Symbol's c-string
} SymId;
// Macro to convert a SymInfo pointer to a pointer to its c-string
#define symIdToStr(infop) ((char *) ((infop)+1))

// Grow the symbol table, by either creating it or doubling its size 
void symGrow();
// Get pointer to SymId for the symbol's string. 
// For unknown symbol, it allocates memory for the string (SymId) and adds it to symbol table.
SymId *symGetId(char *strp, size_t strl);

// Initialize the symbol table with reserved symbols
void symInit();

#endif