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
#include "type.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

// Public globals
size_t gSymTblSlots = 16384;	// Initial maximum number of unique symbols (must be power of 2)
unsigned int gSymTblUtil = 80;	// % utilization that triggers doubling of table

// The structure of a slot in the symbol table
typedef struct SymTblSlot {
	SymId *id;		// Symbol's allocated info (AST-node and string)
	size_t hash;	// Symbol's computed hash
} SymTblSlot;

// Private globals
SymTblSlot *gSymTable = NULL;	// The symbol table array
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
		SymTblSlot *slot; \
		slot = &gSymTable[tbli]; \
		if (slot->id==NULL || (slot->hash == hash && (symtstr=symIdToStr(slot))[strl]=='\0' && strncmp(strp, symtstr, strl))) \
			break; \
		tbli = symHashMod(tbli + step, gSymTblAvail); \
	} \
	tblp = &gSymTable[tbli]; \
}

/** Grow the symbol table, by either creating it or doubling its size */
void symGrow() {
	size_t oldTblAvail;
	SymTblSlot *oldTable;
	size_t newTblMem;
	size_t oldslot;

	// Preserve old table info
	oldTable = gSymTable;
	oldTblAvail = gSymTblAvail;

	// Allocate and initialize new symbol table
	gSymTblAvail = oldTblAvail==0? gSymTblSlots : oldTblAvail<<1;
	gSymTblCeil = (gSymTblUtil * gSymTblAvail) / 100;
	newTblMem = gSymTblAvail * sizeof(SymTblSlot);
	gSymTable = (SymTblSlot*) memAllocBlk(newTblMem);
	memset(gSymTable, 0, newTblMem); // Fill with NULL pointers & 0s

	// Copy existing symbol slots to re-hashed positions in new table
	for (oldslot=0; oldslot < oldTblAvail; oldslot++) {
		SymTblSlot *oldslotp, *newslotp;
		oldslotp = &oldTable[oldslot];
		if (oldslotp->id) {
			char *strp;
			size_t strl, hash;
			strp = symIdToStr(oldslotp->id);
			strl = strlen(strp);
			hash = oldslotp->hash;
			symFindSlot(newslotp, hash, strp, strl);
			newslotp->id = oldslotp->id;
			newslotp->hash = oldslotp->hash;
		}
	}
	// memFreeBlk(oldTable);
}

/** Get pointer to SymId for the symbol's string. 
 * For unknown symbol, this allocates memory for the string (SymId) and adds it to symbol table. */
SymId *symGetId(char *strp, size_t strl) {
	size_t hash;
	SymTblSlot *slotp;

	// Double table if it has gotten too full
	if (gSymTblCeil <= gSymTblUsed)
		symGrow();

	// Hash provide string into table
	symHashFn(hash, strp, strl);
	symFindSlot(slotp, hash, strp, strl);

	// If not already a symbol, allocate memory for string and add to table
	if (slotp->id == NULL) {
		SymId *symid;
		gSymTblUsed++;
		slotp->hash = hash;
		slotp->id = symid = (SymId*) memAllocBlk(strl+1+sizeof(SymId));
		symid->val = NULL;
		strncpy((char*)(symid+1), strp, strl);
		((char*)(symid+1))[strl] = '\0';
	}
	return slotp->id;
}

// Initialize symbol table
void symInit() {
	symGrow();
	typInit();
}
