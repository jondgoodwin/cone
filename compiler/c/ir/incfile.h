/** Generating a package's include file
 *
 * A library compile writes its package's include file: the Cone source a
 * program compiles against in place of the package's source. It is the root
 * module's own source text with what an importer does not need taken out -- a
 * body an importer does not expand, a declaration it cannot reach -- and
 * 'extern' written in where a body was (compiler/c/doc/nodes/module.md,
 * "Generating the include file").
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef incfile_h
#define incfile_h

// Generate the include file of the program's root module, after type check.
// Returns its text, NUL-terminated, with its length in '*lenp'; or NULL where
// something it would have to declare cannot be written yet, each reported
// (ErrorIncSubmodule, ErrorIncCheck).
char *incFileGenerate(ProgramNode *pgm, size_t *lenp);

#endif
