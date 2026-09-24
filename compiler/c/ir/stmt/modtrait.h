/** Module trait node and the conformance a module declares against one
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef modtrait_h
#define modtrait_h

// A MODULE TRAIT is a module's abstraction: 'mod trait Shell { ... }', the
// interface a module plugs into a framework with. It is a declaration of the
// module whose file writes it, as a struct trait is, and is named, imported and
// folded like any other declaration. Its body holds functions and globals, and
// nothing else. A function written with a body, and a global written with an
// initialiser, is a DEFAULT; one written without is a REQUIREMENT.
//
// A module conforms by saying so on its 'mod' line, 'mod prog is Shell;', and
// only by saying so: nothing is inferred from what a module happens to declare.
// Conformance is checked where it is written (modTraitConform, modTraitCheck).
// Each default the module does not declare itself is CLONED into it, as a
// struct trait's defaults are cloned into the struct, and becomes the module's
// own declaration -- owned, spelled and generated as the module's. A module is
// one instance, so conformance is static: no vtable, no dispatch.
//
// The trait's own members are never generated. A requirement has no body, and a
// default's body is the template each conforming module's copy is made from.
typedef struct ModTraitNode {
    INodeHdr;
    Name *namesym;
    DclInfo dclinfo;         // Owner (the declaring module) and visibility
    Nodes *nodes;            // Each member, a FnDclNode or VarDclNode, in the order written
    Namespace namespace;     // Each member by its name
} ModTraitNode;

ModTraitNode *newModTraitNode(Name *namesym);

// Add a parsed member to the trait, refusing a name written twice
void modTraitAddMember(ModTraitNode *trait, INode *member);

void modTraitPrint(ModTraitNode *trait);

// Resolve the trait's members in the trait's own scope: the declaring module's
// names, and over them the trait's own, so a default's body names another member
// bare. Done once, and reached by demand from a conforming module
void modTraitNameRes(NameResState *pstate, ModTraitNode *trait);

// Type check the members' signatures and globals. A default's body is checked
// in each copy, where its names are the conforming module's, and never here
void modTraitTypeCheck(TypeCheckState *pstate, ModTraitNode *trait);

// Resolve the module trait a module's 'mod' line says it 'is', and clone into
// the module each default it does not declare. Attempted at the end of the
// module's own folds (modFoldNames), so what the module holds by folding counts
// and a module folding from it takes the copies; made for any module still
// waiting, and whatever is wrong reported ('report'), after the fold passes and
// before any module's own names are resolved. A requirement nothing meets is
// reported here
void modTraitConform(NameResState *pstate, ModuleNode *mod, int report);

// Check that what the module declares for each member has the member's shape:
// a function with the member's signature, or a global of its type and
// permission. Run at the start of the module's type check, so that both sides
// have their types
void modTraitCheck(TypeCheckState *pstate, ModuleNode *mod);

#endif
