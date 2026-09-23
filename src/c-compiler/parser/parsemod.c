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

    // Inject source of include file, parse its global statements, then pop lexer.
    // An included file never starts a module -- its declarations join the
    // including one -- so a 'mod' declaration in it has nothing to name
    lexInjectFile(filename);
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

// Parse a 'mod' declaration, which names the module a source file belongs to.
//
// Only the header form is built: 'mod name;' as a source file's first
// statement. It names the module the file was loaded as, so that a module's
// identity comes from its declaration rather than from its filename, and it
// binds that name into the module's own namespace -- a module is the registry
// its own contents resolve against, and it publishes itself into it. That
// binding is what makes a module-level name a local or a type member hides
// reachable again, as 'name.x'.
//
// Two shapes the grammar admits are refused because nothing is behind them: a
// nested 'mod name { ... }' block, which needs a namespace of its own and paths
// through it, and 'mod trait', a module's abstraction. Reporting each where it
// is written is what settles its spelling without accepting it.
//
// 'atmodstart' is whether this is the first statement of the module's own
// source. The header claims the whole file, so nothing may precede it, and an
// included file -- whose declarations join the including module -- may carry
// none at all.
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
                "A 'mod' declaration must be its source file's first statement, and a file declares one module.");
        else {
            // The declaration names the module, replacing what its filename gave it
            mod->flags |= FlagModDcl;
            mod->namesym = modname;
            modAddNamedNode(mod, modname, (INode*)mod);
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

// If we don't have it, load an imported module by its path/name, then fully parse it.
// The de-dup key is the filename-derived name, because what must happen once is
// reading the file; a 'mod' declaration inside may name the module anything
ModuleNode *parseLoadAndParseModuleFile(ParseState *parse, char *filename, Name *filesym) {
    // If we already have module, don't re-parse. Just return it.
    ModuleNode *mod = pgmFindModFile(parse->pgm, filesym);
    if (mod)
        return mod;

    // Create and add this new module to list of modules, and make it the current one
    ModuleNode *svmod = parse->mod;
    mod = pgmAddMod(parse->pgm, filesym==corelibName || strcmp(filename, "stdio")? 0 : FlagGenMod);
    mod->filesym = filesym;
    // The filename names the module until its own 'mod' declaration does.
    // Transitional: the folder walk replaces filename naming altogether
    mod->namesym = filesym;
    // Every loaded module names itself in the owner chain; only the root does not
    dclInfoJoin((INode*)mod, NULL);
    mod->dclinfo.facts |= DclNamesChain;
    parse->mod = mod;

    // Inject the module's source into the lexer
    if (filesym == corelibName)
        lexInject(corelibSource, "corelib");
    else if (strcmp(filename, "stdio") == 0)
        lexInject(stdiolib, "stdio");
    else
        lexInjectFile(filename);

    // Before parsing, all modules (except corelib) get an auto-import of core lib
    ModuleNode *corelib = pgmFindModFile(parse->pgm, corelibName);
    if (corelib && corelib != mod) {
        ImportNode *importnode = newImportNode();
        importnode->foldall = 1;
        importnode->module = corelib;
        modAddNode(mod, NULL, (INode*)importnode);
    }

    // Parse the imported module's source, then pop lexer and name hook
    modHook(svmod, mod);
    parseGlobalStmts(parse, mod, 1);
    if (lex->toktype != EofToken) {
        errorMsgLex(ErrorNoEof, "Expected end-of-file");
    }
    lexPop();
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
    // The root is named after its file, as an imported module is, so that an
    // import cycle back to this file finds it (pgmFindModFile) instead of reading
    // the file again as a second module. It sets no DclNamesChain: the root
    // contributes no prefix, so its declarations are spelled bare -- and a 'mod'
    // declaration in the root file changes what the module is called, never how
    // the program's symbols are spelled.
    ModuleNode *mod = pgmAddMod(pgm, FlagGenMod);
    mod->filesym = nametblFind(opt->srcname, strlen(opt->srcname));
    mod->namesym = mod->filesym;
    lexInjectFile(opt->srcpath);
    modHook(NULL, mod);

    // Inject and parse core library module, auto-imported into main source
    ModuleNode *corelib = parseLoadAndParseModuleFile(&parse, "", corelibName);
    ImportNode *importnode = newImportNode();
    importnode->foldall = 1;
    importnode->module = corelib;
    modAddNode(mod, NULL, (INode*)importnode);

    // Now actually parse main source file
    parse.mod = mod;
    modHook(NULL, mod);
    parseGlobalStmts(&parse, mod, 1);
    // A stray '}' at global scope ends the statement loop. Without this the rest
    // of the main file is silently discarded, exactly as an include would be --
    // parseInclude and parseLoadAndParseModuleFile already make the same check.
    if (lex->toktype != EofToken)
        errorMsgLex(ErrorNoEof, "Expected end-of-file");
    modHook(mod, NULL);
    return pgm;
}
