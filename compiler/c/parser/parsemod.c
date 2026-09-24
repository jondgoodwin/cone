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

void parseGlobalStmts(ParseState *parse, ModuleNode *mod, int atmodstart);
ModuleNode *parseLoadAndParseModuleFile(ParseState *parse, char *filename, Name *filesym);

// ---------------------------------------------------------------------------
// The file registry and the folder tree
//
// A module's source files are the files of a folder. The compiler is given one
// file and finds the rest itself: the folder's other '.cone' files join the
// module, and so do the files of every organisational subfolder, at any depth.
// No file set is ever hand-listed, and moving a file between folders is a
// semantic move.
//
// What makes a folder a module folder is the designated file it holds, named for
// the folder -- 'matrix/matrix.cone'. That convention is the whole of the
// trigger: the file the compiler is given sweeps its folder exactly when it is
// that folder's designated file, and the same probe decides every subfolder. A
// file that is not its folder's designated file is a module of its own, exactly
// today's program, and its neighbours are none of its business.
//
// So the folder tree carries the MODULE TREE: a subfolder holding its own
// designated file is a SUBMODULE of the enclosing module, with its own namespace,
// its own files and its own subfolders, and any other subfolder is
// ORGANISATIONAL, grouping the enclosing module's files without minting a
// namespace for them. A module folder must be a direct child of its parent
// module's folder, so the module tree's shape mirrors the folder tree's, and a
// designated file deeper than that is refused rather than drawing a module the
// shape could not hold.
//
// A folder is only worth its ceremony when a module has more than one file
// [Jon 23 Sep], so a submodule may also be ONE FILE: a file of the module's
// folder whose first statement is a 'mod' declaration is a submodule of its own,
// named for its file, in every respect the submodule 'lexer/lexer.cone' would be.
// It does not join the module its folder holds, and none of that module's files
// join it. Growing it into a folder -- moving 'lexer.cone' to
// 'lexer/lexer.cone' -- changes nothing for anyone who names it. What decides it
// is the file's first statement, read off its text before anything is parsed,
// so which files a module holds still does not depend on what any parse imports.
//
// A submodule is a module in every respect; what makes it a child is that its
// parent OWNS it -- which puts the parent's name in front of its declarations'
// symbols -- and that it is private to its parent unless its declaration says
// 'pub'. A parent reaches into it by path, 'sub.name', the ordinary path rule
// through a namespace.
//
// SIDEWAYS IS AN IMPORT, AND THE REGISTRY IT RESOLVES AGAINST IS THE PARENT'S
// NAMESPACE. A module is the registry for its children: they are public to each
// other and to it, invisible outside unless it publishes them. So 'import log'
// inside a submodule is a LOOKUP -- the sweep has already drawn the sister, and
// nothing is loaded -- and a module with no parent has no registry, only the
// filesystem, which is what reaches an external module today. The registry is
// SCOPED and not accumulating: it is the immediate parent's namespace and no
// ancestor's, so a module two deep does not see its parent's sisters and a
// parent re-exports what its children need.
//
// Locating a file, registering it to a module and parsing it into that module
// are three separate steps, because what must happen exactly once is the
// reading. The registry is keyed by the file's CANONICAL path, so a file is read
// once and belongs to one module however many importers name it and however they
// spell the way there -- and a path that walks sideways to a sister therefore
// finds her module rather than building a second one from her files. Finding her
// that way is refused (ErrorModReach): a neighbour is reached because the
// registry holds her name, never because a path arrived at her.
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

// The source files a sweep found, each with the block it was read into. The
// sweep reads every file it finds, because a file's first statement decides
// which module it is, and the block it read is the one later parsed, so each
// file is still read once. A block is NULL for a file not read yet: the
// designated file of a subfolder, which the module it draws reads
typedef struct SrcFiles {
    char **paths;
    Lexer **blocks;
    uint32_t count;
    uint32_t avail;
} SrcFiles;

static void parseSrcFilesInit(SrcFiles *list) {
    list->paths = NULL;
    list->blocks = NULL;
    list->count = 0;
    list->avail = 0;
}

static void parseSrcFilesAdd(SrcFiles *list, char *path, Lexer *block) {
    if (list->count == list->avail) {
        list->avail = list->avail ? list->avail * 2 : 8;
        char **paths = (char **)memAllocBlk(list->avail * sizeof(char *));
        Lexer **blocks = (Lexer **)memAllocBlk(list->avail * sizeof(Lexer *));
        for (uint32_t i = 0; i < list->count; ++i) {
            paths[i] = list->paths[i];
            blocks[i] = list->blocks[i];
        }
        list->paths = paths;
        list->blocks = blocks;
    }
    list->paths[list->count] = path;
    list->blocks[list->count++] = block;
}

