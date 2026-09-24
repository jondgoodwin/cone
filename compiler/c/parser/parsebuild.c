/** Read a build description
 * @file
 *
 * A BUILD DESCRIPTION is what Congo hands the compiler to build one package: the
 * files of each of its modules, where each module's imports are found, and what
 * to produce. Congo walks the folders and reads each file's header; the compiler
 * is told the result and checks it, rather than searching for anything itself.
 * So the folder rules live in one place, Congo [Jon 23 Sep].
 *
 *     build: debug
 *     output: library
 *     q: {
 *         "src/q.cone"
 *         "src/more.cone"
 *         import stdio: "../stdio/stdio.cone"
 *         inner: {
 *             "src/inner.cone"
 *         }
 *     }
 *
 * The settings come first, each at most once: 'build' is 'debug' or 'release',
 * and 'output' is 'executable' or 'library'. Then the package's one module, a
 * name and a braced body holding three kinds of line: a quoted path is a file of
 * this module, 'name: { ... }' is a child module, and 'import name: "path"' says
 * where this module's 'import name' is found. A relative path is relative to the
 * description's own folder.
 *
 * The file is read by the compiler's own lexer, so a comment is a Cone comment,
 * and a path is a Cone string: a backslash begins an escape, so a path is
 * written with '/' (or '\\').
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "lexer.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../shared/fileio.h"
#include "../coneopts.h"

#include <string.h>

// The extension that marks a source path as a build description
#define BuildDescExt ".conebuild"

// The description's own folder, with its trailing slash, or "" for the current
// directory. A relative path the description writes is relative to it
static char *buildFolder;

int parseIsBuildDesc(char *path) {
    char *ext = fileExtPos(path);
    return ext && strcmp(ext, BuildDescExt) == 0;
}

static BuildModule *newBuildModule(Name *name) {
    BuildModule *mod = (BuildModule*)memAllocBlk(sizeof(BuildModule));
    memset(mod, 0, sizeof(BuildModule));
    mod->name = name;
    return mod;
}

// Make room for one more entry in a growing array of 'size'-byte entries
static void *buildGrow(void *items, uint32_t count, uint32_t *avail, size_t size) {
    if (count < *avail)
        return items;
    *avail = *avail ? *avail * 2 : 4;
    void *grown = memAllocBlk(*avail * size);
    if (count)
        memcpy(grown, items, count * size);
    return grown;
}

// A path as the description writes it, in the one spelling the file registry
// is keyed by. A relative one is relative to the description's folder
static char *buildPath(char *written) {
    int absolute = written[0] == '/' || written[0] == '\\' || (written[0] && written[1] == ':');
    return fileCanonicalPath(absolute ? written : parsePathJoin(buildFolder, written));
}

BuildImport *parseBuildFindImport(BuildModule *build, Name *name) {
    for (uint32_t i = 0; i < build->nimports; ++i) {
        if (build->imports[i].name == name)
            return &build->imports[i];
    }
    return NULL;
}

BuildModule *parseBuildImportModule(BuildImport *import) {
    BuildModule *mod = newBuildModule(import->name);
    mod->isimport = 1;
    return mod;
}

static BuildModule *buildFindChild(BuildModule *build, Name *name) {
    for (uint32_t i = 0; i < build->nchildren; ++i) {
        if (build->children[i]->name == name)
            return build->children[i];
    }
    return NULL;
}

// Pass over what a malformed line took, so one mistake is one diagnostic: up to
// the next token that can begin a line, or the '}' that ends the module
static void buildSkipLine() {
    lexNextToken();
    while (!lexIsToken(EofToken) && !lexIsToken(RCurlyToken) && !lexIsToken(StringLitToken)
        && !lexIsToken(ImportToken) && !lexIsToken(IdentToken))
        lexNextToken();
}

static void buildParseBody(BuildModule *mod);

// 'import name: "path"', with the lexer on 'import'
static void buildParseImport(BuildModule *mod) {
    lexNextToken();
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorBuildDesc, "Expected the name this module's 'import' is written with: 'import name: \"path\"'.");
        buildSkipLine();
        return;
    }
    Name *name = lex->val.ident;
    lexNextToken();
    if (!lexIsToken(ColonToken)) {
        errorMsgLex(ErrorBuildDesc, "Expected ':' after 'import %s', and then the quoted path of the file it names.", &name->namestr);
        buildSkipLine();
        return;
    }
    lexNextToken();
    if (!lexIsToken(StringLitToken)) {
        errorMsgLex(ErrorBuildDesc, "Expected the quoted path of the file 'import %s' names.", &name->namestr);
        buildSkipLine();
        return;
    }
    if (parseBuildFindImport(mod, name)) {
        errorMsgLex(ErrorBuildDesc, "Module %s says where 'import %s' is found twice: a module imports a name once.",
            &mod->name->namestr, &name->namestr);
        lexNextToken();
        return;
    }
    mod->imports = (BuildImport*)buildGrow(mod->imports, mod->nimports, &mod->availimports, sizeof(BuildImport));
    mod->imports[mod->nimports].name = name;
    mod->imports[mod->nimports++].path = buildPath(lex->val.strlit);
    lexNextToken();
}

// A module's body, with the lexer just past its '{', up to and past its '}'
static void buildParseBody(BuildModule *mod) {
    while (!lexIsToken(RCurlyToken) && !lexIsToken(EofToken)) {
        if (lexIsToken(StringLitToken)) {
            mod->files = (char**)buildGrow(mod->files, mod->nfiles, &mod->availfiles, sizeof(char*));
            mod->files[mod->nfiles++] = buildPath(lex->val.strlit);
            lexNextToken();
        }
        else if (lexIsToken(ImportToken))
            buildParseImport(mod);
        else if (lexIsToken(IdentToken)) {
            Name *name = lex->val.ident;
            lexNextToken();
            if (!lexIsToken(ColonToken) || (lexNextToken(), !lexIsToken(LCurlyToken))) {
                errorMsgLex(ErrorBuildDesc, "Expected ': {' after %s, opening the child module's files: 'name: { \"file.cone\" }'.",
                    &name->namestr);
                buildSkipLine();
                continue;
            }
            if (buildFindChild(mod, name) != NULL)
                errorMsgLex(ErrorBuildDesc, "Module %s already has a child module named %s.",
                    &mod->name->namestr, &name->namestr);
            lexNextToken();
            BuildModule *child = newBuildModule(name);
            buildParseBody(child);
            mod->children = (BuildModule**)buildGrow(mod->children, mod->nchildren, &mod->availchildren, sizeof(BuildModule*));
            mod->children[mod->nchildren++] = child;
        }
        else {
            errorMsgLex(ErrorBuildDesc, "Expected a quoted file path, an 'import name: \"path\"' line, or a child module 'name: { ... }'.");
            buildSkipLine();
        }
    }
    if (!lexIsToken(RCurlyToken)) {
        errorMsgLex(ErrorBuildDesc, "Expected '}' to close module %s.", &mod->name->namestr);
        return;
    }
    // A module is its files: one listing none has nothing to be
    if (mod->nfiles == 0)
        errorMsgLex(ErrorBuildDesc, "Module %s lists no source file: a module's files are written in its braces as quoted paths.",
            &mod->name->namestr);
    lexNextToken();
}

// A setting, 'name: value', with the lexer on its value. 'seen' is whether it
// was written already
static void buildSetting(ConeOptions *opt, BuildDesc *desc, Name *name, int *seenbuild, int *seenoutput) {
    char *setting = &name->namestr;
    int isbuild = strcmp(setting, "build") == 0;
    int isoutput = strcmp(setting, "output") == 0;
    if (!isbuild && !isoutput) {
        errorMsgLex(ErrorBuildDesc, "%s is not a setting of a build description: its settings are 'build' and 'output'.", setting);
        buildSkipLine();
        return;
    }
    if (desc->root != NULL)
        errorMsgLex(ErrorBuildDesc, "A setting comes before the package's module, at the top of the description.");
    int *seen = isbuild ? seenbuild : seenoutput;
    if (*seen)
        errorMsgLex(ErrorBuildDesc, "'%s' is set twice.", setting);
    *seen = 1;
    char *value = lexIsToken(IdentToken) ? &lex->val.ident->namestr : "";
    if (isbuild && strcmp(value, "debug") == 0)
        opt->release = 0;
    else if (isbuild && strcmp(value, "release") == 0)
        opt->release = 1;
    else if (isoutput && strcmp(value, "executable") == 0)
        desc->library = 0;
    else if (isoutput && strcmp(value, "library") == 0)
        desc->library = 1;
    else {
        errorMsgLex(ErrorBuildDesc, isbuild ? "'build' is 'debug' or 'release'." : "'output' is 'executable' or 'library'.");
        buildSkipLine();
        return;
    }
    lexNextToken();
}

BuildDesc *parseBuildDesc(ConeOptions *opt) {
    buildFolder = memAllocStr(opt->srcpath, fileFolder(opt->srcpath));
    BuildDesc *desc = (BuildDesc*)memAllocBlk(sizeof(BuildDesc));
    desc->root = NULL;
    // '--library' is the default an 'output' line overrides, as 'build'
    // overrides '--release' and '--debug'
    desc->library = opt->library;
    int seenbuild = 0, seenoutput = 0;

    lexInjectPath(opt->srcpath);
    while (!lexIsToken(EofToken)) {
        if (!lexIsToken(IdentToken)) {
            errorMsgLex(ErrorBuildDesc, "Expected a setting, 'build: ...' or 'output: ...', or the package's module, 'name: { ... }'.");
            buildSkipLine();
            continue;
        }
        Name *name = lex->val.ident;
        lexNextToken();
        if (!lexIsToken(ColonToken)) {
            errorMsgLex(ErrorBuildDesc, "Expected ':' after %s.", &name->namestr);
            buildSkipLine();
            continue;
        }
        lexNextToken();
        if (!lexIsToken(LCurlyToken)) {
            buildSetting(opt, desc, name, &seenbuild, &seenoutput);
            continue;
        }
        // The package's module: one per description, since a description
        // builds one package
        if (desc->root != NULL)
            errorMsgLex(ErrorBuildDesc, "A build description describes one package, and its module is %s already.",
                &desc->root->name->namestr);
        lexNextToken();
        BuildModule *mod = newBuildModule(name);
        buildParseBody(mod);
        if (desc->root == NULL)
            desc->root = mod;
    }
    if (desc->root == NULL)
        errorMsgLex(ErrorBuildDesc, "The build description names no package module: 'name: { \"file.cone\" }'.");
    lexPop();
    // A library is compiled as one: position-independent, since it may be
    // linked into a position-independent executable or a shared library, and
    // exporting its public definitions (genlIsExported). Generation's setup,
    // which reads the first, runs after this
    opt->library = desc->library;
    return desc;
}
