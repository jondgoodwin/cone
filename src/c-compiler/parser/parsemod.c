/** Parse modules, including program
 * @file
 *
 * parsePgm is the entry point into parsing, which translates the lexer's tokens into IR nodes
 * These functions handle all module/global area parsing
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../shared/fileio.h"
#include "../ir/nametbl.h"
#include "../coneopts.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>

// Temporary hack:  The source for the importable stdio package
char *stdiolib =
"pub extern {fn printStr(str &[]u8); fn printCStr(str *u8); fn printFloat(a f64); fn printInt(a i64); fn printUInt(a u64); fn printChar(code u64);}\n"
"pub struct IOStream{"
"  pub fd i32;"
"  pub fn appendStr overload `<-`(self &mut, str &[]u8) {printStr(str);}"
"  pub fn appendCStr overload `<-`(self &mut, str *u8) {printCStr(str);}"
"  pub fn appendInt overload `<-`(self &mut, i i64) {printInt(i);}"
"  pub fn appendFloat overload `<-`(self &mut, n f64) {printFloat(n);}"
"  pub fn appendUInt overload `<-`(self &mut, i u64) {printUInt(i);}"
"}"
"pub mut print = IOStream[0];"
;

void parseGlobalStmts(ParseState *parse, ModuleNode *mod, int atmodstart);
ModuleNode *parseLoadAndParseModuleFile(ParseState *parse, char *filename, Name *filesym);

// ---------------------------------------------------------------------------
// The file registry and the folder sweep
//
// A module's source files are the files of a folder. The compiler is given one
// file and finds the rest itself: the folder's other '.cone' files join the
// module, and so do the files of every subfolder, at any depth. No file set is
// ever hand-listed, and moving a file between folders is a semantic move.
//
// What makes a folder a module folder is the designated file it holds, named for
// the folder -- 'matrix/matrix.cone'. That convention is the whole of the
// trigger: the file the compiler is given sweeps its folder exactly when it is
// that folder's designated file, which is the same probe a subfolder gets. A
// file that is not its folder's designated file is a module of its own, exactly
// today's program, and its neighbours are none of its business.
//
// Locating a file, registering it to a module and parsing it into that module
// are three separate steps, because what must happen exactly once is the
// reading. The registry is keyed by the file's path, so a file is read once and
// belongs to one module however many importers name it.
// ---------------------------------------------------------------------------

// folder + name, where folder carries its trailing slash
char *parsePathJoin(char *folder, char *name) {
    char *path = memAllocStr(folder, strlen(folder) + strlen(name));
    strcat(path, name);
    return path;
}

// The name of the folder whose designated file this path names, or NULL.
// Where there is one, the file's basename and the folder's name are the same
// string, which is what lets a module's name be read off its path by a tool that
// cannot parse Cone
Name *parseDesignatedFolder(char *path) {
    size_t folderlen = fileFolder(path);
    char *foldername;
    if (folderlen > 1)
        foldername = fileName(memAllocStr(path, folderlen - 1));
    else if (folderlen == 0)
        // No folder in front of the file, so it sits in the current directory and
        // that is the folder whose name to read. A file's module may not depend on
        // the spelling of the path used to reach it
        foldername = fileCurFolderName();
    else
        foldername = NULL;    // a path rooted at the filesystem's own root
    if (foldername == NULL || strcmp(foldername, fileName(path)) != 0)
        return NULL;
    return nametblFind(foldername, strlen(foldername));
}

// Collect the paths of every '.cone' file beneath a folder: the folder's own
// files first, then each subfolder's, at any depth
void parseCollectFolder(FileNames *files, char *folder, char *designated) {
    FileNames cones, folders;
    if (!fileFolderScan(folder, &cones, &folders))
        return;    // a folder that cannot be read contributes no files
    for (uint32_t i = 0; i < cones.count; ++i) {
        char *path = parsePathJoin(folder, cones.names[i]);
        if (strcmp(path, designated) != 0)
            fileNamesAdd(files, path);
    }
    for (uint32_t i = 0; i < folders.count; ++i) {
        // A subfolder holding its own designated file is a SUBMODULE, and this
        // is the one branch the walk does not take yet: it would probe here for
        // '<sub>/<sub>.cone' and recurse into it as a module of its own instead
        // of absorbing the subfolder's files. Until that is built every
        // subfolder is organisational, so its files, at any depth, belong to the
        // enclosing module -- which is what lets a forty-file module group its
        // files by topic without minting namespaces for them
        char *subfolder = parsePathJoin(parsePathJoin(folder, folders.names[i]), "/");
        parseCollectFolder(files, subfolder, designated);
    }
}

// Every file of the module a designated file draws: the designated file first,
// then the rest of its folder's tree. A file that is nobody's designated file is
// a module of one file
void parseModuleFiles(FileNames *files, char *path, int designated) {
    fileNamesInit(files);
    fileNamesAdd(files, path);
    if (designated)
        parseCollectFolder(files, memAllocStr(path, fileFolder(path)), path);
}

// Register a module's files, so that each is read once and belongs to this
// module alone, and diagnose what cannot join it. Both diagnostics name full
// paths, because the paths are the only thing that tells the files apart.
//
// A path that cannot join is dropped from the list rather than parsed, and the
// module goes on with the rest of its files
void parseRegisterModuleFiles(ParseState *parse, ModuleNode *mod, FileNames *files) {
    for (uint32_t i = 0; i < files->count; ++i) {
        char *path = files->names[i];
        Name *pathsym = nametblFind(path, strlen(path));
        ModuleNode *owner = pgmFindFile(parse->pgm, pathsym);
        if (owner) {
            errorMsg(ErrorModFile,
                "Source file %s already belongs to module %s, and a file belongs to one module.",
                path, &owner->namesym->namestr);
            files->names[i] = NULL;
            continue;
        }
        // Two files of one module sharing a basename leave neither nameable: a
        // basename is what 'include' spells and what a diagnostic reports
        // against, so one of the two has to be renamed
        for (uint32_t j = 0; j < i; ++j) {
            if (files->names[j] == NULL || strcmp(fileName(files->names[j]), fileName(path)) != 0)
                continue;
            errorMsg(ErrorDupFile,
                "Module %s holds two files named %s: %s and %s. A module's files are named by their basenames, so the two cannot be told apart.",
                &mod->namesym->namestr, fileName(path), files->names[j], path);
            files->names[i] = NULL;
            break;
        }
        if (files->names[i])
            pgmSetFile(parse->pgm, pathsym, mod);
    }
}

// Parse source filename/path as identifier or string literal
char *parseFilename() {
    char *filename;
    switch (lex->toktype) {
    case IdentToken:
        filename = &lex->val.ident->namestr;
        lexNextToken();
        break;
    case StringLitToken:
        filename = lex->val.strlit;
        lexNextToken();
        break;
    default:
        errorExit(ExitNF, "Invalid source file; expected identifier or string");
        filename = NULL;
    }
    return filename;
}

// Parse include statement
void parseInclude(ParseState *parse) {
    // Obtain filename of source file we want to include
    char *filename;
    lexNextToken();
    filename = parseFilename();
    parseEndOfStatement();

    // Locate the file, then ask the registry for it: a file belongs to one
    // module, so a file this module's folder already swept in, or that another
    // module holds, cannot be injected into this one as well
    char *path = fileFindSrc(lex ? lex->url : NULL, filename);
    if (path == NULL)
        errorExit(ExitNF, "Cannot find or read source file %s", filename);
    Name *pathsym = nametblFind(path, strlen(path));
    ModuleNode *owner = pgmFindFile(parse->pgm, pathsym);
    if (owner) {
        // Reported after the statement's ';' rather than at the token the parse
        // has reached, which is the next declaration: the include is what is
        // wrong, and the lexer has already moved past it
        errorMsgLexAfter(ErrorModFile,
            "Source file %s already belongs to module %s, and a file belongs to one module.",
            path, &owner->namesym->namestr);
        return;
    }
    pgmSetFile(parse->pgm, pathsym, parse->mod);

    // Inject source of include file, parse its global statements, then pop lexer.
    // An included file never starts a module -- its declarations join the
    // including one -- so a 'mod' declaration in it has nothing to name
    lexInjectPath(path);
    parseGlobalStmts(parse, parse->mod, 0);
    if (lex->toktype != EofToken) {
        errorMsgLex(ErrorNoEof, "Expected end-of-file");
    }
    lexPop();
}

// Parse import statement
ImportNode *parseImport(ParseState *parse) {
    // Create import node
    ImportNode *importnode = newImportNode();
    lexNextToken();

    // Parse name of imported module
    char *filename = parseFilename();
    char *modstr = fileName(filename);

    // Process name folding instructions
    if (lexIsToken(DotToken)) {
        lexNextToken();
        if (lexIsToken(StarToken)) {
            importnode->foldall = 1;
            lexNextToken();
        }
        else
            errorMsgLex(ErrorBadTerm, "Expected '*' after '.': selective import is not supported yet.");
    }
    parseEndOfStatement();

    // Parse the imported modules
    Name *filesym = nametblFind(modstr, strlen(modstr));
    ModuleNode *newmod = parseLoadAndParseModuleFile(parse, filename, filesym);

    // A file of this module's own folder is already part of this module, so
    // naming it here asks the module to import itself. The folder is what brings
    // a sibling file in; 'import' reaches a different module
    if (newmod == parse->mod) {
        // After the statement's ';', for parseInclude's reason: the parse has
        // already moved on to the next declaration
        errorMsgLexAfter(ErrorModFile,
            "This file is already part of module %s: a file of the module's folder joins it without being imported.",
            &newmod->namesym->namestr);
        return NULL;
    }

    // Add imported module to namespace of existing module, under the name the
    // module declares for itself. That is the filename-derived one until the
    // file carries a 'mod' declaration: the declaration wins, and the file is
    // only where the module was found
    modAddNamedNode(parse->mod, newmod->namesym, (INode*)newmod);
    importnode->module = newmod;

    return importnode;
}

// Parse function or variable, as it may be preceded by a qualifier
// Return NULL if not either
void parseFnOrVar(ParseState *parse, uint16_t flags) {

    if (lexIsToken(FnToken)) {
        FnDclNode *node = (FnDclNode*)parseFn(parse, (flags&FlagExtern)? (ParseMayName | ParseMaySig) : (ParseMayName | ParseMayImpl));
        node->flags |= flags;
        modAddFn(parse->mod, node);
        return;
    }

    // A global variable declaration, if it begins with a permission
    else if lexIsToken(PermToken) {
        // A module's global may carry a fold clause: it is the one-instance
        // analogue of a field, so 'config Config use *' admits Config's members
        // as names of this module, reached through 'config'. An 'extern' global
        // is supplied from elsewhere and has no clause to write, since there is
        // no declaration here for the fold to read.
        VarDclNode *node = parseVarDcl(parse, immPerm,
            (flags&FlagExtern) ? ParseMaySig : ParseMayImpl | ParseMaySig | ParseMayFold);
        node->flags |= flags;
        node->flowtempflags |= VarInitialized;   // Globals always hold a valid value
        parseEndOfStatement();
        modAddNode(parse->mod, node->namesym, (INode*)node);
    }
    else {
        errorMsgLex(ErrorBadGloStmt, "Expected function or variable declaration");
        parseSkipToNextStmt();
        return;
    }
}

// Parse a global area statement (within a module)
// modAddNode adds node to module, as needed, including error message for dupes
// Consume a 'pub' that precedes a declaration, returning the flag it sets.
// The flag is on the node before the node joins its namespace, since that is
// when dclInfoJoin reads it.
uint16_t parsePub() {
    if (!lexIsToken(PubToken))
        return 0;
    lexNextToken();
    return FlagPub;
}

// Consume a 'static' that precedes a declaration, returning the flag it sets.
// 'static' means one copy shared by every instance of the enclosing thing, so
// it applies to a variable and to nothing else; the caller refuses the rest.
uint16_t parseStatic() {
    if (!lexIsToken(StaticToken))
        return 0;
    lexNextToken();
    return FlagStatic;
}

// Report 'static' on a declaration that has no per-instance copies to share
void parseBadStatic(uint16_t staticflag) {
    if (staticflag)
        errorMsgLex(ErrorBadStatic, "'static' applies to a variable, which has a copy per instance to share; this declaration has none");
}

// Skip a declaration's body whole, counting depth, so nothing inside it is read
// as a global statement and reported a second time. Used where a declaration's
// shape is admitted and its semantics are not built
void parseSkipDclBody() {
    if (!lexIsToken(LCurlyToken)) {
        parseSkipToNextStmt();
        return;
    }
    uint32_t depth = 0;
    do {
        if (lexIsToken(LCurlyToken))
            ++depth;
        else if (lexIsToken(RCurlyToken))
            --depth;
        lexNextToken();
    } while (depth > 0 && !lexIsToken(EofToken));
}

// Parse a 'mod' declaration, which declares the module a folder's files belong
// to.
//
// Only the header form is built: 'mod name;' as the first statement of the
// module's designated file. What NAMES the module is its folder, which is a
// filesystem fact a tool that cannot parse Cone can read off a path; a name
// written here is checked against the folder's and may not replace it. A module
// named after its file rather than its folder -- a lone file, which is today's
// program -- has no folder name to check against, so its declaration still
// renames it, and that is transitional.
//
// The declaration also binds the module's name into the module's own namespace
// -- a module is the registry its own contents resolve against, and it publishes
// itself into it -- except where the folder already did so at load, which is
// what makes a module-level name a local or a type member hides reachable again,
// as 'name.x'.
//
// Two shapes the grammar admits are refused because nothing is behind them: a
// nested 'mod name { ... }' block, which needs a namespace of its own and paths
// through it, and 'mod trait', a module's abstraction. Reporting each where it
// is written is what settles its spelling without accepting it.
//
// 'atmodstart' is whether this is the first statement of the module's designated
// file. The declaration claims the module, so nothing may precede it, a second
// one has nothing left to declare, and a file the folder swept in -- or an
// included one, whose declarations join the including module -- carries none at
// all.
void parseModuleDcl(ModuleNode *mod, int atmodstart) {
    lexNextToken();

    // 'mod trait' is a module's abstraction: the spelling is settled by 'trait'
    // being a modifier on the kind, and there is nothing behind it
    if (lexIsToken(TraitToken)) {
        errorMsgLex(ErrorUnbuiltKind,
            "'mod trait' names a module's abstraction, which the compiler does not build yet.");
        lexNextToken();
        if (lexIsToken(IdentToken))
            lexNextToken();
        parseSkipDclBody();
        return;
    }

    Name *modname = NULL;
    if (lexIsToken(IdentToken)) {
        modname = lex->val.ident;
        lexNextToken();
    }
    else
        errorMsgLex(ErrorNoName, "Expected a name for the module this file declares");

    // A nested module. Its namespace, the hook push and pop its parse needs, and
    // the paths reaching through it are all unbuilt
    if (lexIsToken(LCurlyToken) || lexIsToken(ColonToken)) {
        errorMsgLex(ErrorUnbuiltKind,
            "A nested 'mod' block is not built yet. 'mod name;' names the module of the whole file.");
        parseSkipDclBody();
        return;
    }
    // Reported before the statement's ';' is consumed, so that the diagnostic
    // lands on this declaration rather than on the token that follows it
    if (modname != NULL) {
        if (!atmodstart || (mod->flags & FlagModDcl))
            errorMsgLex(ErrorModDcl,
                "A 'mod' declaration must be its module's designated file's first statement, and a module declares itself once. A file the folder swept in declares nothing.");
        else {
            mod->flags |= FlagModDcl;
            if (mod->foldersym != NULL) {
                // The folder names the module and has bound that name already
                if (modname != mod->foldersym)
                    errorMsgLex(ErrorModName,
                        "This module is named for its folder, '%s'. A 'mod' declaration may restate that name; it may not change it.",
                        &mod->foldersym->namestr);
            }
            else {
                // A module that is one file is still named after that file,
                // which is transitional, so its declaration may rename it
                mod->namesym = modname;
                modAddNamedNode(mod, modname, (INode*)mod);
            }
        }
    }
    parseEndOfStatement();
}

void parseGlobalStmts(ParseState *parse, ModuleNode *mod, int atmodstart) {
    // Create and populate a Module node for the program
    while (lex->toktype!=EofToken && !parseBlockEnd()) {
        int atstart = atmodstart;
        atmodstart = 0;
        uint16_t pubflag = parsePub();
        // At module scale a static is shared across every instantiation of the
        // module. An ordinary module is instantiated once, so today it is a
        // global like any other; the flag is recorded for the generic module
        // that will make the distinction real.
        uint16_t staticflag = parseStatic();
        if (staticflag && !lexIsToken(PermToken) && !lexIsToken(FnToken))
            parseBadStatic(staticflag);
        switch (lex->toktype) {

        // Re-export is the module work's to define: 'pub' has no meaning here yet
        case IncludeToken:
        case ImportToken:
            if (pubflag)
                errorMsgLex(ErrorBadPub, "'pub' may not precede include or import");
            if (lexIsToken(IncludeToken))
                parseInclude(parse);
            else {
                ImportNode *newnode = parseImport(parse);
                if (newnode)
                    modAddNode(mod, NULL, (INode*)newnode);
            }
            break;

        // 'typedef' declares an alias: the same binding record a fold makes,
        // with a type expression as its target. 'pub' is its own bit on that
        // binding, as it is on any declaration.
        case TypedefToken: {
            AliasDclNode *newnode = parseTypedef(parse);
            if (newnode == NULL)
                break;
            newnode->flags |= pubflag;
            modAddNode(mod, newnode->namesym, (INode*)newnode);
            break;
        }

        // 'struct'-style type definition, optionally modified by 'trait'
        case StructToken: {
            INode *node = parseStruct(parse, pubflag);
            modAddNode(mod, inodeGetName(node), node);
            break;
        }

        // 'trait' by itself is a synonym for 'struct trait': one node, one flag,
        // and the struct family by default
        case TraitToken: {
            INode *node = parseStruct(parse, TraitType | pubflag);
            modAddNode(mod, inodeGetName(node), node);
            break;
        }

        // 'mod' names the module this file belongs to. 'pub' would say that the
        // module is visible outside a parent it does not have yet, and a module
        // has no instances for a 'static' to be shared across
        case ModToken:
            if (pubflag)
                errorMsgLex(ErrorBadPub, "'pub' may not precede a module declaration");
            parseBadStatic(staticflag);
            parseModuleDcl(mod, atstart);
            break;

        // 'actor' is a kind the grammar admits and the compiler does not build.
        // Naming it here is what makes 'trait' a modifier on the kind rather
        // than a keyword of its own: the abstraction of each kind is that kind's
        // keyword followed by 'trait'. There is nothing behind it yet, so the
        // declaration is reported and its body skipped rather than accepted with
        // no semantics under it.
        case ActorToken: {
            errorMsgLex(ErrorUnbuiltKind,
                "'actor' names a kind the compiler does not build yet. Its abstraction is spelled 'actor trait'.");
            lexNextToken();
            if (lexIsToken(TraitToken))
                lexNextToken();
            if (lexIsToken(IdentToken))
                lexNextToken();
            parseSkipDclBody();
            break;
        }

        // 'enum' type definition: the closed family, in both size varieties.
        // Every variant is padded out to the size of the largest unless the
        // declaration writes '@unsized', so SameSize is the default that
        // attribute clears.
        case EnumToken: {
            INode *node = parseStruct(parse, TraitType | SameSize | EnumType | pubflag);
            modAddNode(mod, inodeGetName(node), node);
            break;
        }

        // 'macro'
        case MacroToken: {
            MacroDclNode *macro = parseMacro(parse);
            macro->flags |= pubflag;
            modAddNode(mod, macro->namesym, (INode*)macro);
            break;
        }

        // 'extern' qualifier in front of fn or var (block). A 'pub' before
        // 'extern' reaches every declaration in the block; one inside it
        // reaches that declaration alone.
        case ExternToken:
        {
            lexNextToken();
            uint16_t extflag = FlagExtern | pubflag;
            if (lexIsToken(IdentToken)) {
                if (strcmp(&lex->val.ident->namestr, "system")==0)
                    extflag |= FlagSystem;
                lexNextToken();
            }
            if (lexIsToken(ColonToken) || lexIsToken(LCurlyToken)) {
                parseBlockStart();
                while (!parseBlockEnd()) {
                    uint16_t itemflag = extflag | parsePub();
                    if (lexIsToken(FnToken) || lexIsToken(PermToken))
                        parseFnOrVar(parse, itemflag);
                    else {
                        errorMsgLex(ErrorNoSemi, "Extern expects only functions and variables");
                        parseSkipToNextStmt();
                    }
                }
            }
            else
                parseFnOrVar(parse, extflag);
        }
            break;

        // Function or variable
        case FnToken:
            parseBadStatic(staticflag);
            parseFnOrVar(parse, pubflag);
            break;
        case PermToken:
            parseFnOrVar(parse, pubflag | staticflag);
            break;

        // Named const declaration
        case ConstToken: {
            ConstDclNode *constnode = parseConstDcl(parse);
            constnode->flags |= pubflag;
            modAddNode(parse->mod, constnode->namesym, (INode*)constnode);
            break;
        }

        default:
            errorMsgLex(ErrorBadGloStmt, "Invalid global area statement");
            lexNextToken();
            parseSkipToNextStmt();
            break;
        }
    }
}

// Parse every file of a module, in the order the sweep collected them: the
// designated file first, since it is the only one that may declare the module,
// and then each file the folder brought in. A file dropped by registration --
// one another module holds, or one whose basename collides -- is skipped
void parseModuleFilesParse(ParseState *parse, ModuleNode *mod, FileNames *files) {
    for (uint32_t i = 0; i < files->count; ++i) {
        if (files->names[i] == NULL)
            continue;
        lexInjectPath(files->names[i]);
        parseGlobalStmts(parse, mod, i == 0);
        if (lex->toktype != EofToken) {
            errorMsgLex(ErrorNoEof, "Expected end-of-file");
        }
        lexPop();
    }
}

// Load the module a name reaches, unless a module holds its file already, then
// fully parse it. Three steps: locate the file, register it and every other file
// its folder sweeps in, and parse each of them into the module.
//
// The de-dup key is the file's PATH, because what must happen exactly once is
// reading the file; neither the filename nor a 'mod' declaration's name decides
// it, and either may be shared by files in different folders
ModuleNode *parseLoadAndParseModuleFile(ParseState *parse, char *filename, Name *filesym) {
    // LOCATE. A built-in module is a string inside the compiler rather than a
    // file, and stands in the registry under the pseudo-file name its
    // diagnostics are reported against
    int builtin = filesym == corelibName || strcmp(filename, "stdio") == 0;
    char *path;
    if (builtin)
        path = filesym == corelibName ? "corelib" : "stdio";
    else {
        path = fileFindSrc(lex ? lex->url : NULL, filename);
        if (path == NULL)
            errorExit(ExitNF, "Cannot find or read source file %s", filename);
    }
    Name *pathsym = nametblFind(path, strlen(path));

    // REGISTER. If a module holds this file already, that module is what the
    // name reaches: the file is not read a second time
    ModuleNode *mod = pgmFindFile(parse->pgm, pathsym);
    if (mod)
        return mod;

    // Create and add this new module to list of modules, and make it the current one
    ModuleNode *svmod = parse->mod;
    mod = pgmAddMod(parse->pgm, filesym==corelibName || strcmp(filename, "stdio")? 0 : FlagGenMod);
    mod->filesym = filesym;
    // The module's name is a filesystem fact: its folder's, where a designated
    // file drew the module out of a folder, and its file's otherwise. Filename
    // naming is transitional and is what a designated file replaces
    mod->foldersym = builtin ? NULL : parseDesignatedFolder(path);
    mod->namesym = mod->foldersym ? mod->foldersym : filesym;
    // Every loaded module names itself in the owner chain; only the root does not
    dclInfoJoin((INode*)mod, NULL);
    mod->dclinfo.facts |= DclNamesChain;
    parse->mod = mod;

    // The module's files, all registered before any of them is parsed, so that
    // which files the module holds does not depend on what the parse of one of
    // them imports
    FileNames files;
    parseModuleFiles(&files, path, mod->foldersym != NULL);
    if (builtin)
        pgmSetFile(parse->pgm, pathsym, mod);
    else
        parseRegisterModuleFiles(parse, mod, &files);

    // Before parsing, all modules (except corelib) get an auto-import of core lib
    ModuleNode *corelib = pgmFindFile(parse->pgm, corelibName);
    if (corelib && corelib != mod) {
        ImportNode *importnode = newImportNode();
        importnode->foldall = 1;
        importnode->module = corelib;
        modAddNode(mod, NULL, (INode*)importnode);
    }

    // Parse the module's source, then pop lexer and name hook
    modHook(svmod, mod);
    // A module folder's name is in reach inside the module whether or not a
    // declaration restates it, since the folder is what names it
    if (mod->foldersym)
        modAddNamedNode(mod, mod->namesym, (INode*)mod);
    if (builtin) {
        lexInject(filesym == corelibName ? corelibSource : stdiolib, path);
        parseGlobalStmts(parse, mod, 1);
        if (lex->toktype != EofToken) {
            errorMsgLex(ErrorNoEof, "Expected end-of-file");
        }
        lexPop();
    }
    else
        parseModuleFilesParse(parse, mod, &files);
    modHook(mod, svmod);

    // Restore focus to original module we were working on
    parse->mod = svmod;
    return mod;
}

// Parse a program = the main module
ProgramNode *parsePgm(ConeOptions *opt) {
    // Initialize name table and lexer
    nametblInit();
    typetblInit();
    lexInit(opt);
    stdlibInit(opt->ptrsize);

    ProgramNode *pgm = newProgramNode();

    // Initialize parser state
    ParseState parse;
    parse.pgm = pgm;
    parse.mod = NULL;
    parse.typenode = NULL;
    parse.inrettype = 0;

    // Create module node and set up for parsing main source file.
    // The root's file is registered like any other, so an import cycle back to
    // it finds the module already parsed instead of reading the file again as a
    // second module. It sets no DclNamesChain: the root contributes no prefix, so
    // its declarations are spelled bare -- and naming the root module changes
    // what it is called, never how the program's symbols are spelled.
    ModuleNode *mod = pgmAddMod(pgm, FlagGenMod);
    mod->filesym = nametblFind(opt->srcname, strlen(opt->srcname));

    // The program is one file, or a folder's worth of them: the file the
    // compiler was pointed at sweeps its folder when it is that folder's
    // designated file, and is a module of one file otherwise
    char *path = fileFindSrc(lex ? lex->url : NULL, opt->srcpath);
    if (path == NULL)
        errorExit(ExitNF, "Cannot find or read source file %s", opt->srcpath);
    mod->foldersym = parseDesignatedFolder(path);
    mod->namesym = mod->foldersym ? mod->foldersym : mod->filesym;
    FileNames files;
    parseModuleFiles(&files, path, mod->foldersym != NULL);
    parseRegisterModuleFiles(&parse, mod, &files);

    // Inject and parse core library module, auto-imported into main source
    ModuleNode *corelib = parseLoadAndParseModuleFile(&parse, "", corelibName);
    ImportNode *importnode = newImportNode();
    importnode->foldall = 1;
    importnode->module = corelib;
    modAddNode(mod, NULL, (INode*)importnode);

    // Now actually parse the main module's files
    parse.mod = mod;
    modHook(NULL, mod);
    if (mod->foldersym)
        modAddNamedNode(mod, mod->namesym, (INode*)mod);
    // A stray '}' at global scope ends a file's statement loop. Without the
    // end-of-file check inside, the rest of that file would be silently
    // discarded, exactly as an include would be
    parseModuleFilesParse(&parse, mod, &files);
    modHook(mod, NULL);
    return pgm;
}
