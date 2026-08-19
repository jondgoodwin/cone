/** Name and Member Use nodes.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// A list of module names (qualifiers)
typedef struct NameList {
    uint16_t avail;     // Max. number of names allocated for
    uint16_t used;      // Number of names stored in list
    ModuleNode *basemod;  // base module (root or current) holding qualifiers
    // Name* pointers for qualifiers follow, starting here
} NameList;

// Create a new name use node
NameUseNode *newNameUseNode(Name *namesym) {
    NameUseNode *name;
    newNode(name, NameUseNode, NameUseTag);
    // Type checking replaces this with the declaration's type. A use inside a
    // template that is only ever cloned -- a trait's default method, a generic's
    // body -- is never type checked at all, and keeps it.
    name->vtype = unknownType;
    name->qualNames = NULL;
    name->dclnode = NULL;
    name->namesym = namesym;
    return name;
}

// The same, positioned on an existing node rather than on wherever the lexer
// happens to be. A name use the parser synthesizes after it has consumed the
// construct it belongs to would otherwise carry the position of whatever the
// lexer had moved on to.
NameUseNode *newNameUseFromLex(Name *namesym, INode *lexnode) {
    NameUseNode *name = newNameUseNode(namesym);
    inodeLexCopy((INode*)name, lexnode);
    return name;
}

// Create a working variable for a value we intend to reuse later
// The vardcl is appended to a list of nodes, and the nameuse node to it is returned
INode *newNameUseAndDcl(Nodes **nodesp, INode *val, uint16_t scope) {
    VarDclNode *var = (VarDclNode*)newVarDclFull(tempName, VarDclTag, unknownType, (INode*)immPerm, val);
    var->scope = scope;
    nodesAdd(nodesp, (INode*)var);
    NameUseNode *varuse = newNameUseNode(tempName);
    varuse->tag = VarNameUseTag;
    varuse->dclnode = (INode*)var;
    return (INode*)varuse;
}

// Create a new nameuse node pointing to an existing dclnode
INode *newNameUseFromDclNode(INode *dclnode, INode *lexnode) {
    Name *name = inodeGetName(dclnode);
    NameUseNode *fnuse = newNameUseNode(name);
    inodeLexCopy((INode*)fnuse, lexnode);
    fnuse->dclnode = dclnode;
    if (isTypeNode(dclnode))
        fnuse->tag = TypeNameUseTag;
    else {
        fnuse->tag = VarNameUseTag;
        fnuse->vtype = ((IExpNode*)dclnode)->vtype;
    }
    fnuse->tag = isTypeNode(dclnode) ? TypeNameUseTag : VarNameUseTag;
    return (INode *)fnuse;
}

NameUseNode *newMemberUseNode(Name *namesym) {
    NameUseNode *name;
    newNode(name, NameUseNode, MbrNameUseTag);
    name->vtype = unknownType;
    name->qualNames = NULL;
    name->dclnode = NULL;
    name->namesym = namesym;
    return name;
}

// Clone NameUse
INode *cloneNameUseNode(CloneState *cstate, NameUseNode *node) {
    NameUseNode *newnode;
    newnode = memAllocBlk(sizeof(NameUseNode));
    memcpy(newnode, node, sizeof(NameUseNode));
    newnode->dclnode = cloneDclFix(node->dclnode);
    return (INode *)newnode;
}

// If a NameUseNode has module name qualifiers, it will first set basemod
// (either root module or the current module scope). This allocates an area
// for qualifiers to be added.
void nameUseBaseMod(NameUseNode *node, ModuleNode *basemod) {
    node->qualNames = (NameList *)memAllocBlk(sizeof(NameList) + 4 * sizeof(Name*));
    node->qualNames->avail = 4;
    node->qualNames->used = 0;
    node->qualNames->basemod = basemod;
}

// Add a module name qualifier to the end of the list
void nameUseAddQual(NameUseNode *node, Name *name) {
    uint16_t used = node->qualNames->used;
    if (used + 1 >= node->qualNames->avail) {
        NameList *oldlist = node->qualNames;
        uint16_t newavail = oldlist->avail << 1;
        node->qualNames = (NameList *)memAllocBlk(sizeof(NameList) + newavail * sizeof(Name*));
        node->qualNames->avail = newavail;
        node->qualNames->used = used;
        node->qualNames->basemod = oldlist->basemod;
        Name **oldp = (Name**)(oldlist + 1);
        Name **newp = (Name**)(node->qualNames + 1);
        uint16_t cnt = used;
        while (cnt--)
            *newp++ = *oldp++;
    }
    Name **namep = (Name**)&(node->qualNames + 1)[used];
    *namep = name;
    ++node->qualNames->used;
}

// Serialize a name use node
void nameUsePrint(NameUseNode *name) {
    if (name->qualNames) {
        // if root: inodeFprint("::");
        uint16_t cnt = name->qualNames->used;
        Name **namep = (Name**)(name->qualNames + 1);
        while (cnt--)
            inodeFprint("%s::", &(*namep++)->namestr);
    }
    inodeFprint("%s", &name->namesym->namestr);
}

// Handle name resolution for name use references
// - Point to name declaration in other module or this one
// - If name is for a method or field, rewrite node as 'self.field'
// - If not method/field, re-tag it as either TypeNameUse or VarNameUse
void nameUseNameRes(AnalysisState *pstate, NameUseNode **namep) {
    NameUseNode *name = *namep;

    // If name is already "resolved", we are done.
    // This will happen with de-sugaring logic that creates pre-resolved phantom variables
    if (name->dclnode)
        return;

    // For module-qualified names, look up name in that module
    if (name->qualNames) {
        // Do iterative look ups of module qualifiers beginning with basemod
        ModuleNode *qualmod = name->qualNames->basemod;
        Namespace *namespace = &qualmod->namespace;
        uint16_t cnt = name->qualNames->used;
        Name **namep = (Name**)(name->qualNames + 1);
        while (cnt--) {
            INode *foundnode = namespaceFind(namespace, *namep++);
            if (foundnode == NULL) {
                errorMsgNode((INode*)name, ErrorUnkName, "Namespace %s does not exist", &(*--namep)->namestr);
                return;
            }
            else if (foundnode->tag == ModuleTag) {
                qualmod = (ModuleNode*)foundnode;
                namespace = &qualmod->namespace;
            }
            else if (foundnode->tag == StructTag)
                namespace = &((StructNode*)foundnode)->namespace;
            else {
                errorMsgNode((INode*)name, ErrorUnkName, "%s is not a valid namespace", &(*--namep)->namestr);
                return;
            }
        }
        name->dclnode = namespaceFind(namespace, name->namesym);

        // A private name belongs to the module that declares it, and qualifying
        // reaches past that. Refusing it here is what refmodule.html says, and
        // is also the only answer generation can honour: it emits no symbol for
        // a private declaration of a module whose bodies this compile does not
        // generate, so the call site would otherwise be left with nothing to
        // call. A private candidate selected through a *public* overload name is
        // untouched by this, because the program never names it.
        //
        // The declaration stays attached after the diagnostic: it is the one the
        // program asked for, and leaving the use unresolved would only hand the
        // next pass a null to trip over.
        if (name->dclnode && qualmod != pstate->mod && name->namesym->namestr == '_')
            errorMsgNode((INode*)name, ErrorNotPublic,
                "%s is private to its module and may not be named from outside it.",
                &name->namesym->namestr);
    }
    else
        // For non-qualified names (current module), should already be hooked in global name table
        name->dclnode = name->namesym->node;

    if (!name->dclnode) {
        errorMsgNode((INode*)name, ErrorUnkName, "The name %s does not refer to a declared name", &name->namesym->namestr);
        return;
    }

    // If name is for a method or field, rewrite node as 'self.field'
    if (name->dclnode->tag == FieldDclTag && name->dclnode->flags & FlagMethFld) {
        // Doing this rewrite ensures we reuse existing type check and gen code for
        // properly handling field access
        NameUseNode *selfnode = newNameUseNode(selfName);
        copyNodeLex(selfnode, name);
        FnCallNode *fncall = newFnCallNode((INode *)selfnode, 0);
        fncall->methfld = (INode*)name;
        fncall->methfld->tag = MbrNameUseTag;
        copyNodeLex(fncall, name); // Copy lexer info into injected node in case it has errors
        *((FnCallNode**)namep) = fncall;
        inodeNameRes(pstate, (INode **)namep);
        return;
    }

    // Distinguish whether a name is for a variable/function name vs. type
    if (name->dclnode->tag == VarDclTag 
        || name->dclnode->tag == FnDclTag 
        || name->dclnode->tag == FnOverloadDclTag
        || name->dclnode->tag == ConstDclTag)
        name->tag = VarNameUseTag;
    else if (name->dclnode->tag == MacroDclTag)
        name->tag = MacroNameTag;
    else if (name->dclnode->tag == GenVarDclTag)
        name->tag = GenVarUseTag;
    else
        name->tag = TypeNameUseTag;
}

// Handle type check for variable/function name use references
void nameUseTypeCheck(AnalysisState *pstate, NameUseNode **namep) {
    NameUseNode *name = *namep;
    // An overload name has no value of its own: it names a set of concrete
    // declarations. Only a call may use it, and the call type check selects and
    // rewrites this use to the concrete declaration before reaching here.
    if (name->dclnode->tag == FnOverloadDclTag) {
        errorMsgNode((INode*)name, ErrorOverloadUse,
            "The overload name %s may only be used as the name being called. Use a concrete name for its value.",
            &name->namesym->namestr);
        name->vtype = unknownType;
        return;
    }
    // Rule 1: reaching a name analyzes the declaration it names, so what is read
    // below is a finished type rather than whatever source order happened to
    // leave behind. A declaration already analyzed returns at once; one still
    // under analysis returns having established its own type, which is all a use
    // needs and is what lets two functions call each other.
    inodeTypeCheckAny(pstate, &name->dclnode);

    // Rule 6: a constant, and a variable or field whose type is inferred, take
    // their type *from* the value that may name them back, so re-entering one
    // before that type exists leaves nothing to answer with. Every other
    // declaration has established its type by now, so an unknown type here is
    // exactly a definition depending on itself.
    INode *dcl = name->dclnode;
    if (((IExpNode*)dcl)->vtype == unknownType
        && (dcl->flags & Analyzing) && !(dcl->flags & Analyzed)) {
        errorMsgNode((INode*)name, ErrorCircular,
            "%s is defined in terms of itself, so it has no type to give.",
            &name->namesym->namestr);
        name->vtype = errorType;
        return;
    }

    name->vtype = ((IExpNode*)dcl)->vtype;
}

// Handle type check for type name use references
void nameUseTypeCheckType(AnalysisState *pstate, NameUseNode **namep) {
    // Do type check on the type declaration this refers to,
    // to ensure it is correct and knows about its infectious constraints
    // Guards are in place to ensure this only will be done once, as early as possible.
    INode **dclnode = &(*namep)->dclnode;
    if (((*dclnode)->flags & Analyzing) && !((*dclnode)->flags & Analyzed)) {
        errorMsgNode((INode*)*namep, ErrorRecurse, "Recursive types are not supported for now.");
        return;
    }
    else
        inodeTypeCheckAny(pstate, dclnode);
}

// Ensure variable has a usable value
void nameuseFlow(FlowState *fstate, NameUseNode **nodep) {
    NameUseNode *node = *nodep;
    VarDclNode *vardclnode = (VarDclNode *)((NameUseNode*)node)->dclnode;
    if (vardclnode->tag != VarDclTag)
        return;
    if (!(vardclnode->flowtempflags & VarInitialized))
        errorMsgNode((INode*)node, ErrorMove, "This variable has not been initialized. There is no value to use.");
    else if (vardclnode->flowtempflags & VarMoved)
        errorMsgNode((INode*)node, ErrorMove, "This variable's value has been moved out. It is no longer there to use.");
}