/** Compiler Helpers: Memory, File i/o, Symbols, AST
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef helpers_h
#define helpers_h

#include <stdio.h>
#include "helpers/ast.h"

// Exit error codes
enum Exit {
	ExitError,	// Program fails to compile due to caught errors
	ExitNF,		// Could not find specified source files
	ExitMem,	// Out of memory
	ExitOpts	// Invalid compiler options
};

/* *****************************************************
 * Memory - memory.c
 * *****************************************************/

// Configurable size for arenas (specify as multiples of 4096 byte pages)
size_t gMemBlkArenaSize;	// Default is 256 pages
size_t gMemStrArenaSize;	// Default is 128 pages

// memory.c has static globals for arena bookkeeping

// Allocate memory for a block, aligned to a 16-byte boundary
void *memAllocBlk(size_t size);

// Allocate memory for a string and copy contents over, if not NULL
// Allocates extra byte for string-ending 0, appending it to copied string
char *memAllocStr(char *str, size_t size);

/* *****************************************************
 * File i/o - fileio.c
 * *****************************************************/

// Load a file into an allocated string, return pointer or NULL if not found
char *fileLoad(char *fn);
// Send an error message to stderr
void fileErrorMsg(const char *msg, ...);

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

#endif