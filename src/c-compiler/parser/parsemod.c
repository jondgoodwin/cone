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

// Parse source filename/path as identifier or string literal
char *parseFile() {
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

void parseGlobalStmts(ParseState *parse, ModuleNode *mod);

// Parse include statement
void parseInclude(ParseState *parse) {
    char *filename;
    lexNextToken();
    filename = parseFile();
    parseEndOfStatement();

    lexInjectFile(filename);
    parseGlobalStmts(parse, parse->mod);
    if (lex->toktype != EofToken) {
        errorMsgLex(ErrorNoEof, "Expected end-of-file");
    }
    lexPop();
}

char *stdiolib =
"extern {fn printStr(str &[]u8); fn printCStr(str *u8); fn printFloat(a f64); fn printInt(a i64); fn printUInt(a u64); fn printChar(code u64);}\n"
"struct IOStream{"
"  fd i32;"
"  fn `<-`(self &mut, str &[]u8) {printStr(str)}"
"  fn `<-`(self &mut, str *u8) {printCStr(str)}"
"  fn `<-`(self &mut, i i64) {printInt(i)}"
"  fn `<-`(self &mut, n f64) {printFloat(n)}"
"  fn `<-`(self &mut, i u64) {printUInt(i)}"
"}"
"mut print = IOStream[0]"
;

// Parse imported module
ModuleNode *parseImportModule(ParseState *parse, char *filename, Name *modname) {
    // If we already have module, don't re-parse. Just return it.
    ModuleNode *newmod = pgmFindMod(parse->pgm, modname);
    if (newmod)
        return newmod;

    // Let's load and parse the module
    char *svprefix = parse->gennamePrefix;
    ModuleNode *svmod = parse->mod;
    nameNewPrefix(&parse->gennamePrefix, &modname->namestr);

    if (modname == corelibName)
        lexInject(corelibSource, "corelib");
    else if (strcmp(filename, "stdio") == 0)
        lexInject(stdiolib, "stdio");
    else
        lexInjectFile(filename);
    newmod = pgmAddMod(parse->pgm);
    newmod->namesym = modname;
    parse->mod = newmod;

    // Auto-import core lib (except into corelib)
    ModuleNode *corelib = pgmFindMod(parse->pgm, corelibName);
    if (corelib && corelib != newmod) {
        ImportNode *importnode = newImportNode();
        importnode->foldall = 1;
        importnode->module = corelib;
        modAddNode(newmod, NULL, (INode*)importnode);
    }

    modHook(svmod, newmod);
    parseGlobalStmts(parse, newmod);
    if (lex->toktype != EofToken) {
        errorMsgLex(ErrorNoEof, "Expected end-of-file");
    }
    lexPop();
    modHook(newmod, svmod);

    parse->mod = svmod;
    parse->gennamePrefix = svprefix;
    return newmod;
}

// Parse import statement
ImportNode *parseImport(ParseState *parse) {
    ImportNode *importnode = newImportNode();
    lexNextToken();
    char *filename = parseFile();
    char *modstr = fileName(filename);
    Name *modname = nametblFind(modstr, strlen(modstr));

    if (lexIsToken(DblColonToken)) {
        lexNextToken();
        if (lexIsToken(StarToken)) {
            importnode->foldall = 1;
            lexNextToken();
        }
    }
    parseEndOfStatement();

    // Parse the imported modules
    ModuleNode *newmod = parseImportModule(parse, filename, modname);

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
        nameGenVarName((VarDclNode *)node, parse->gennamePrefix);
        modAddNode(parse->mod, node->namesym, (INode*)node);
        return;
    }

    // A global variable declaration, if it begins with a permission
    else if lexIsToken(PermToken) {
        VarDclNode *node = parseVarDcl(parse, immPerm, ParseMayConst | ((flags&FlagExtern) ? ParseMaySig : ParseMayImpl | ParseMaySig));
        node->flags |= flags;
        node->flowtempflags |= VarInitialized;   // Globals always hold a valid value
        parseEndOfStatement();
        nameGenVarName((VarDclNode *)node, parse->gennamePrefix);
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
void parseGlobalStmts(ParseState *parse, ModuleNode *mod) {
    // Create and populate a Module node for the program
    while (lex->toktype!=EofToken && !parseBlockEnd()) {
        lexStmtStart();
        switch (lex->toktype) {

        case IncludeToken:
            parseInclude(parse);
            break;

        case ImportToken: {
            ImportNode *newnode = parseImport(parse);
            modAddNode(mod, NULL, (INode*)newnode);
            break;
        }

        case TypedefToken: {
            TypedefNode *newnode = parseTypedef(parse);
            modAddNode(mod, newnode->namesym, (INode*)newnode);
            break;
        }

        // 'struct'-style type definition
        case StructToken: {
            INode *node = parseStruct(parse, 0);
            modAddNode(mod, inodeGetName(node), node);
            break;
        }

        // 'trait' type definition
        case TraitToken: {
            INode *node = parseStruct(parse, TraitType);
            modAddNode(mod, inodeGetName(node), node);
            break;
        }

        // 'union' type definition
        case UnionToken: {
            INode *node = parseStruct(parse, TraitType | SameSize);
            modAddNode(mod, inodeGetName(node), node);
            break;
        }

        // 'macro'
        case MacroToken: {
            MacroDclNode *macro = parseMacro(parse);
            modAddNode(mod, macro->namesym, (INode*)macro);
            break;
        }

        // 'extern' qualifier in front of fn or var (block)
        case ExternToken:
        {
            lexNextToken();
            uint16_t extflag = FlagExtern;
            if (lexIsToken(IdentToken)) {
                if (strcmp(&lex->val.ident->namestr, "system")==0)
                    extflag |= FlagSystem;
                lexNextToken();
            }
            if (lexIsToken(ColonToken) || lexIsToken(LCurlyToken)) {
                parseBlockStart();
                while (!parseBlockEnd()) {
                    lexStmtStart();
                    if (lexIsToken(FnToken) || lexIsToken(PermToken))
                        parseFnOrVar(parse, extflag);
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
        case PermToken:
            parseFnOrVar(parse, 0);
            break;

        // Named const declaration
        case ConstToken: {
            ConstDclNode *constnode = parseConstDcl(parse);
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

// Parse a module's global statement block
ModuleNode *parseModuleBlk(ParseState *parse, ModuleNode *mod) {
    ModuleNode *oldmod = parse->mod;
    parse->mod = mod;
    modHook(oldmod, mod);
    parseGlobalStmts(parse, mod);
    modHook(mod, oldmod);
    parse->mod = oldmod;
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
    parse.gennamePrefix = "";

    // Create module node and set up for parsing main source file
    ModuleNode *pgmmod = pgmAddMod(pgm);
    parse.pgmmod = pgmmod;
    lexInjectFile(opt->srcpath);
    modHook(NULL, pgmmod);

    // Inject and parse core libary module, auto-imported into main source
    ModuleNode *corelib = parseImportModule(&parse, "", corelibName);
    ImportNode *importnode = newImportNode();
    importnode->foldall = 1;
    importnode->module = corelib;
    modAddNode(pgmmod, NULL, (INode*)importnode);

    // Now actually parse main source file
    parseModuleBlk(&parse, pgmmod);
    modHook(pgmmod, NULL);
    return pgm;
}
