/** Name handling - general purpose
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef name_h
#define name_h

#include <stdlib.h>

// Name is an interned symbol, unique by its collection of characters (<=255)
// A name can be hashed into the global name table or a particular node's namespace.
// The struct for a name is an unmovable allocated block in memory
typedef struct Name {
    INode *node;             // Node currently assigned to name
    size_t hash;             // Name's computed hash
    unsigned char namesz;    // Number of characters in the name (<=255)
    char namestr;            // First byte of name's string (the rest follows)
} Name;

// Common symbols - see nametbl.c
Name *anonName;  // "_" - the absence of a name
Name *selfName;  // "self"
Name *selfTypeName; // "Self"
Name *thisName;  // "this"
Name *cloneName; // "clone" method
Name *finalName; // "final" method

Name *plusEqName;   // "+="
Name *minusEqName;  // "-="
Name *multEqName;   // "*="
Name *divEqName;    // "/="
Name *remEqName;    // "%="
Name *orEqName;     // "|="
Name *andEqName;    // "&="
Name *xorEqName;    // "^="
Name *shlEqName;    // "<<="
Name *shrEqName;    // ">>="
Name *lessDashName; // "<-"

Name *plusName;     // "+"
Name *minusName;    // "-"
Name *multName;     // "*"
Name *divName;      // "/"
Name *remName;      // "%"
Name *orName;       // "|"
Name *andName;      // "&"
Name *xorName;      // "^"
Name *shlName;      // "<<"
Name *shrName;      // ">>"

Name *incrName;     // "++"
Name *decrName;     // "--"
Name *incrPostName; // "_++"
Name *decrPostName; // "_--"

Name *eqName;       // "=="
Name *neName;       // "!="
Name *leName;       // "<="
Name *ltName;       // "<"
Name *geName;       // ">="
Name *gtName;       // ">"

Name *parensName;   // "()"
Name *indexName;    // "[]"
Name *refIndexName; // "&[]"

Name *optionName;   // "Option"

typedef struct VarDclNode VarDclNode;
typedef struct FnDclNode FnDclNode;

// Create new prefix that concatenates a new name to the old prefix, followed by _
void nameConcatPrefix(char **prefix, char *name);
// Create globally unique variable name, prefixed by module/type name
void nameGenVarName(VarDclNode *node, char *prefix);
// Create globally unique mangled function name, prefixed by module/type name
void nameGenFnName(FnDclNode *node, char *prefix);


#endif