// Collect a module's files from its folder tree, and the submodules it holds: the
// folder's own files first, then each organisational subfolder's, at any depth.
//
// Every subfolder gets the same probe the file the compiler was given got. One
// that holds its own designated file draws a module, so the sweep stops there and
// leaves that folder's files to it; any other is organisational and its files are
// this module's.
//
// Every file gets a probe too: one whose first statement is a 'mod' declaration
// is a ONE-FILE MODULE, a submodule exactly as a subfolder's would be, and joins
// 'submodules' rather than 'files'.
//
// 'organisational' says this folder is one of those, and it is what makes the
// direct-child rule a rule rather than a convention: a module beneath an
// organisational folder would be one whose parent module's folder is not where it
// sits, which the module tree has no shape for. A designated file there is
// refused, and the folder holding it stays organisational -- its files, that one
// among them, belong to the enclosing module, so the compile goes on with a
// coherent file set. A one-file module there is refused and left out, since the
// one thing it says is that it is not the enclosing module's
void parseCollectFolder(SrcFiles *files, SrcFiles *submodules, char *folder, char *designated, int organisational) {
    FileNames cones, folders;
    if (!fileFolderScan(folder, &cones, &folders))
        return;    // a folder that cannot be read contributes no files
    for (uint32_t i = 0; i < cones.count; ++i) {
        char *path = parsePathJoin(folder, cones.names[i]);
        if (strcmp(path, designated) == 0)
            continue;
        Lexer *block = lexLoadPath(path);
        if (!lexOpensWithMod(block->source)) {
            parseSrcFilesAdd(files, path, block);
            continue;
        }
        // A designated file here was refused already, as the folder above it was
        // probed, and is swept in like the rest of its folder
        if (organisational && parseDesignatedFolder(path) != NULL) {
            parseSrcFilesAdd(files, path, block);
            continue;
        }
        if (organisational) {
            errorMsg(ErrorModFolder,
                "Source file %s declares a module of its own, but a one-file module must sit directly in its parent module's folder. It sits in organisational folder %s, whose files are the enclosing module's: move it into the module's folder, or remove its 'mod' declaration to make it one of the enclosing module's files.",
                path, folder);
            continue;
        }
        // A file and a folder both drawing one name in one parent is two modules
        // of one name, and the fix is to keep one of them: the folder is what the
        // file grows into, so it is the one kept
        char *name = fileName(path);
        char *twin = fileDesignatedFile(parsePathJoin(parsePathJoin(folder, name), "/"), name);
        if (twin != NULL) {
            errorMsg(ErrorModFileFolder,
                "Source file %s declares module '%s', and so does module folder %s beside it. A module is one file or one folder: move the file's declarations into the folder, or remove one of them.",
                path, name, twin);
            continue;
        }
        parseSrcFilesAdd(submodules, path, block);
    }
    for (uint32_t i = 0; i < folders.count; ++i) {
        char *subfolder = parsePathJoin(parsePathJoin(folder, folders.names[i]), "/");
        char *subdesignated = fileDesignatedFile(subfolder, folders.names[i]);
        if (subdesignated != NULL && !organisational) {
            parseSrcFilesAdd(submodules, subdesignated, NULL);
            continue;
        }
        if (subdesignated != NULL)
            errorMsg(ErrorModFolder,
                "Source file %s is named for its folder and so draws a module, but a module folder must be a direct child of its parent module's folder. It sits beneath organisational folder %s, whose files are the enclosing module's.",
                subdesignated, folder);
        parseCollectFolder(files, submodules, subfolder, designated, 1);
    }
}

// Every file of the module a file draws: that file first, then, where it is its
// folder's designated file, the rest of its folder's tree, minus whatever its
// submodules hold. 'submodules' comes back with the file of each submodule: a
// subfolder's designated file, or a one-file module's one file. A file that is
// nobody's designated file is a module of one file, and sweeps nothing.
// 'block' is the file's own block, where it has been read already
void parseModuleFiles(SrcFiles *files, SrcFiles *submodules, char *path, Lexer *block, int designated) {
    parseSrcFilesInit(files);
    parseSrcFilesInit(submodules);
    parseSrcFilesAdd(files, path, block);
    if (!designated)
        return;
    parseCollectFolder(files, submodules, memAllocStr(path, fileFolder(path)), path, 0);

    // The submodules in the order of their names, whichever shape each has: the
    // sweep met the one-file ones among the folder's files and the others among
    // its subfolders, and growing a module from one shape into the other must not
    // move it in the order submodules are drawn, bound and emitted
    for (uint32_t i = 1; i < submodules->count; ++i) {
        char *subpath = submodules->paths[i];
        Lexer *subblock = submodules->blocks[i];
        uint32_t j = i;
        while (j > 0 && strcmp(fileName(submodules->paths[j - 1]), fileName(subpath)) > 0) {
            submodules->paths[j] = submodules->paths[j - 1];
            submodules->blocks[j] = submodules->blocks[j - 1];
            --j;
        }
        submodules->paths[j] = subpath;
        submodules->blocks[j] = subblock;
    }
}

