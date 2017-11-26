/** Symbols and Symbol Table.
 * @file
 *
 * The lexer generates symbolic, hashed tokens to speed up parsing matches (== vs. strcmp).
 * Every symbol is immutable and uniquely hashed based on its string characters.
 *
 * All symbols are hashed and stored in the global symbol table.
 * The symbol's table entry points to an allocated block that holds its current "value", computed hash and c-string.
 *
 * Dan Bernstein's djb algorithm is the hash function for strings, being fast and effective
 * The symbol table uses open addressing (vs. chaining) with quadratic probing (no Robin Hood).
 * The symbol table starts out large, but will double in size whenever it gets close to full.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "symbol.h"
#include "memory.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

// Public globals
size_t gSymbols = 16384;	// Initial maximum number of unique symbols (must be power of 2)
unsigned int gSymTblUtil = 80;	// % utilization that triggers doubling of table

// Private globals
Symbol *gSymTable = NULL;	// The symbol table array
size_t gSymTblAvail = 0;	// Number of allocated symbol table slots (power of 2)
size_t gSymTblCeil = 0;		// Ceiling that triggers table growth
size_t gSymTblUsed = 0;		// Number of symbol table slots used

/** String hash function (djb: Dan Bernstein)
 * Ref: http://www.cse.yorku.ca/~oz/hash.html
 * Ref: http://www.partow.net/programming/hashfunctions/#AvailableHashFunctions
 * Would this help?: hash ^= hash >> 11; // Extra randomness for long symbols?
 */
#define symHashFn(hash, strp, strl) \
{ \
	char *p; \
	size_t len = strl; \
	p = strp; \
	hash = 5381; \
	while (len--) \
		hash = ((hash << 5) + hash) ^ ((size_t)*p++); \
}

/** Modulo operation that calculates primary table entry from symbol's hash.
 * 'size' is always a power of 2 */
#define symHashMod(hash, size) \
	(assert(((size)&((size)-1))==0), (size_t) ((hash) & ((size)-1)) )

/** Calculate index into symbol table for a symbol using quadratic probing
 * The table's slot at index is either empty or matches the provided symbol/hash
 * http://stackoverflow.com/questions/2348187/moving-from-linear-probing-to-quadratic-probing-hash-collisons/2349774#2349774
 */
#define symFindSlot(tblp, hash, strp, strl) \
{ \
	size_t tbli, step; \
	for (tbli = symHashMod(hash, gSymTblAvail), step = 1;; ++step) { \
		char *symtstr; \
		Symbol *slot; \
		slot = &gSymTable[tbli]; \
		if (slot->name==NULL || (slot->hash == hash && (symtstr=slot->name)[strl]=='\0' && strncmp(strp, symtstr, strl)==0)) \
			break; \
		tbli = symHashMod(tbli + step, gSymTblAvail); \
	} \
	tblp = &gSymTable[tbli]; \
}

/** Grow the symbol table, by either creating it or doubling its size */
void symGrow() {
	size_t oldTblAvail;
	Symbol *oldTable;
	size_t newTblMem;
	size_t oldslot;

	// Preserve old table info
	oldTable = gSymTable;
	oldTblAvail = gSymTblAvail;

	// Allocate and initialize new symbol table
	gSymTblAvail = oldTblAvail==0? gSymbols : oldTblAvail<<1;
	gSymTblCeil = (gSymTblUtil * gSymTblAvail) / 100;
	newTblMem = gSymTblAvail * sizeof(Symbol);
	gSymTable = (Symbol*) memAllocBlk(newTblMem);
	memset(gSymTable, 0, newTblMem); // Fill with NULL pointers & 0s

	// Copy existing symbol slots to re-hashed positions in new table
	for (oldslot=0; oldslot < oldTblAvail; oldslot++) {
		Symbol *oldslotp, *newslotp;
		oldslotp = &oldTable[oldslot];
		if (oldslotp->name) {
			char *strp;
			size_t strl, hash;
			strp = oldslotp->name;
			strl = strlen(strp);
			hash = oldslotp->hash;
			symFindSlot(newslotp, hash, strp, strl);
			newslotp->name = oldslotp->name;
			newslotp->hash = oldslotp->hash;
			newslotp->node = oldslotp->node;
		}
	}
	// memFreeBlk(oldTable);
}

/** Get pointer to SymId for the symbol's string. 
 * For unknown symbol, this allocates memory for the string (SymId) and adds it to symbol table. */
Symbol *symFind(char *strp, size_t strl) {
	size_t hash;
	Symbol *slotp;

	// Hash provide string into table
	symHashFn(hash, strp, strl);
	symFindSlot(slotp, hash, strp, strl);

	// If not already a symbol, allocate memory for string and add to table
	if (slotp->name == NULL) {
		// Double table if it has gotten too full
		if (++gSymTblUsed >= gSymTblCeil)
			symGrow();

		// Populate symbol info into table
		slotp->name = memAllocStr(strp, strl);
		slotp->hash = hash;
		slotp->node = NULL;		// Undefined symbol
	}
	return slotp;
}

// Initialize symbol table
void symInit() {
	symGrow();
}
