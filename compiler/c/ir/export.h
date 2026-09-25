/** What a library compile exports
 *
 * One answer, asked by two consumers: generation, which gives each exported
 * definition a symbol an importer's object links against, and the include-file
 * generator, which declares exactly those definitions in the file an importer
 * compiles against (compiler/c/doc/nodes/module.md, "Generating the include
 * file"). Both ask these functions, so the object and the include file cannot
 * disagree. They read only facts name resolution and type check recorded.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef export_h
#define export_h

// Whether a declaration belongs to a generic's instance: it is one, or it is a
// member of one, or of an instance of a generic module
int dclIsInstance(INode *dclnode);

// Whether a type's own braces hold a body an importer expands: an inline or
// generic method, a macro method, or -- in a trait or a generic type -- every method
int typeHoldsExpanded(INode *type);

// Whether a function is a type's 'final' or 'clone', which a value of the type
// calls wherever it is dropped or copied, without naming it
int fnIsTypeLifecycle(INode *dclnode);

// Whether a library compile whose root module is 'libroot' exports a definition,
// so that an importer's object links against it. NULL 'libroot' exports nothing
int dclIsExported(ModuleNode *libroot, INode *dclnode);

#endif