// Register a module's files, so that each is read once and belongs to this
// module alone, and diagnose what cannot join it. Both diagnostics name full
// paths, because the paths are the only thing that tells the files apart.
//
// A path that cannot join is dropped from the list rather than parsed, and the
// module goes on with the rest of its files
void parseRegisterModuleFiles(ParseState *parse, ModuleNode *mod, SrcFiles *files) {
    for (uint32_t i = 0; i < files->count; ++i) {
        char *path = files->paths[i];
        Name *pathsym = nametblFind(path, strlen(path));
        ModuleNode *owner = pgmFindFile(parse->pgm, pathsym);
        if (owner) {
            errorMsg(ErrorModFile,
                "Source file %s already belongs to module %s, and a file belongs to one module.",
                path, &owner->namesym->namestr);
            files->paths[i] = NULL;
            continue;
        }
        // Two files of one module sharing a basename leave neither nameable: a
        // basename is what a diagnostic reports against, so one of the two has
        // to be renamed
        for (uint32_t j = 0; j < i; ++j) {
            if (files->paths[j] == NULL || strcmp(fileName(files->paths[j]), fileName(path)) != 0)
                continue;
            errorMsg(ErrorDupFile,
                "Module %s holds two files named %s: %s and %s. A module's files are named by their basenames, so the two cannot be told apart.",
                &mod->namesym->namestr, fileName(path), files->paths[j], path);
            files->paths[i] = NULL;
            break;
        }
        if (files->paths[i])
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

// 'include' is retired. A module's files are its folder's files, so a file joins
// a module by where it sits and nothing in any file brings another in -- which
// is what lets a tool handed one file tell from its path which module it is in.
//
// The word stays a keyword so that no program can bind it, and the statement it
// began is reported where it is written and skipped whole, whatever it names:
// one file, a path, or a list. What it named is not looked for, because nothing
// would be done with it.
//
// The skip reads what the statement took rather than scanning for a ';', so an
// 'include' with its ';' missing ends at its last name and does not swallow the
// declarations after it. Only where no name follows does it scan
static void parseRetiredInclude() {
    errorMsgLex(ErrorInclude,
        "'include' is retired: the folder brings a module's files in. A file in the same folder as the file named for that folder is part of its module.");
    lexNextToken();
    int named = 0;
    while (lexIsToken(IdentToken) || lexIsToken(StringLitToken)) {
        named = 1;
        lexNextToken();
        if (!lexIsToken(CommaToken))
            break;
        lexNextToken();
    }
    if (lexIsToken(SemiToken))
        lexNextToken();
    else if (!named)
        parseSkipToNextStmt();
}

// The module a name reaches in the REGISTRY this module's imports resolve
// against, or NULL where the registry holds no module under that name.
//
// A module is the registry for its children: they are public to each other and
// to it, and invisible outside unless it publishes them. So a module's registry
// is its PARENT's namespace -- its own sisters, and whatever its parent bound
// there -- and a module with no parent has no registry, only the outside world.
//
// It is SCOPED rather than accumulating: the registry is the immediate parent's
// and no ancestor's, so a module two deep does not see its parent's sisters and
// a parent that wants one of them reachable re-exports it. (Adopted
// provisionally; Ruminations\modules-design-brief.md decision 10 records that it
// is to be revisited.)
static ModuleNode *parseImportRegistry(ParseState *parse, Name *modname) {
    ModuleNode *parent = (ModuleNode*)parse->mod->dclinfo.owner;
    if (parent == NULL)
        return NULL;
    INode *found = aliasDclResolve(namespaceFind(&parent->namespace, modname));
    if (found == NULL || found->tag != ModuleTag)
        return NULL;
    return (ModuleNode*)found;
}

// The import of this module that this module already holds, or NULL
static ImportNode *parseImportPrior(ModuleNode *mod, ModuleNode *imported) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(mod->imports, cnt, nodesp)) {
        if (((ImportNode*)*nodesp)->module == imported)
            return (ImportNode*)*nodesp;
    }
    return NULL;
}

// Hold an import of a name of the parent, to be bound in the fold passes: an
// alias under the name, not yet pointing at anything (importBindName). Two
// imports of one name are refused here, as two imports of one module are
static ImportNode *parseImportName(ParseState *parse, ImportNode *importnode, Name *name) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(parse->mod->imports, cnt, nodesp)) {
        ImportNode *prior = (ImportNode*)*nodesp;
        if (prior->binding == NULL || prior->binding->namesym != name)
            continue;
        errorMsgNode((INode*)importnode, ErrorDupImport,
            "%s is imported already, at %s:%u. A module imports a name once: leave out the second.",
            &name->namestr, prior->lexer->url, prior->linenbr);
        return NULL;
    }
    NameUseNode *target = newNameUseNode(name);
    inodeLexCopy((INode*)target, (INode*)importnode);
    AliasDclNode *alias = newNameAliasDclNode(name, (INode*)target);
    inodeLexCopy((INode*)alias, (INode*)importnode);
    alias->flags |= FlagImportName;
    if (importnode->ispub)
        alias->flags |= FlagPub;
    importnode->binding = alias;
    return importnode;
}

