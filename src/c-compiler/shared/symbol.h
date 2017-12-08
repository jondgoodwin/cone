/** Hashed symbol table
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef symbol_h
#define symbol_h

typedef struct AstNode AstNode;	// ../ast/ast.h

#include <stdlib.h>

/* *****************************************************
 * Symbols and Symbol Table - symbol.c
 * *****************************************************/

// Configurable size for symbol table and % utilization trigger
size_t gSymbols;		// Initial maximum number of unique symbols (must be power of 2)
unsigned int gSymTblUtil;	// % utilization that triggers doubling of table

// Symbol info (a slot in the symbol table)
typedef struct Symbol {
	char *namestr;		// Symbol's name
	AstNode *node;	// AST node currently assigned to symbol
	size_t hash;	// Symbol's computed hash
} Symbol;

// Get pointer to Symbol for the symbol's string in the symbol table 
// For unknown symbol, it allocates memory for the string and adds it to symbol table.
Symbol *symFind(char *strp, size_t strl);

size_t symUnused();
	
// Initialize the symbol table with reserved symbols
void symInit();

#endif