/** Permission types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef permission_h
#define permission_h

#include "type.h"

struct AstNode;

// Permission type info
typedef struct PermTypeInfo {
	TypeHeader;
	uint8_t ptype;
	uint8_t flags;
	struct AstNode *locker;
} PermTypeInfo;

// Permission types
enum PermType {
	MutPerm,
	MmutPerm,
	ImmPerm,
	ConstPerm,
	ConstxPerm,
	MutxPerm,
	IdPerm,
	LockPerm
};

// Permission type flags
#define MayRead 0x01		// If on, the contents may be read
#define MayWrite 0x02		// If on, the contents may be mutated
// If on, another live alias may be created able to read or mutate the contents. 
// If off, any new alias turns off use of the old alias for the lifetime of the new alias.
#define MayAlias 0x04		
#define MayAliasWrite 0x08	// If on, another live alias may be created able to write the contents.
#define MaySend 0x10		// If on, a reference may be sent to another thread.
// If on, no locks are needed to read or mutate the contents. 
// If off, the permission's designated locking mechanism must be wrapped around all content access.
#define MayUnlock 0x20
// If on, lifetime-constrained interior references may be borrowed from this alias.
#define MayIntRef 0x40

void permInit();

#endif