// Parse import statement. 'pubflag' re-exports what the import binds: the
// module's name here, and every name it folds in.
//
// What follows the module is a 'use' clause, the one a global carries: '*', a
// list with 'as', a block, '* but', and 'pub use'. The clause is the one
// spelling of a fold: '.*' and '.name' are refused, naming it. The two 'pub's
// cannot disagree because they do not overlap: 'pub import' reaches every
// binding, and 'pub use' only the folds, so both together ('pub import m pub
// use ...') say what 'pub import' says alone.
ImportNode *parseImport(ParseState *parse, uint16_t pubflag) {
    // Create import node
    ImportNode *importnode = newImportNode();
    importnode->ispub = pubflag ? 1 : 0;
    lexNextToken();

    // Parse name of imported module. A bare identifier may name a neighbour in
    // the registry; a quoted string is a path and nothing else
    int isname = lexIsToken(IdentToken);
    char *filename = parseFilename();
    char *modstr = fileName(filename);

    // Process name folding instructions. A '.' after the module is refused
    // whatever follows it, and what it meant is passed over, so the statement
    // still ends where it was written
    if (lexIsToken(DotToken)) {
        lexNextToken();
        if (lexIsToken(StarToken)) {
            errorMsgLex(ErrorBadTerm, "An import does not fold with '.*': a wildcard import is written with a 'use' clause, as in 'import mod use *'.");
            lexNextToken();
            // Read as the 'use *' it means, unless a clause follows to say otherwise
            if (!parseIsFoldClause()) {
                importnode->fold = newFoldClause();
                importnode->fold->star = 1;
            }
        }
        else {
            errorMsgLex(ErrorBadTerm, "An import does not name a member with '.': a selective import is written with a 'use' clause, as in 'import mod use a, b as c'.");
            if (lexIsToken(IdentToken))
                lexNextToken();
        }
    }
    if (parseIsFoldClause())
        importnode->fold = parseFoldClause(parse, FoldMayPub);
    if (importnode->fold && pubflag)
        importnode->fold->ispub = 1;
    parseEndOfStatement();

    Name *filesym = nametblFind(modstr, strlen(modstr));

    // THE REGISTRY FIRST, AND THE FILESYSTEM ONLY AFTER IT. A neighbour has
    // already been found -- the folder sweep drew it -- so reaching it is a
    // lookup in a namespace and never a second load of its files
    ModuleNode *newmod = isname ? parseImportRegistry(parse, filesym) : NULL;
    if (newmod == parse->mod)
        newmod = NULL;      // a module publishes its own name; see below
    if (newmod != NULL && newmod == (ModuleNode*)parse->mod->dclinfo.owner) {
        // Containment runs one way. A child naming its parent is a cycle back
        // along the edge that contains it, which is the one shape the module
        // tree rules out
        errorMsgLexAfter(ErrorModReach,
            "Module %s is this module's parent, and a module may not import the module that contains it.",
            &newmod->namesym->namestr);
        return NULL;
    }

    // THE REGISTRY HOLDS MORE THAN MODULES [Jon 23 Sep]. A submodule's bare name
    // may be any public name of its parent -- a type, a function, a global -- and
    // is bound as an alias rather than loaded. The submodule is parsed before its
    // parent's own files, so only its sisters are there to be found yet: a name
    // no file answers either is held, and bound in the fold passes once the
    // parent's namespace is complete (importBindName). A package is a file the
    // name reaches, like any other
    if (newmod == NULL && isname && parse->mod->dclinfo.owner != NULL
        && fileFindSrc(lex ? lex->url : NULL, filename) == NULL)
        return parseImportName(parse, importnode, filesym);
    if (newmod == NULL && isname && parse->mod->dclinfo.owner != NULL)
        importnode->isnamedfile = 1;

    if (newmod == NULL) {
        // Nothing of that name in the registry, so the name is a FILE PATH:
        // beside the importing file, and then on the package search path, which
        // is how 'import stdio' reaches the packages folder
        newmod = parseLoadAndParseModuleFile(parse, filename, filesym);

        // A file of this module's own folder is already part of this module, so
        // naming it here asks the module to import itself. The folder is what
        // brings a sibling file in; 'import' reaches a different module
        if (newmod == parse->mod) {
            // Reported after the statement's ';' rather than at the token the
            // parse has reached, which is the next declaration: the import is
            // what is wrong, and the lexer has already moved past it
            errorMsgLexAfter(ErrorModFile,
                "This file is already part of module %s: a file of the module's folder joins it without being imported.",
                &newmod->namesym->namestr);
            return NULL;
        }

        // Nor is a submodule imported. A subfolder holding its own designated file
        // is already a module of this one, bound under the name its folder gives
        // it, so naming it here asks for a second binding of a name it already has
        if (newmod->dclinfo.owner == (INode*)parse->mod) {
            errorMsgLexAfter(ErrorModFile,
                "Module %s is a submodule of this one: where its folder or its file sits is what brings it in, so it is named without being imported.",
                &newmod->namesym->namestr);
            return NULL;
        }

        // ANY other module inside a tree is refused, and this is the whole of the
        // sideways rule. The registry above is what reaches a neighbour; a path
        // that happens to arrive at one reaches it by accident of spelling, and
        // under the scoped rule the ones it would reach are exactly those that
        // must not resolve -- an ancestor's sister, a stranger's child.
        //
        // What it also closes is a MISCOMPILE. The path is canonical now, so a
        // spelling that walks to a sister finds the module the sweep already
        // registered instead of building a second one from her files; both spelt
        // the same symbols, LLVM renamed the second, and the call was left
        // referencing a declaration nothing defined
        if (newmod->dclinfo.owner != NULL) {
            ModuleNode *owner = (ModuleNode*)newmod->dclinfo.owner;
            errorMsgLexAfter(ErrorModReach,
                "Module %s is a module of %s, and a module inside a module tree is reached by its name where its parent's registry holds it, not by a path to its file.",
                &newmod->namesym->namestr, &owner->namesym->namestr);
            return NULL;
        }
    }

    // ONE IMPORT OF A MODULE PER MODULE, and a second is refused, naming where
    // the first one is. An identical repeat is two ways of bringing in the same
    // thing, which is a cleanliness issue [Jon 23 Sep]; one that differs would
    // leave the module's name and its folds meaning two things.
    importnode->module = newmod;
    ImportNode *prior = parseImportPrior(parse->mod, newmod);
    if (prior != NULL) {
        if (importSame(prior, importnode))
            errorMsgNode((INode*)importnode, ErrorDupImport,
                "Module %s is imported already, the same way, at %s:%u. A module imports another once: leave out the second.",
                &newmod->namesym->namestr, prior->lexer->url, prior->linenbr);
        else
            errorMsgNode((INode*)importnode, ErrorDupImport,
                "Module %s is imported already, differently, at %s:%u. A module imports another once: write what both say in one import.",
                &newmod->namesym->namestr, prior->lexer->url, prior->linenbr);
        return NULL;
    }

    // Bind the module's name here, as an alias carrying this import's own
    // visibility. The name is the one the module declares for itself -- its
    // folder's, or its file's until a folder names it
    importBindModule(parse->mod, importnode);

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

// Parse the default fold a 'mod' line's 'use' clause names, with the lexer on
// the 'use' or on the 'pub' before it. It is an import's clause -- names, a
// block, '*', '* but' -- read by the one clause parser, less the two things
// nobody ruled for it: 'as', since what an importer calls a name is the
// importer's to say in a clause of its own, and 'pub', since each import decides
// how visible its folds are. Each is refused, and the clause taken without it.
// Whether each name is a public name of the module is known only once its
// declarations and folds are, so that is checked in the fold pass that reports
// (modDefaultFoldCheck).
static FoldClause *parseModDefaultFold(ParseState *parse) {
    if (lexIsToken(PubToken) || lexNextIsWord("pub"))
        errorMsgLex(ErrorBadPub,
            "A 'mod' line's 'use' names what a bare import of this module folds, and each import decides how visible its folds are, so 'pub' has nothing to say here. Write 'pub import' where the module is imported.");
    FoldClause *fold = parseFoldClause(parse, FoldRecover);
    fold->ispub = 0;
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(fold->items, cnt, itemp)) {
        AliasDclNode *alias = (AliasDclNode*)*itemp;
        Name *srcname = ((NameUseNode*)alias->target)->namesym;
        if (alias->namesym == srcname)
            continue;
        errorMsgNode((INode*)alias, ErrorBadFold,
            "A 'mod' line's 'use' folds each name into its importers under the module's own spelling. An importer that wants %s as %s writes that in its own clause: 'import ... use %s as %s'.",
            &srcname->namestr, &alias->namesym->namestr, &srcname->namestr, &alias->namesym->namestr);
        alias->namesym = srcname;
    }
    return fold;
}

