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

void parseGlobalStmts(ParseState *parse, ModuleNode *mod);
ModuleNode *parseLoadAndParseModuleFile(ParseState *parse, char *filename, Name *modname);

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

    // Inject source of include file, parse its global statements, then pop lexer
    lexInjectFile(filename);
    parseGlobalStmts(parse, parse->mod);
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
    Name *modname = nametblFind(modstr, strlen(modstr));
    ModuleNode *newmod = parseLoadAndParseModuleFile(parse, filename, modname);

    // Add imported module to namespace of existing module
    modAddNamedNode(parse->mod, modname, (INode*)newmod);
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
        VarDclNode *node = parseVarDcl(parse, immPerm, (flags&FlagExtern) ? ParseMaySig : ParseMayImpl | ParseMaySig);
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

void parseGlobalStmts(ParseState *parse, ModuleNode *mod) {
    // Create and populate a Module node for the program
    while (lex->toktype!=EofToken && !parseBlockEnd()) {
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

        case TypedefToken: {
            TypedefNode *newnode = parseTypedef(parse);
            newnode->flags |= pubflag;
            modAddNode(mod, newnode->namesym, (INode*)newnode);
            break;
        }

        // 'struct'-style type definition
        case StructToken: {
            INode *node = parseStruct(parse, pubflag);
            modAddNode(mod, inodeGetName(node), node);
            break;
        }

        // 'trait' type definition: the open abstraction
        case TraitToken: {
            INode *node = parseStruct(parse, TraitType | pubflag);
            modAddNode(mod, inodeGetName(node), node);
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

// If we don't have it, load an imported module by its path/name, then fully parse it
ModuleNode *parseLoadAndParseModuleFile(ParseState *parse, char *filename, Name *modname) {
    // If we already have module, don't re-parse. Just return it.
    ModuleNode *mod = pgmFindMod(parse->pgm, modname);
    if (mod)
        return mod;

    // Create and add this new module to list of modules, and make it the current one
    ModuleNode *svmod = parse->mod;
    mod = pgmAddMod(parse->pgm, modname==corelibName || strcmp(filename, "stdio")? 0 : FlagGenMod);
    mod->namesym = modname;
    // Every loaded module names itself in the owner chain; only the root does not
    dclInfoJoin((INode*)mod, NULL);
    mod->dclinfo.facts |= DclNamesChain;
    parse->mod = mod;

    // Inject the module's source into the lexer
    if (modname == corelibName)
        lexInject(corelibSource, "corelib");
    else if (strcmp(filename, "stdio") == 0)
        lexInject(stdiolib, "stdio");
    else
        lexInjectFile(filename);

    // Before parsing, all modules (except corelib) get an auto-import of core lib
    ModuleNode *corelib = pgmFindMod(parse->pgm, corelibName);
    if (corelib && corelib != mod) {
        ImportNode *importnode = newImportNode();
        importnode->foldall = 1;
        importnode->module = corelib;
        modAddNode(mod, NULL, (INode*)importnode);
    }

    // Parse the imported module's source, then pop lexer and name hook
    modHook(svmod, mod);
    parseGlobalStmts(parse, mod);
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
    // import cycle back to this file finds it (pgmFindMod) instead of reading
    // the file again as a second module. It sets no DclNamesChain: the root
    // contributes no prefix, so its declarations are spelled bare.
    ModuleNode *mod = pgmAddMod(pgm, FlagGenMod);
    mod->namesym = nametblFind(opt->srcname, strlen(opt->srcname));
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
    parseGlobalStmts(&parse, mod);
    // A stray '}' at global scope ends the statement loop. Without this the rest
    // of the main file is silently discarded, exactly as an include would be --
    // parseInclude and parseLoadAndParseModuleFile already make the same check.
    if (lex->toktype != EofToken)
        errorMsgLex(ErrorNoEof, "Expected end-of-file");
    modHook(mod, NULL);
    return pgm;
}
