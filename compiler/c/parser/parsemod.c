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
static ModuleNode *parseLoadBuildImport(ParseState *parse, BuildImport *import);

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

    // IN A DESCRIBED MODULE THE COMPILER NEVER SEARCHES. After the registry, a
    // name is answered by the build description's import line for it, which says
    // where the file is; a submodule's other bare names are its parent's, as
    // below; and anything else -- a quoted path, a name the description does
    // not provide -- is refused, naming what the description lacks. An include
    // file an import line loaded imports like any module file [Jon 25 Sep], and
    // its lines are the description's package lines (parseBuildImportModule).
    // The file an import line names is loaded below, where every file an import
    // reaches is checked
    BuildImport *buildimport = NULL;
    if (newmod == NULL && parse->build != NULL) {
        buildimport = isname ? parseBuildFindImport(parse->build, filesym) : NULL;
        if (buildimport == NULL && isname && parse->mod->dclinfo.owner != NULL)
            return parseImportName(parse, importnode, filesym);
        if (buildimport == NULL) {
            if (isname && parse->build->isimport)
                errorMsgLexAfter(ErrorBuildImport,
                    "The build description has no package line for %s, which 'import %s' in module %s names: an include file's import is found where the description's top-level 'import %s: \"path\"' line says.",
                    filename, filename, &parse->mod->namesym->namestr, filename);
            else if (isname)
                errorMsgLexAfter(ErrorBuildImport,
                    "The build description provides nothing for 'import %s' in module %s: a described module's import is found where its 'import %s: \"path\"' line says.",
                    filename, &parse->mod->namesym->namestr, filename);
            else
                errorMsgLexAfter(ErrorBuildImport,
                    "An import in a described module is written by name, and found where the build description's import line for that name says; a path is not searched for.");
            return NULL;
        }
    }

    // THE REGISTRY HOLDS MORE THAN MODULES [Jon 23 Sep]. A submodule's bare name
    // may be any public name of its parent -- a type, a function, a global -- and
    // is bound as an alias rather than loaded. The submodule is parsed before its
    // parent's own files, so only its sisters are there to be found yet: a name
    // no file answers either is held, and bound in the fold passes once the
    // parent's namespace is complete (importBindName). A package is a file the
    // name reaches, like any other
    if (newmod == NULL && buildimport == NULL && isname && parse->mod->dclinfo.owner != NULL
        && fileFindSrc(lex ? lex->url : NULL, filename) == NULL)
        return parseImportName(parse, importnode, filesym);
    if (newmod == NULL && isname && parse->mod->dclinfo.owner != NULL)
        importnode->isnamedfile = 1;

    if (newmod == NULL) {
        // Nothing of that name in the registry, so the name is a FILE PATH:
        // where the build description's import line says, or else beside the
        // importing file and then on the package search path, which is how
        // 'import stdio' reaches the packages folder
        newmod = buildimport ? parseLoadBuildImport(parse, buildimport)
            : parseLoadAndParseModuleFile(parse, filename, filesym);

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

// Parse the '@c' marker, if the lexer is on one, into the facts and the string
// of 'dclinfo', returning whether it was there. It is written after the keyword
// of a 'mod' line or a 'fn' [Jon 23 Sep], in one of four forms: '@c', '@c("str")',
// '@c(system)' and '@c("str", system)'. What the string means is the position's:
// on a module ('onmod') a prefix every C name it gives carries, on a function
// its whole symbol. 'system' is the platform's system calling convention.
int parseCAttr(DclInfo *dclinfo, int onmod) {
    if (!lexIsToken(CAttrToken))
        return 0;
    lexNextToken();
    dclinfo->facts |= DclCName;
    if (!lexIsToken(LParenToken))
        return 1;
    lexNextToken();
    int wantcc = 1, bad = 0;
    if (lexIsToken(StringLitToken)) {
        if (lex->strlen == 0) {
            errorMsgLex(ErrorCAttr, onmod
                ? "A module's '@c' string is the prefix its C names carry, and an empty one is no prefix: write '@c'."
                : "A function's '@c' string is its whole symbol, and an empty one names nothing.");
            bad = 1;
        }
        else
            dclinfo->cname = lex->val.strlit;
        lexNextToken();
        wantcc = lexIsToken(CommaToken);
        if (wantcc)
            lexNextToken();
    }
    if (wantcc) {
        if (lexIsToken(IdentToken) && strcmp(&lex->val.ident->namestr, "system") == 0) {
            dclinfo->facts |= DclSystemCC;
            lexNextToken();
        }
        else {
            errorMsgLex(ErrorCAttr,
                "'@c' takes a string, 'system', or both: '@c(\"name\")', '@c(system)', '@c(\"name\", system)'.");
            bad = 1;
            while (!lexIsToken(RParenToken) && !lexIsToken(SemiToken) && !lexIsToken(LCurlyToken) && !lexIsToken(EofToken))
                lexNextToken();
        }
    }
    parseCloseTok(RParenToken);
    // A marker already reported is dropped whole, so that nothing is reported
    // again for what it would have said
    if (bad) {
        dclinfo->facts &= ~DclStated;
        dclinfo->cname = NULL;
        return 0;
    }
    return 1;
}

// Report 'extern' on a function whose body an importer must have to use it:
// an inline function is expanded where it is called, and a generic one is
// instantiated there, so neither has a definition elsewhere to reach
void parseExternFnCheck(FnDclNode *fn) {
    if (!(fn->flags & FlagExtern))
        return;
    if (fn->flags & FlagInline)
        errorMsgNode((INode*)fn, ErrorBadExtern,
            "An inline function is expanded where it is called, so it has no definition elsewhere for 'extern' to name. Write its body.");
    else if (fn->genericinfo)
        errorMsgNode((INode*)fn, ErrorBadExtern,
            "A generic function is instantiated where it is used, so it has no definition elsewhere for 'extern' to name. Write its body.");
}

// Parse function or variable, as it may be preceded by a qualifier
// Return NULL if not either
INode *parseFnOrVar(ParseState *parse, uint16_t flags) {

    if (lexIsToken(FnToken)) {
        FnDclNode *node = (FnDclNode*)parseFn(parse, (flags&FlagExtern)? (ParseMayName | ParseMaySig) : (ParseMayName | ParseMayImpl));
        node->flags |= flags;
        parseExternFnCheck(node);
        // A bare '@c' in a module that already gives its functions C names says
        // nothing: the name is C already [Penny 23 Sep, delegated by Jon]. The
        // string form is the override and says something; so does '@c(system)'
        // where the module's convention is not already the system one
        DclInfo *dclinfo = &node->dclinfo;
        DclInfo *modinfo = &parse->mod->dclinfo;
        if (node->namesym && (dclinfo->facts & DclCName) && dclinfo->cname == NULL && (modinfo->facts & DclCName)
            && (!(dclinfo->facts & DclSystemCC) || (modinfo->facts & DclSystemCC)))
            errorMsgNode((INode*)node, ErrorCNameTwice,
                "Module %s already gives its functions C names, so a bare '@c' on %s says nothing. To spell its symbol differently, write it: '@c(\"symbol\")'.",
                &parse->mod->namesym->namestr, &node->namesym->namestr);
        modAddFn(parse->mod, node);
        return (INode*)node;
    }

    // A global variable declaration, if it begins with a permission
    else if lexIsToken(PermToken) {
        // A module's global may carry a fold clause: it is the one-instance
        // analogue of a field, so 'config Config use *' admits Config's members
        // as names of this module, reached through 'config'. An 'extern' global
        // may too [Jon 25 Sep]: the fold reads only the declared type, which an
        // 'extern' global has, and 'extern' says only that the global is defined
        // in a differently compiled unit. It is how an include file passes on
        // what a folded global puts into its package's namespace.
        VarDclNode *node = parseVarDcl(parse, immPerm,
            (flags&FlagExtern) ? ParseMaySig | ParseMayFold : ParseMayImpl | ParseMaySig | ParseMayFold);
        node->flags |= flags;
        node->flowtempflags |= VarInitialized;   // Globals always hold a valid value
        parseEndOfStatement();
        modAddNode(parse->mod, node->namesym, (INode*)node);
        return (INode*)node;
    }
    else {
        errorMsgLex(ErrorBadGloStmt, "Expected function or variable declaration");
        parseSkipToNextStmt();
        return NULL;
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

// Pass over a member a module trait's body may not hold, however it is written:
// up to and including its ';', or its braced body whole, stopping short of the
// '}' that closes the trait
static void parseModTraitSkipMember() {
    while (!lexIsToken(EofToken) && !lexIsToken(RCurlyToken)) {
        if (lexIsToken(SemiToken)) {
            lexNextToken();
            return;
        }
        if (lexIsToken(LCurlyToken)) {
            parseSkipDclBody();
            if (lexIsToken(SemiToken))
                lexNextToken();
            return;
        }
        lexNextToken();
    }
}

// Parse a module trait, 'mod trait Shell { ... }', with the lexer on 'mod'.
//
// A module trait is a module's abstraction, as 'struct trait' is a struct's: the
// spelling is 'trait' modifying the kind. It is a declaration of the module whose
// file writes it, reached, imported and folded as any declaration is, and 'pub'
// before it is its own visibility. Its body holds the two things a module's
// interface to a framework is made of, functions and globals, each with its own
// 'pub': a function with a body and a global with an initialiser are DEFAULTS, a
// conforming module's unless it declares its own; a function with no body and a
// global with none are REQUIREMENTS. Nothing else is a member [Jon 23 Sep]: a type
// would be a trait requiring types, which is not built, and an import, a 'use',
// a macro or a 'mod' is not something a module conforms to. A generic function
// and an overload name are refused too, since a member is one name with one
// signature for a module to match. With no body, the trait is a marker with no
// members.
static ModTraitNode *parseModTrait(ParseState *parse, uint16_t pubflag) {
    lexNextToken();   // 'mod'
    lexNextToken();   // 'trait'
    ModTraitNode *trait = newModTraitNode(anonName);
    if (lexIsToken(IdentToken)) {
        trait->namesym = lex->val.ident;
        lexNextToken();
    }
    else
        errorMsgLex(ErrorNoName, "Expected a name for the module trait");
    trait->flags |= pubflag;

    if (!parseHasBlock()) {
        parseEndOfStatement();
        return trait;
    }
    parseBlockStart();
    while (!parseBlockEnd()) {
        uint16_t memberpub = parsePub();
        if (lexIsToken(FnToken)) {
            FnDclNode *fn = (FnDclNode*)parseFn(parse, ParseMayName | ParseMaySig | ParseMayImpl);
            fn->flags |= memberpub;
            if (fn->genericinfo)
                errorMsgNode((INode*)fn, ErrorModTraitBody,
                    "A module trait's function is one signature a conforming module declares or takes, so it cannot be generic.");
            else if (fn->overloadsym)
                errorMsgNode((INode*)fn, ErrorModTraitBody,
                    "A module trait's function is required or given under its own name, so it declares no overload name.");
            else if (fn->namesym)
                modTraitAddMember(trait, (INode*)fn);
        }
        else if (lexIsToken(PermToken)) {
            VarDclNode *var = parseVarDcl(parse, immPerm, ParseMayImpl | ParseMaySig);
            var->flags |= memberpub;
            var->flowtempflags |= VarInitialized;   // A global always holds a valid value
            parseEndOfStatement();
            modTraitAddMember(trait, (INode*)var);
        }
        else {
            errorMsgLex(ErrorModTraitBody,
                "A module trait holds functions and globals, each a requirement or a default, and nothing else: a type it would require is not built.");
            parseModTraitSkipMember();
        }
    }
    return trait;
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
// 'mod prog is Shell;' says the module conforms to a module trait: it declares,
// or takes the trait's default for, each member the trait has, and that is
// checked where it is written (modTraitConform, modTraitCheck). The line's order
// is 'mod prog extends base is Shell use Y;' [Jon 23 Sep].
//
// 'mod trait Shell { ... }' is not this declaration: it declares a module trait,
// a declaration of the module like any other (parseModTrait). A
// 'mod name { ... }' block is recognised only to refuse it, since it does not
// exist: a module is never declared inside a file, and a nested module is a file
// of its own or a subfolder with its own designated file.
//
// 'atmodstart' is 1 at the first statement of the module's designated or one
// file -- or, in a described module, of the first file the build description
// lists -- 2 at the first statement of any other of its files, and 0 anywhere
// else. The declaration claims the module, so nothing may precede it, a
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
//
// '@c' after 'mod' makes the module C-named: 'mod @c("SDL_") sdl;'. The
// module's naming is the only thing it states; whether a declaration is defined
// elsewhere is that declaration's own 'extern' [Jon 23 Sep] (parseCAttr).
void parseModuleDcl(ParseState *parse, ModuleNode *mod, int atmodstart, uint16_t pubflag) {
    // Where the declaration is written. The module node was made positioned at
    // the first line of its designated file, which is the nearest thing a module
    // named by its folder has to a declaration; an accepted declaration is a
    // nearer one, so it gives the node this position, and a duplicate of the
    // module's name is then reported against the declaration
    INode dclat;
    memset(&dclat, 0, sizeof(dclat));
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

    // '@c' after 'mod' gives the module C naming: every function and global it
    // owns directly is spelled as C spells it, after the prefix a string states
    // [Jon 23 Sep]. Read into a record of its own, and given to the module only
    // where the declaration is accepted
    DclInfo cattr;
    dclInfoInit(&cattr);
    parseCAttr(&cattr, 1);

    Name *modname = NULL;
    INode *ownword = mod->namesym ? mod->namesym->node : NULL;
    if (lexIsToken(IdentToken)) {
        modname = lex->val.ident;
        lexNextToken();
    }
    // The module's own name, where its folder or file named it after a keyword
    // or a permission. That was refused where the name was bound
    // (modAddNamedNode), and such a word never reads as a name, so the line
    // that restates it is taken as naming the module, not reported again
    else if (ownword && lex->val.ident == mod->namesym
        && ((ownword->tag == KeywordTag && lex->toktype == ownword->flags)
            || (ownword->tag == PermTag && lexIsToken(PermToken)))) {
        modname = mod->namesym;
        lexNextToken();
    }
    else
        errorMsgLex(ErrorNoName, "Expected a name for the module this file declares");

    // 'mod stack[T];' declares a GENERIC module, written as a generic type is:
    // its type parameters in square brackets after its name [Jon 23 Sep]. Nothing
    // of it is compiled until an instance, 'stack[i64]', is named (modInstantiate)
    GenericInfo *genericinfo = NULL;
    if (lexIsToken(LBracketToken)) {
        genericinfo = newGenericInfo();
        genericinfo->parms = parseGenericParms(parse);
    }

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

    // 'is' names the module trait this module conforms to, after 'extends' and
    // before 'use' [Jon 23 Sep]. One trait, by one name, as 'extends' names one
    // module: a trait further away is imported and folded in first. What it
    // names is resolved once every fold has run (modTraitConform), so it is only
    // recorded here
    NameUseNode *traitname = NULL;
    if (lexIsToken(IsToken)) {
        lexNextToken();
        if (lexIsToken(IdentToken)) {
            traitname = newNameUseNode(lex->val.ident);
            lexNextToken();
            if (lexIsToken(DotToken)) {
                errorMsgLex(ErrorModIs,
                    "A module's 'is' names a module trait by one name. Import the module that declares it and fold the trait in: 'import hosts use Shell'.");
                traitname = NULL;
                while (lexIsToken(DotToken) || lexIsToken(IdentToken))
                    lexNextToken();
            }
            if (lexIsToken(CommaToken)) {
                errorMsgLex(ErrorModIs, "A module conforms to one module trait.");
                while (lexIsToken(CommaToken) || lexIsToken(IdentToken) || lexIsToken(DotToken))
                    lexNextToken();
            }
        }
        else
            errorMsgLex(ErrorNoName, "Expected the name of the module trait this module conforms to");
        if (lexIsToken(ExtendsToken)) {
            errorMsgLex(ErrorModIs,
                "A 'mod' line's 'is' comes after 'extends': 'mod name extends base is Trait'.");
            while (lexIsToken(ExtendsToken) || lexIsToken(IdentToken))
                lexNextToken();
        }
    }

    // What a bare import of this module folds by default, last on the line
    FoldClause *deffold = NULL;
    if (parseIsFoldClause()) {
        deffold = parseModDefaultFold(parse);
        if (lexIsToken(ExtendsToken) || lexIsToken(IsToken)) {
            errorMsgLex(ErrorBadFold,
                "A 'mod' line's 'use' comes last, after 'extends' and 'is': 'mod name extends base is Trait use ...'.");
            while (lexIsToken(ExtendsToken) || lexIsToken(IsToken) || lexIsToken(IdentToken))
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
    // lands on this declaration rather than on the token that follows it.
    //
    // A DESCRIBED module's name is the build description's, bound at load as a
    // folder's is, and a 'mod' line opening any of its files is checked against
    // it: a file listed in the wrong module is reported as that, wherever in the
    // module's list it sits, before the rule that only the first may declare
    BuildModule *build = parse->build;
    if (modname != NULL && build != NULL && atmodstart && modname != build->name) {
        if (build->isimport)
            errorMsgLex(ErrorBuildModName,
                "The build description imports this file as '%s', and its 'mod' line names '%s'. The import's name is what names the module.",
                &build->name->namestr, &modname->namestr);
        else
            errorMsgLex(ErrorBuildModName,
                "The build description lists this file in module '%s', and its 'mod' line names '%s'. A file declares the module the description lists it in.",
                &build->name->namestr, &modname->namestr);
    }
    else if (modname != NULL) {
        if (atmodstart != 1 || (mod->flags & FlagModDcl))
            errorMsgLex(ErrorModDcl,
                "A 'mod' declaration must be its file's first statement, and a module declares itself once. A file that does not open with one is a file of its folder's module, and declares nothing.");
        else {
            mod->flags |= FlagModDcl;
            mod->extendsname = (INode*)extendsname;
            mod->traitname = (INode*)traitname;
            mod->deffold = deffold;
            mod->dclinfo.facts |= cattr.facts & DclStated;
            mod->dclinfo.cname = cattr.cname;
            // A C name has no room for an instance's type arguments, so each
            // instance's globals would be one symbol: a generic module keeps Cone
            // names, as a generic function in a C-named module does
            if (genericinfo && (cattr.facts & DclCName)) {
                errorMsgNode((INode*)&dclat, ErrorCAttr,
                    "A generic module cannot be C-named: each instance's functions and globals need a name of their own, which only a Cone name can carry.");
                mod->dclinfo.facts &= ~DclStated;
                mod->dclinfo.cname = NULL;
            }
            mod->genericinfo = genericinfo;
            // What 'pub' does, where there is a parent for it to speak to: the
            // submodule joins its parent's namespace as a public name, which its
            // parent's neighbours may then name a path through
            if (pubflag && issubmodule) {
                mod->flags |= FlagPub;
                mod->dclinfo.facts &= ~DclPrivate;
            }
            if (build != NULL) {
                // The description names the module and has bound that name
                // already; the declaration restates it, as checked above
                copyNodeLex(mod, &dclat);
            }
            else if (mod->foldersym != NULL) {
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

// A file's header is its 'mod' line, where it has one, and then its imports; a
// module's first file -- its designated file, a one-file module's file, a lone
// file, the first file a build description lists -- opens with the 'mod' line
// naming the module [Jon 24 Sep], and every file's imports come before anything
// else it declares. 'atmodstart' is as parseModuleDcl describes it
void parseGlobalStmts(ParseState *parse, ModuleNode *mod, int atmodstart) {
    // An empty first file has no statement to report against, so the end of
    // the file stands in for it
    if (atmodstart == 1 && lexIsToken(EofToken))
        errorMsgLex(ErrorNoModDcl,
            "A module's first file opens with its 'mod' line, and this file opens module '%s': begin it with 'mod %s;', after any comments.",
            &mod->namesym->namestr, &mod->namesym->namestr);
    // Set by the first statement that is neither the 'mod' line nor an import
    int pastheader = 0;
    while (lex->toktype!=EofToken && !parseBlockEnd()) {
        int atstart = atmodstart;
        atmodstart = 0;
        // Where the statement starts, before any 'pub' or 'static', which is
        // where a header rule reports it
        INode stmtat;
        memset(&stmtat, 0, sizeof(stmtat));
        stmtat.lexer = lex;
        stmtat.srcp = lex->tokp;
        stmtat.linep = lex->linep;
        stmtat.linenbr = lex->linenbr;
        uint16_t pubflag = parsePub();
        // Where the statement's keyword is, after its 'pub', and what it makes:
        // its span is recorded once it is parsed (dclspan.h)
        char *kwat = lex->tokp;
        INode *made = NULL;
        uint16_t spankind = SpanDcl;
        DclSpans *items = NULL;
        int modline = lexIsToken(ModToken) && !lexNextIsWord("trait");
        // The module is named by its folder, its file or its build description,
        // and the 'mod' line restates that name where a reader of the file will
        // look first. The statement is still parsed as written
        if (atstart == 1 && !modline)
            errorMsgNode(&stmtat, ErrorNoModDcl,
                "A module's first file opens with its 'mod' line, and this file opens module '%s': begin it with 'mod %s;', after any comments.",
                &mod->namesym->namestr, &mod->namesym->namestr);
        if (lexIsToken(ImportToken) && pastheader)
            errorMsgNode(&stmtat, ErrorImportLate,
                "Imports belong right after the 'mod' line, ahead of every other declaration: move this one up into the file's header.");
        // A retired 'include' is reported as that, and ends no header
        if (!modline && !lexIsToken(ImportToken) && !lexIsToken(IncludeToken))
            pastheader = 1;
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
            made = (INode*)newnode;
            spankind = SpanImport;
            break;
        }

        // Retired, and reported once however it is written: a 'pub' before it
        // speaks for a statement that is gone, so it earns no diagnostic of its own
        case IncludeToken:
            parseRetiredInclude();
            spankind = SpanOther;
            break;

        // 'use' folds an enum's variants, or a submodule's public names, in as
        // names of this module. The bindings it makes are the fold's, so their
        // visibility is the fold's to say, and it says it the way every
        // declaration and every fold clause does: 'pub' first, as 'pub use'.
        case UseToken: {
            parseBadStatic(staticflag);
            ModUseNode *use = parseModUse(parse, pubflag);
            modAddNode(mod, NULL, (INode*)use);
            made = (INode*)use;
            spankind = SpanUse;
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
            made = (INode*)newnode;
            break;
        }

        // 'struct'-style type definition, optionally modified by 'trait'
        case StructToken: {
            INode *node = parseStruct(parse, pubflag);
            modAddNode(mod, inodeGetName(node), node);
            made = node;
            break;
        }

        // 'trait' by itself is a synonym for 'struct trait': one node, one flag,
        // and the struct family by default
        case TraitToken: {
            INode *node = parseStruct(parse, TraitType | pubflag);
            modAddNode(mod, inodeGetName(node), node);
            made = node;
            break;
        }

        // 'mod' names the module this file belongs to. 'pub' says a SUBMODULE is
        // visible outside its parent, and is refused where the module has no
        // parent; a module has no instances for a 'static' to be shared across
        case ModToken:
            parseBadStatic(staticflag);
            // 'mod trait' declares a module trait, a declaration of this module
            // like any other, and 'pub' is its visibility
            if (lexNextIsWord("trait")) {
                ModTraitNode *trait = parseModTrait(parse, pubflag);
                modAddNode(mod, trait->namesym, (INode*)trait);
                made = (INode*)trait;
                break;
            }
            parseModuleDcl(parse, mod, atstart, pubflag);
            made = (INode*)mod;
            spankind = SpanModLine;
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
            spankind = SpanOther;
            break;
        }

        // 'enum' type definition: the closed family, in both size varieties.
        // Every variant is padded out to the size of the largest unless the
        // declaration writes '@unsized', so SameSize is the default that
        // attribute clears.
        case EnumToken: {
            INode *node = parseStruct(parse, TraitType | SameSize | EnumType | pubflag);
            modAddNode(mod, inodeGetName(node), node);
            made = node;
            break;
        }

        // 'macro'
        case MacroToken: {
            MacroDclNode *macro = parseMacro(parse);
            macro->flags |= pubflag;
            modAddNode(mod, macro->namesym, (INode*)macro);
            made = (INode*)macro;
            break;
        }

        // 'extern' qualifier in front of fn or var (block): defined elsewhere,
        // and nothing more [Jon 23 Sep]. The symbol is spelled by the module's
        // naming like any other declaration's, so in a Cone-named module an
        // extern declaration reaches a Cone package's definition, and a C
        // function is reached by a C-named module or the fn's own '@c'. A 'pub'
        // before 'extern' reaches every declaration in the block; one inside it
        // reaches that declaration alone.
        case ExternToken:
        {
            lexNextToken();
            uint16_t extflag = FlagExtern | pubflag;
            // 'extern system' said C naming and the system convention together,
            // which are now '@c(system)' on the fn
            if (lexIsToken(IdentToken)) {
                if (strcmp(&lex->val.ident->namestr, "system")==0)
                    errorMsgLex(ErrorCAttr,
                        "'extern' says only that a declaration is defined elsewhere. The system calling convention, and the C name that came with it, are '@c(system)' after 'fn': 'extern fn @c(system) name(...)'.");
                else
                    errorMsgLex(ErrorBadExtern, "'extern' is followed by a function, a global, or a block of them");
                lexNextToken();
            }
            if (lexIsToken(ColonToken) || lexIsToken(LCurlyToken)) {
                parseBlockStart();
                spankind = SpanExternBlock;
                while (!parseBlockEnd()) {
                    char *itemat = lex->tokp;
                    uint16_t itemflag = extflag | parsePub();
                    char *itemkw = lex->tokp;
                    if (lexIsToken(FnToken) || lexIsToken(PermToken)) {
                        INode *item = parseFnOrVar(parse, itemflag);
                        if (item)
                            parseSpan(parse, &items, item, itemat, itemkw, SpanDcl);
                    }
                    else {
                        errorMsgLex(ErrorNoSemi, "Extern expects only functions and variables");
                        parseSkipToNextStmt();
                    }
                }
            }
            else
                made = parseFnOrVar(parse, extflag);
        }
            break;

        // Function or variable
        case FnToken:
            parseBadStatic(staticflag);
            made = parseFnOrVar(parse, pubflag);
            break;
        case PermToken:
            made = parseFnOrVar(parse, pubflag | staticflag);
            break;

        // Named const declaration
        case ConstToken: {
            ConstDclNode *constnode = parseConstDcl(parse);
            constnode->flags |= pubflag;
            modAddNode(parse->mod, constnode->namesym, (INode*)constnode);
            made = (INode*)constnode;
            break;
        }

        default:
            errorMsgLex(ErrorBadGloStmt, "Invalid global area statement");
            lexNextToken();
            parseSkipToNextStmt();
            spankind = SpanOther;
            break;
        }

        DclSpan *span = parseSpan(parse, &mod->spans, made, stmtat.srcp, kwat, spankind);
        span->members = items;
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
        parseGlobalStmts(parse, mod, i == 0 ? 1 : 2);
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
    BuildModule *build;       // The build description's entry, where the module is described
} DrawnModule;

void parseSubmoduleDraw(ParseState *parse, ModuleNode *parent, char *path, Lexer *block, DrawnModule *drawn);
void parseSubmoduleParse(ParseState *parse, DrawnModule *drawn);
void parseBuildModuleTree(ParseState *parse, ModuleNode *mod, SrcFiles *files, BuildModule *build);

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
    importnode->iscore = 1;
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
    drawn->build = NULL;
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
    BuildModule *svbuild = parse->build;
    parse->mod = mod;
    parse->build = drawn->build;
    modHook(svmod, mod);
    // Its folder, its file or the build description names it, so its name is in
    // reach inside it whether or not a declaration restates it
    modAddNamedNode(mod, mod->namesym, (INode*)mod);
    if (drawn->build)
        parseBuildModuleTree(parse, mod, &drawn->files, drawn->build);
    else
        parseModuleTree(parse, mod, &drawn->files, &drawn->submodules);
    modHook(mod, svmod);
    parse->mod = svmod;
    parse->build = svbuild;
}

// ---------------------------------------------------------------------------
// A described build
//
// Where the compiler is given a BUILD DESCRIPTION (parsebuild.c), it is told
// the whole module tree of one package rather than finding it: each module's
// name, its files in order and its child modules. Nothing is swept and no file
// is probed; the folder rules are Congo's, which wrote the description, and
// the compiler checks what it was told against each file's own 'mod' line
// (parseModuleDcl). Everything else -- the registry, the order modules are drawn
// and parsed in, ownership, 'pub' -- is exactly what a folder tree gets.
// ---------------------------------------------------------------------------

// A described module's files, the first read into its block now, as a
// designated file's is, since the module takes its position from it
static void parseBuildFiles(SrcFiles *files, BuildModule *build, ModuleNode *mod) {
    parseSrcFilesInit(files);
    for (uint32_t i = 0; i < build->nfiles; ++i)
        parseSrcFilesAdd(files, build->files[i], i == 0 ? parseModulePosition(mod, build->files[0], NULL) : NULL);
}

// Draw a described child module: owned by its parent and bound in its
// namespace under the name the description gives it, private to it until its
// own 'mod' line says 'pub', and generated exactly when its parent is
static void parseBuildSubmoduleDraw(ParseState *parse, ModuleNode *parent, BuildModule *build, DrawnModule *drawn) {
    ModuleNode *mod = pgmAddMod(parse->pgm, parent->flags & FlagGenMod);
    parseBuildFiles(&drawn->files, build, mod);
    parseSrcFilesInit(&drawn->submodules);
    mod->filesym = mod->namesym = build->name;
    dclInfoJoin((INode*)mod, (INode*)parent);
    mod->dclinfo.facts |= DclNamesChain;
    modAddNamedNode(parent, mod->namesym, (INode*)mod);
    parseRegisterModuleFiles(parse, mod, &drawn->files);
    parseAddCorelibImport(parse, mod);
    drawn->mod = mod;
    drawn->build = build;
}

// Draw a described module's children, then parse them, then parse its own
// files: the order parseModuleTree keeps, for the same reasons. The children
// are taken in the order the description writes them
void parseBuildModuleTree(ParseState *parse, ModuleNode *mod, SrcFiles *files, BuildModule *build) {
    DrawnModule *drawn = build->nchildren
        ? (DrawnModule*)memAllocBlk(build->nchildren * sizeof(DrawnModule)) : NULL;
    for (uint32_t i = 0; i < build->nchildren; ++i)
        parseBuildSubmoduleDraw(parse, mod, build->children[i], &drawn[i]);
    for (uint32_t i = 0; i < build->nchildren; ++i)
        parseSubmoduleParse(parse, &drawn[i]);
    parseModuleFilesParse(parse, mod, files);
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
static ModuleNode *parseLoadModulePath(ParseState *parse, char *path, Name *filesym, uint16_t genflag, BuildModule *build) {
    Name *pathsym = nametblFind(path, strlen(path));

    // REGISTER. If a module holds this file already, that module is what the
    // name reaches: the file is not read a second time
    ModuleNode *mod = pgmFindFile(parse->pgm, pathsym);
    if (mod)
        return mod;

    // Create and add this new module to list of modules, and make it the current one
    ModuleNode *svmod = parse->mod;
    BuildModule *svbuild = parse->build;
    mod = pgmAddMod(parse->pgm, genflag);
    Lexer *dsgfile = parseModulePosition(mod, path, NULL);
    mod->filesym = filesym;
    // The module's name is a filesystem fact: its folder's, where a designated
    // file drew the module out of a folder, and its file's otherwise. Filename
    // naming is transitional and is what a designated file replaces. A file a
    // build description's import line names is one file, swept for nothing, and
    // the import's name is its name
    mod->foldersym = build ? NULL : parseDesignatedFolder(path);
    mod->namesym = mod->foldersym ? mod->foldersym : filesym;
    // Every loaded module names itself in the owner chain; only the root does not
    dclInfoJoin((INode*)mod, NULL);
    mod->dclinfo.facts |= DclNamesChain;
    parse->mod = mod;
    parse->build = build;

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
    // declaration restates it, since the folder is what names it; so is one a
    // build description names
    if (mod->foldersym || build)
        modAddNamedNode(mod, mod->namesym, (INode*)mod);
    parseModuleTree(parse, mod, &files, &submodules);
    modHook(mod, svmod);

    // Restore focus to original module we were working on
    parse->mod = svmod;
    parse->build = svbuild;
    return mod;
}

// Load the module a described module's import line names. The compiler never
// searches in a described build: the description says where the file is, and
// the import's name is the module's. It is one file, whose bodies this object
// only declares -- a package is built on its own, and imported through its
// include file [Jon 23 Sep]. What the include file itself imports is found where
// the description's package lines say
static ModuleNode *parseLoadBuildImport(ParseState *parse, BuildImport *import) {
    return parseLoadModulePath(parse, import->path, import->name, 0, parseBuildImportModule(import));
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
    return parseLoadModulePath(parse, path, filesym, genflag, NULL);
}

// Load the core package, the prelude every module imports. It is found on the
// package search path and nowhere else, so no file beside a program can stand in
// for it, and like every package found there it is compiled into this object
static ModuleNode *parseLoadCore(ParseState *parse) {
    char *path = fileFindPackage("core");
    if (path == NULL)
        errorExit(ExitNF, "Cannot find the core package, core/src/core.cone or core/core.cone, on the package search path. The packages folder is named by CONE_PACKAGES, else found at or above conec's own folder, else built into the compiler, and '--path' adds folders ahead of it.");
    return parseLoadModulePath(parse, path, nametblFind("core", 4), FlagGenMod, NULL);
}

// Parse a generated include file's text as the module it declares, for the
// generator's self-check (conec.c): a module of its own beside the root it
// stands for, never generated and never added to the program's modules. Its
// imports are answered as the root's are -- by the root's import lines in a
// described build, and from the root's folder otherwise, which 'url' names
ModuleNode *parseIncludeCheck(ProgramNode *pgm, BuildDesc *desc, char *text, char *url) {
    ModuleNode *root = (ModuleNode*)nodesGet(pgm->modules, 0);
    ParseState parse;
    parse.pgm = pgm;
    parse.mod = NULL;
    parse.typenode = NULL;
    parse.inrettype = 0;
    parse.core = NULL;
    parse.build = NULL;
    parse.bodyp = parse.bodyendp = parse.nameendp = NULL;
    parse.typed = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(root->imports, cnt, nodesp)) {
        if (((ImportNode*)*nodesp)->iscore)
            parse.core = ((ImportNode*)*nodesp)->module;
    }
    // Compiling core itself, the include file is core's, and imports nothing
    if (parse.core == root)
        parse.core = NULL;

    BuildModule check;
    if (desc != NULL) {
        memset(&check, 0, sizeof(check));
        check.name = root->namesym;
        check.files = &url;
        check.nfiles = check.availfiles = 1;
        check.imports = desc->root->imports;
        check.nimports = check.availimports = desc->root->nimports;
        parse.build = &check;
    }

    ModuleNode *mod = newModuleNode();
    Lexer *file = lexNew(text, url);
    mod->lexer = file;
    mod->srcp = mod->linep = file->source;
    mod->linenbr = 1;
    mod->filesym = mod->namesym = root->namesym;
    dclInfoJoin((INode*)mod, NULL);
    mod->dclinfo.facts |= DclNamesChain;
    parse.mod = mod;
    parseAddCorelibImport(&parse, mod);
    modHook(NULL, mod);
    if (desc != NULL)
        modAddNamedNode(mod, mod->namesym, (INode*)mod);
    lexPush(file);
    parseGlobalStmts(&parse, mod, 1);
    if (lex->toktype != EofToken)
        errorMsgLex(ErrorNoEof, "Expected end-of-file");
    lexPop();
    modHook(mod, NULL);
    return mod;
}

// Set up the name table and the lexer. A build description is read by the
// lexer, and before generation is set up, so this comes first of all
void parseInit(ConeOptions *opt) {
    nametblInit();
    lexInit(opt);
}

// Parse a program = the main module. 'desc' is the build description the
// compiler was given, or NULL where it was given a source file
ProgramNode *parsePgm(ConeOptions *opt, BuildDesc *desc) {
    typetblInit();
    stdlibInit(opt->ptrsize);

    ProgramNode *pgm = newProgramNode();

    // Initialize parser state
    ParseState parse;
    parse.pgm = pgm;
    parse.mod = NULL;
    parse.typenode = NULL;
    parse.inrettype = 0;
    parse.core = NULL;
    parse.build = NULL;
    parse.bodyp = parse.bodyendp = parse.nameendp = NULL;
    parse.typed = 0;

    // Create module node and set up for parsing main source file.
    // The root's file is registered like any other, so an import loop back to
    // it finds the module already parsed instead of reading the file again as a
    // second module, and the loop is refused naming the root (pgmModuleOrder). It sets no DclNamesChain: the root contributes no prefix, so
    // its declarations are spelled bare -- and naming the root module changes
    // what it is called, never how the program's symbols are spelled. The one
    // exception is a LIBRARY a build description names: a package built on its
    // own is imported by name, so its root is spelled as its importers spell it
    ModuleNode *mod = pgmAddMod(pgm, FlagGenMod);
    SrcFiles files, submodules;
    if (desc != NULL) {
        // A described build: the description names the root and lists its
        // files, and nothing is swept
        mod->filesym = mod->namesym = desc->root->name;
        parseBuildFiles(&files, desc->root, mod);
        parseSrcFilesInit(&submodules);
        if (desc->library) {
            dclInfoJoin((INode*)mod, NULL);
            mod->dclinfo.facts |= DclNamesChain;
        }
    }
    else {
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
        parseModuleFiles(&files, &submodules, path, dsgfile, mod->foldersym != NULL);
    }
    parseRegisterModuleFiles(&parse, mod, &files);

    // Load and parse the core package, auto-imported into main source and, from
    // here on, into every module loaded
    ModuleNode *corelib = parseLoadCore(&parse);
    parse.core = corelib;
    ImportNode *importnode = newImportNode();
    importnode->fold = newFoldClause();
    importnode->fold->star = 1;
    importnode->module = corelib;
    importnode->iscore = 1;
    modAddNode(mod, NULL, (INode*)importnode);

    // Now actually parse the main module's files
    parse.mod = mod;
    modHook(NULL, mod);
    if (mod->foldersym || desc)
        modAddNamedNode(mod, mod->namesym, (INode*)mod);
    // A stray '}' at global scope ends a file's statement loop. Without the
    // end-of-file check inside, the rest of that file would be silently
    // discarded
    if (desc) {
        parse.build = desc->root;
        parseBuildModuleTree(&parse, mod, &files, desc->root);
        parse.build = NULL;
    }
    else
        parseModuleTree(&parse, mod, &files, &submodules);
    modHook(mod, NULL);
    return pgm;
}