// Parse a 'mod' declaration, which declares the module a folder's files belong
// to.
//
// Only the header form is built: 'mod name;' as the first statement of the
// module's designated file, or of a one-file module's one file. What NAMES the
// module is its folder, or a one-file module's file, which is a filesystem fact a
// tool that cannot parse Cone can read off a path; a name written here is checked
// against it and may not replace it. A lone file that is no module's submodule --
// today's program, or a file an import reached by path -- has no such name to
// check against, so its declaration still renames it, and that is transitional.
//
// The declaration also binds the module's name into the module's own namespace
// -- a module is the registry its own contents resolve against, and it publishes
// itself into it -- except where its folder or its file already did so at load,
// which is what makes a module-level name a local or a type member hides
// reachable again, as 'name.x'.
//
// 'mod name extends base;' makes this module one that reuses another: every
// declaration and fold of the base becomes a name of this one, as visible here as
// there and still the base's declaration, while the base's imports stay its own
// dependencies (modExtendsResolve, modFoldNames). One base, named by one name.
//
// 'mod bigint use BigInt;' names what a BARE import of this module folds by
// default [Jon 23 Sep]: a package holding one thing becomes that thing where it
// is imported. The clause runs the other way from every other 'use' -- it folds
// into the IMPORTER, not into the module it is written in -- and it is not an
// export list: 'pub' still decides what is reachable, and the clause only picks
// the fold an import writing none of its own gets (parseModDefaultFold). With
// 'extends', it comes last: 'mod bigint extends base use BigInt;'.
//
// 'mod trait', a module's abstraction, is admitted and refused because nothing
// is behind it yet: reporting it where it is written is what settles its
// spelling without accepting it. A 'mod name { ... }' block is recognised only
// to refuse it, since it does not exist: a module is never declared inside a
// file, and a nested module is a file of its own or a subfolder with its own
// designated file.
//
// 'atmodstart' is whether this is the first statement of the module's designated
// or one file. The declaration claims the module, so nothing may precede it, a
// second one has nothing left to declare, and a file the folder swept in carries
// none at all: a swept file that opened with one would have been a one-file
// module rather than swept.
//
// 'pub' is what opens a SUBMODULE to its parent's neighbours: a submodule is
// private to its parent unless its own declaration says otherwise, which is the
// rule every declaration follows, reaching the declaration that draws a module.
// Its own declaration is the only place it can be written, since the parent
// declares nothing about a subfolder or a file -- where it sits is the
// declaration. A module with no parent has nothing to be visible outside of.
void parseModuleDcl(ParseState *parse, ModuleNode *mod, int atmodstart, uint16_t pubflag) {
    // Where the declaration is written. The module node was made positioned at
    // the first line of its designated file, which is the nearest thing a module
    // named by its folder has to a declaration; an accepted declaration is a
    // nearer one, so it gives the node this position, and a duplicate of the
    // module's name is then reported against the declaration
    INode dclat;
    dclat.lexer = lex;
    dclat.srcp = lex->tokp;
    dclat.linep = lex->linep;
    dclat.linenbr = lex->linenbr;

    // A submodule is the module its parent owns, and the only one 'pub' can speak
    // for. The root, a module that is a file of its own and an imported module are
    // each inside nothing
    int issubmodule = mod->dclinfo.owner != NULL;
    if (pubflag && !issubmodule)
        errorMsgLex(ErrorBadPub,
            "'pub' on a module declaration says the module is visible outside its parent module, and this module has no parent.");
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

    // 'extends' names the one module this one reuses, by the name it is reached
    // by: a sister, or a module this module imports. What it names is resolved
    // with the module's other names, once every import is bound, so it is only
    // recorded here
    NameUseNode *extendsname = NULL;
    if (lexIsToken(ExtendsToken)) {
        lexNextToken();
        if (lexIsToken(IdentToken)) {
            extendsname = newNameUseNode(lex->val.ident);
            lexNextToken();
            // Each refusal passes over what it refused, so the declaration still
            // names the module and the statement still ends where it was written
            if (lexIsToken(DotToken)) {
                errorMsgLex(ErrorModExtends,
                    "A module's 'extends' names a module by one name: a sister, or a module this module imports. Import a module further away, and name it here.");
                extendsname = NULL;
                while (lexIsToken(DotToken) || lexIsToken(IdentToken))
                    lexNextToken();
            }
            if (lexIsToken(CommaToken)) {
                errorMsgLex(ErrorExtends, "A module extends one module.");
                while (lexIsToken(CommaToken) || lexIsToken(IdentToken) || lexIsToken(DotToken))
                    lexNextToken();
            }
        }
        else
            errorMsgLex(ErrorNoName, "Expected the name of the module this one extends");
    }

    // What a bare import of this module folds by default, last on the line
    FoldClause *deffold = NULL;
    if (parseIsFoldClause()) {
        deffold = parseModDefaultFold(parse);
        if (lexIsToken(ExtendsToken)) {
            errorMsgLex(ErrorBadFold,
                "A 'mod' line's 'use' comes last, after 'extends': 'mod name extends base use ...'.");
            while (lexIsToken(ExtendsToken) || lexIsToken(IdentToken))
                lexNextToken();
        }
    }

    // An in-file module block. Nesting is by files and folders only, so there is
    // no such construct; its body is skipped so that nothing in it is reported again
    if (lexIsToken(LCurlyToken) || lexIsToken(ColonToken)) {
        errorMsgLex(ErrorUnbuiltKind,
            "A module cannot be declared inside a file: a nested module is a file of its own, or a subfolder with its own designated file.");
        parseSkipDclBody();
        return;
    }
    // Reported before the statement's ';' is consumed, so that the diagnostic
    // lands on this declaration rather than on the token that follows it
    if (modname != NULL) {
        if (!atmodstart || (mod->flags & FlagModDcl))
            errorMsgLex(ErrorModDcl,
                "A 'mod' declaration must be its file's first statement, and a module declares itself once. A file that does not open with one is a file of its folder's module, and declares nothing.");
        else {
            mod->flags |= FlagModDcl;
            mod->extendsname = (INode*)extendsname;
            mod->deffold = deffold;
            // What 'pub' does, where there is a parent for it to speak to: the
            // submodule joins its parent's namespace as a public name, which its
            // parent's neighbours may then name a path through
            if (pubflag && issubmodule) {
                mod->flags |= FlagPub;
                mod->dclinfo.facts &= ~DclPrivate;
            }
            if (mod->foldersym != NULL) {
                // The folder names the module and has bound that name already
                if (modname != mod->foldersym)
                    errorMsgLex(ErrorModName,
                        "This module is named for its folder, '%s'. A 'mod' declaration may restate that name; it may not change it.",
                        &mod->foldersym->namestr);
                else
                    copyNodeLex(mod, &dclat);
            }
            else if (issubmodule) {
                // A one-file module: its file names it, as a folder would, and
                // its parent has bound that name already
                if (modname != mod->filesym)
                    errorMsgLex(ErrorModName,
                        "This module is named for its file, '%s.cone'. A 'mod' declaration may restate that name; it may not change it.",
                        &mod->filesym->namestr);
                else
                    copyNodeLex(mod, &dclat);
            }
            else {
                // A module that is one file is still named after that file,
                // which is transitional, so its declaration may rename it
                mod->namesym = modname;
                copyNodeLex(mod, &dclat);
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

        // 'pub' on an import is RE-EXPORT: every binding the import makes -- the
        // module's name here, and each name it folds in -- is a public name of
        // this module. That is 'pub' with its one meaning, on a binding as on a
        // declaration, and it is what makes transit fall out: a third module sees
        // through this one exactly what this one re-exported.
        case ImportToken: {
            ImportNode *newnode = parseImport(parse, pubflag);
            if (newnode)
                modAddNode(mod, NULL, (INode*)newnode);
            break;
        }

        // Retired, and reported once however it is written: a 'pub' before it
        // speaks for a statement that is gone, so it earns no diagnostic of its own
        case IncludeToken:
            parseRetiredInclude();
            break;

        // 'use' folds an enum's variants, or a submodule's public names, in as
        // names of this module. The bindings it makes are the fold's, so their
        // visibility is the fold's to say, and it says it the way every
        // declaration and every fold clause does: 'pub' first, as 'pub use'.
        case UseToken: {
            parseBadStatic(staticflag);
            ModUseNode *use = parseModUse(parse, pubflag);
            modAddNode(mod, NULL, (INode*)use);
            break;
        }

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

        // 'mod' names the module this file belongs to. 'pub' says a SUBMODULE is
        // visible outside its parent, and is refused where the module has no
        // parent; a module has no instances for a 'static' to be shared across
        case ModToken:
            parseBadStatic(staticflag);
            parseModuleDcl(parse, mod, atstart, pubflag);
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

// Give a module the position of the file that makes it one, line 1 column 1:
// its designated file, or the one file of a module that is a file. A module
// named by its folder has no declaration in any source, and the file that makes
// the folder a module is the nearest thing it has to one, so that is where a
// diagnostic about the module -- the module's half of a duplicate name -- is
// reported. A 'mod' declaration, where the file makes one, moves it there.
//
// The file is read now, when the module is made, because a module's name is
// bound before any of its files is parsed and a collision is reported as the
// second binding is made. The block that holds it is what parseModuleFilesParse
// later makes current, so the file is still read once. 'block' is the file's
// block where the sweep that found the file has read it already
Lexer *parseModulePosition(ModuleNode *mod, char *path, Lexer *block) {
    Lexer *file = block ? block : lexLoadPath(path);
    mod->lexer = file;
    mod->srcp = mod->linep = file->source;
    mod->linenbr = 1;
    return file;
}

// Parse every file of a module, in the order the sweep collected them: the
// designated file first, since it is the only one that may declare the module,
// and then each file the folder brought in. A file dropped by registration --
// one another module holds, or one whose basename collides -- is skipped.
// Each file's block is the one read when the module was made or its folder swept
void parseModuleFilesParse(ParseState *parse, ModuleNode *mod, SrcFiles *files) {
    for (uint32_t i = 0; i < files->count; ++i) {
        if (files->paths[i] == NULL)
            continue;
        if (files->blocks[i] != NULL)
            lexPush(files->blocks[i]);
        else
            lexInjectPath(files->paths[i]);
        parseGlobalStmts(parse, mod, i == 0);
        if (lex->toktype != EofToken) {
            errorMsgLex(ErrorNoEof, "Expected end-of-file");
        }
        lexPop();
    }
}

// A submodule drawn but not yet parsed: its node, bound in its parent, and the
// files it will be parsed from, its designated or one file first
typedef struct DrawnModule {
    ModuleNode *mod;          // NULL where the submodule's file could not join
    SrcFiles files;
    SrcFiles submodules;
} DrawnModule;

void parseSubmoduleDraw(ParseState *parse, ModuleNode *parent, char *path, Lexer *block, DrawnModule *drawn);
void parseSubmoduleParse(ParseState *parse, DrawnModule *drawn);

// Add the auto-import of the core package, which every module but core itself
// carries, ahead of whatever the module's own files import. Core is loaded
// before any other module, and is the one loaded while parse->core is unset
void parseAddCorelibImport(ParseState *parse, ModuleNode *mod) {
    ModuleNode *corelib = parse->core;
    if (corelib == NULL || corelib == mod)
        return;
    ImportNode *importnode = newImportNode();
    importnode->fold = newFoldClause();
    importnode->fold->star = 1;
    importnode->module = corelib;
    modAddNode(mod, NULL, (INode*)importnode);
}

// Draw the submodules this module holds, then parse them, then
// parse this module's own files. The module's hook is current, so every name
// added joins this module's namespace.
//
// EVERY SUBMODULE IS DRAWN BEFORE ANY IS PARSED, which is the same rule the
// files follow and holds for the same reason at one level up: what this module's
// namespace holds may not depend on the order its subfolders were reached in. A
// submodule importing a SISTER resolves that name against this namespace, so a
// sister drawn later would be a name that was not there -- and the sister's
// files would be unregistered too, so a path spelled to one would read them a
// second time and build a duplicate module.
//
// The submodules also come before this module's own files, and both reasons are
// about what a name means before a file is read. A submodule is a name of this
// namespace that no statement in any of these files declares, so binding it
// ahead of them makes a collision with a declaration report the declaration as
// the duplicate and the submodule -- at its 'mod' declaration, or at its
// designated file's first line where it has none -- as the name it met. And it registers the submodule's files, so a file of this
// module that names one of them reaches the module that holds it rather than
// reading it a second time
void parseModuleTree(ParseState *parse, ModuleNode *mod, SrcFiles *files, SrcFiles *submodules) {
    DrawnModule *drawn = submodules->count
        ? (DrawnModule*)memAllocBlk(submodules->count * sizeof(DrawnModule)) : NULL;
    for (uint32_t i = 0; i < submodules->count; ++i)
        parseSubmoduleDraw(parse, mod, submodules->paths[i], submodules->blocks[i], &drawn[i]);
    for (uint32_t i = 0; i < submodules->count; ++i)
        parseSubmoduleParse(parse, &drawn[i]);
    parseModuleFilesParse(parse, mod, files);
}

// Draw a submodule: the module a subfolder's designated file names, holding that
// folder's files and whatever its own subfolders draw, or a one-file module,
// holding its one file. Either is bound in its parent's namespace under the name
// its folder or its file gives it, and nothing else tells the two apart: a module
// grows from a file into a folder without anyone who names it seeing a change.
//
// Two things make it a child rather than a neighbour. Its parent OWNS it, which is
// what puts the parent's name in front of every symbol it declares, and it is
// private to its parent unless its declaration says 'pub'. Everything else about
// it is what any module has, which is why this is one branch of the sweep rather
// than a second kind of module.
//
// The parent's hook is current when this is called, which is what the binding
// needs: the name joins the parent's namespace and is unhooked when the parent's
// parse ends. Drawing is separated from parsing so that every sister of a level
// is bound and registered before any of their files is read
void parseSubmoduleDraw(ParseState *parse, ModuleNode *parent, char *path, Lexer *block, DrawnModule *drawn) {
    drawn->mod = NULL;
    Name *pathsym = nametblFind(path, strlen(path));
    ModuleNode *held = pgmFindFile(parse->pgm, pathsym);
    if (held) {
        // A subfolder is swept by exactly one module folder, and the parent
        // registers its own files before drawing any submodule, so the only way
        // here is a file named twice by a path spelled two ways
        errorMsg(ErrorModFile,
            "Source file %s already belongs to module %s, and a file belongs to one module.",
            path, &held->namesym->namestr);
        return;
    }

    // Generated exactly when its parent is: a submodule of a module this object
    // only declares is declared with it
    ModuleNode *mod = pgmAddMod(parse->pgm, parent->flags & FlagGenMod);
    // Positioned before it is bound, since the binding is where a collision
    // with its parent's own name is found
    Lexer *file = parseModulePosition(mod, path, block);
    char *basename = fileName(path);
    mod->filesym = nametblFind(basename, strlen(basename));
    // The folder names the module, as it does for any module a designated file
    // draws -- and here the folder is a subfolder of the parent's. A one-file
    // module has no folder of its own, and its file names it
    mod->foldersym = parseDesignatedFolder(path);
    mod->namesym = mod->foldersym ? mod->foldersym : mod->filesym;
    // The parent owns it: a submodule's declarations are spelled after the
    // parent's name, and dclInfoJoin makes it private to the parent until a 'pub'
    // on its declaration says otherwise
    dclInfoJoin((INode*)mod, (INode*)parent);
    mod->dclinfo.facts |= DclNamesChain;
    // Bound in the parent, which is how the parent reaches it: 'sub.name' is the
    // ordinary path rule through a namespace, and a duplicate of the name is the
    // ordinary namespace rule
    modAddNamedNode(parent, mod->namesym, (INode*)mod);

    // Its files, all registered before any of THIS module's or any sister's is
    // parsed, for the reason the enclosing module's are. A one-file module
    // sweeps nothing: the folder it sits in is its parent's
    parseModuleFiles(&drawn->files, &drawn->submodules, path, file, mod->foldersym != NULL);
    parseRegisterModuleFiles(parse, mod, &drawn->files);
    parseAddCorelibImport(parse, mod);
    drawn->mod = mod;
}

// Parse a drawn submodule's files, and draw and parse its own submodules. The
// parent's hook is current; this swaps it over, so a submodule neither sees nor
// collides with its parent's names
void parseSubmoduleParse(ParseState *parse, DrawnModule *drawn) {
    ModuleNode *mod = drawn->mod;
    if (mod == NULL)
        return;
    ModuleNode *svmod = parse->mod;
    parse->mod = mod;
    modHook(svmod, mod);
    // Its folder or its file names it, so its name is in reach inside it whether
    // or not a declaration restates it
    modAddNamedNode(mod, mod->namesym, (INode*)mod);
    parseModuleTree(parse, mod, &drawn->files, &drawn->submodules);
    modHook(mod, svmod);
    parse->mod = svmod;
}

// Load the module whose file is at 'path', unless a module holds that file
// already, then fully parse it: register the file and every other file its folder
// sweeps in, and parse each of them into the module. 'genflag' is FlagGenMod
// where this object is to hold the module's bodies, and 0 where it only declares
// them.
//
// The de-dup key is the file's PATH, because what must happen exactly once is
// reading the file; neither the filename nor a 'mod' declaration's name decides
// it, and either may be shared by files in different folders
static ModuleNode *parseLoadModulePath(ParseState *parse, char *path, Name *filesym, uint16_t genflag) {
    Name *pathsym = nametblFind(path, strlen(path));

    // REGISTER. If a module holds this file already, that module is what the
    // name reaches: the file is not read a second time
    ModuleNode *mod = pgmFindFile(parse->pgm, pathsym);
    if (mod)
        return mod;

    // Create and add this new module to list of modules, and make it the current one
    ModuleNode *svmod = parse->mod;
    mod = pgmAddMod(parse->pgm, genflag);
    Lexer *dsgfile = parseModulePosition(mod, path, NULL);
    mod->filesym = filesym;
    // The module's name is a filesystem fact: its folder's, where a designated
    // file drew the module out of a folder, and its file's otherwise. Filename
    // naming is transitional and is what a designated file replaces
    mod->foldersym = parseDesignatedFolder(path);
    mod->namesym = mod->foldersym ? mod->foldersym : filesym;
    // Every loaded module names itself in the owner chain; only the root does not
    dclInfoJoin((INode*)mod, NULL);
    mod->dclinfo.facts |= DclNamesChain;
    parse->mod = mod;

    // The module's files, all registered before any of them is parsed, so that
    // which files the module holds does not depend on what the parse of one of
    // them imports, and the designated file of every subfolder that draws a
    // submodule of it
    SrcFiles files, submodules;
    parseModuleFiles(&files, &submodules, path, dsgfile, mod->foldersym != NULL);
    parseRegisterModuleFiles(parse, mod, &files);

    // Before parsing, all modules (except core) get an auto-import of core
    parseAddCorelibImport(parse, mod);

    // Parse the module's source, then pop lexer and name hook
    modHook(svmod, mod);
    // A module folder's name is in reach inside the module whether or not a
    // declaration restates it, since the folder is what names it
    if (mod->foldersym)
        modAddNamedNode(mod, mod->namesym, (INode*)mod);
    parseModuleTree(parse, mod, &files, &submodules);
    modHook(mod, svmod);

    // Restore focus to original module we were working on
    parse->mod = svmod;
    return mod;
}

// Load the module a name reaches, unless a module holds its file already, then
// fully parse it. The name is LOCATED beside the importing file first, and then
// on the package search path: each '--path' folder, then the packages folder,
// which is where 'import stdio' finds stdio.
//
// Where it was found decides whether its bodies are generated. UNTIL SEPARATE
// COMPILATION LANDS, EVERY MODULE FOUND ON THE SEARCH PATH IS COMPILED INTO THIS
// OBJECT: a package is Cone source and nothing else supplies its definitions, so
// a program that imports one links. A module found beside its importer is
// declared and not generated, which is the separate-compilation gap
// (compiler/c/doc/nodes/module.md)
ModuleNode *parseLoadAndParseModuleFile(ParseState *parse, char *filename, Name *filesym) {
    uint16_t genflag = 0;
    char *path = fileFindLocal(lex ? lex->url : NULL, filename);
    if (path == NULL) {
        path = fileFindPackage(filename);
        genflag = FlagGenMod;
    }
    if (path == NULL)
        errorExit(ExitNF, "Cannot find or read source file %s", filename);
    return parseLoadModulePath(parse, path, filesym, genflag);
}

// Load the core package, the prelude every module imports. It is found on the
// package search path and nowhere else, so no file beside a program can stand in
// for it, and like every package found there it is compiled into this object
static ModuleNode *parseLoadCore(ParseState *parse) {
    char *path = fileFindPackage("core");
    if (path == NULL)
        errorExit(ExitNF, "Cannot find the core package, core/core.cone, on the package search path. The packages folder is named by CONE_PACKAGES or built into the compiler, and '--path' adds folders ahead of it.");
    return parseLoadModulePath(parse, path, nametblFind("core", 4), FlagGenMod);
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
    parse.core = NULL;

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
    Lexer *dsgfile = parseModulePosition(mod, path, NULL);
    mod->foldersym = parseDesignatedFolder(path);
    mod->namesym = mod->foldersym ? mod->foldersym : mod->filesym;
    SrcFiles files, submodules;
    parseModuleFiles(&files, &submodules, path, dsgfile, mod->foldersym != NULL);
    parseRegisterModuleFiles(&parse, mod, &files);

    // Load and parse the core package, auto-imported into main source and, from
    // here on, into every module loaded
    ModuleNode *corelib = parseLoadCore(&parse);
    parse.core = corelib;
    ImportNode *importnode = newImportNode();
    importnode->fold = newFoldClause();
    importnode->fold->star = 1;
    importnode->module = corelib;
    modAddNode(mod, NULL, (INode*)importnode);

    // Now actually parse the main module's files
    parse.mod = mod;
    modHook(NULL, mod);
    if (mod->foldersym)
        modAddNamedNode(mod, mod->namesym, (INode*)mod);
    // A stray '}' at global scope ends a file's statement loop. Without the
    // end-of-file check inside, the rest of that file would be silently
    // discarded
    parseModuleTree(&parse, mod, &files, &submodules);
    modHook(mod, NULL);
    return pgm;
}
