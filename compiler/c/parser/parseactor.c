/** Parse an actor declaration, and generate what it stands for
 *
 * 'actor Counter { count u64; fn init(self &new, n u64) {...}; pub fn bump(self,
 * n u64) {...} }' declares an actor: state that lives on, reached by nothing
 * but messages, each a call of one of its 'pub' methods, run later, one at a
 * time, on whichever worker thread of the actors package takes it up. No node
 * stands for an actor. The parser turns its declaration into the four
 * declarations the actors package runs an actor by, generated as Cone source
 * and parsed here, in this order:
 *
 *   enum Counter.Msg is Sendable {struct bump {n u64;}}     one variant per message
 *   struct Counter.State {count u64; fn init...; fn bump...} the body as written
 *   struct Counter {                                         the handle
 *     mailbox Arc[opaq, actors.Mailbox[Counter.Msg]];
 *     fn init(self &new, n u64) {*self = new Self(mailbox:
 *       actors.startActor[Counter.State, Counter.Msg](new Counter.State(n), &Counter.dispatch));}
 *     pub fn bump(self &, n u64) {actors.send[Counter.Msg](mailbox, Counter.Msg.bump[n]);}
 *   }
 *   fn Counter.dispatch(self &mut Counter.State, msg *Counter.Msg) {
 *     if &*msg is &Counter.Msg.bump {imm p = msg as *Counter.Msg.bump; self.bump(actors.take(&(*p).n));}
 *   }
 *
 * and, once per module, the helper 'actors.take' (a function of the module's
 * own, named as no source can spell), which moves a field out of a message:
 * 'fn actors.take[T](p &T) T {mem.readRaw[T](p as *T);}'.
 *
 * The derived names -- 'Counter.State', 'Counter.Msg', 'Counter.dispatch',
 * the handle's one field, and the bindings of the actors and sync packages the
 * generated text reaches them through -- are names no source can spell
 * (nametblPrivate): the text writes each as '`#n`', which only a generated
 * source reads (Lexer.gennames). So nothing outside the actor names its state,
 * its messages or its mailbox, however it is written, and a diagnostic reads
 * them as 'Counter.State'.
 *
 * Copied text: each message's and each initializer's parameters are written
 * again in the variant and the handle's method, as the author wrote their
 * types and default values (DclText, recorded while the body was read). A
 * diagnostic against generated text is reported at the actor's name, with the
 * generated line beside it (Lexer.genat).
 *
 * The order is what keeps one actor from meeting the order-dependent refusal of
 * a type still being laid out (ErrorNoSize): the message enum holds handles by
 * value, and a handle reaches the enum again through Mailbox's type argument,
 * so the enum is declared first, before the state and the handle.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../ir/nametbl.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>

// What the generated text spells '`#n`'. The first six are the same in every
// actor of every module; the last three are each actor's own
enum GenSlot {
    GenActors,      // The actors package, as this module imports it
    GenSync,        // The sync package, whose Arc counts a handle's owners
    GenSendable,    // The built-in marker the message enum declares
    GenTake,        // The module's helper that moves a message's field out of its node
    GenMailbox,     // The handle's one field
    GenNone,        // The one variant of the message enum of an actor with no messages
    GenState,       // 'Counter.State'
    GenMsg,         // 'Counter.Msg'
    GenDispatch,    // 'Counter.dispatch'
    GenSlots
};

static Name *genShared[GenState];

// ---- Text the declaration's members were written with ----------------------

void parseDclText(ParseState *parse, INode *dcl, char *type, char *typeend, char *value, char *valueend) {
    DclTexts *texts = parse->dcltexts;
    if (texts->count == texts->avail) {
        uint32_t avail = texts->avail ? texts->avail * 2 : 16;
        DclText *grown = (DclText *)memAllocBlk(avail * sizeof(DclText));
        if (texts->count)
            memcpy(grown, texts->items, texts->count * sizeof(DclText));
        texts->items = grown;
        texts->avail = avail;
    }
    DclText *text = &texts->items[texts->count++];
    text->dcl = dcl;
    text->type = type;
    text->typeend = typeend;
    text->value = value;
    text->valueend = valueend;
}

static DclText *parseActorText(DclTexts *texts, INode *dcl) {
    for (uint32_t i = 0; i < texts->count; ++i) {
        if (texts->items[i].dcl == dcl)
            return &texts->items[i];
    }
    return NULL;
}

// Does the text hold 'word' as a name of its own?
static int parseActorTextHas(char *from, char *to, char *word) {
    size_t len = strlen(word);
    for (char *p = from; p + len <= to; ++p) {
        if (strncmp(p, word, len) != 0)
            continue;
        int before = p > from && (p[-1] == '_' || (p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9'));
        char after = p + len < to ? p[len] : ' ';
        int follows = after == '_' || (after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') || (after >= '0' && after <= '9');
        if (!before && !follows)
            return 1;
    }
    return 0;
}

// ---- The generated text ------------------------------------------------------

typedef struct GenText {
    char *text;
    size_t len;
    size_t avail;
} GenText;

static void genPutn(GenText *g, const char *s, size_t n) {
    if (g->len + n + 1 > g->avail) {
        size_t avail = g->avail ? g->avail : 1024;
        while (avail < g->len + n + 1)
            avail *= 2;
        char *grown = (char *)memAllocBlk(avail);
        if (g->len)
            memcpy(grown, g->text, g->len);
        g->text = grown;
        g->avail = avail;
    }
    memcpy(g->text + g->len, s, n);
    g->len += n;
    g->text[g->len] = '\0';
}

static void genPuts(GenText *g, const char *s) {
    genPutn(g, s, strlen(s));
}

// A name the author wrote, back-ticked so that it is read as a name whatever it is
static void genName(GenText *g, Name *name) {
    genPuts(g, "`");
    genPutn(g, &name->namestr, name->namesz);
    genPuts(g, "`");
}

// A name no source can spell
static void genSlot(GenText *g, enum GenSlot slot) {
    char buf[16];
    snprintf(buf, sizeof(buf), "`#%d`", (int)slot);
    genPuts(g, buf);
}

// A parameter as the author wrote it: its name, its type, and its default
static void genParm(GenText *g, DclText *text, Name *name) {
    genName(g, name);
    genPuts(g, " ");
    genPutn(g, text->type, text->typeend - text->type);
    if (text->value) {
        genPuts(g, " = ");
        genPutn(g, text->value, text->valueend - text->value);
    }
}

// ---- What the body may hold --------------------------------------------------

static PermNode *parseActorPerm(RefNode *ref) {
    INode *perm = ref->perm;
    if (perm == NULL || perm->tag == UnknownTag)
        return roPerm;
    return (PermNode *)itypeGetTypeDcl(perm);
}

// Is this type, as parsed, a borrowed reference, which may never leave its
// thread? A function reference is not one: '&fn' is a function's address
static int parseActorIsBorrow(INode *type) {
    if (type->tag != RefTag && type->tag != ArrayRefTag && type->tag != VirtRefTag)
        return 0;
    RefNode *ref = (RefNode *)type;
    if (ref->region != borrowRef)
        return 0;
    return ref->vtexp == NULL || ref->vtexp->tag != FnSigTag;
}

static int parseActorIsInit(FnDclNode *fn) {
    return fn->namesym == initName || fn->overloadsym == initName;
}

// Check a method's 'self': the state, lent to the method for the message it
// handles. Written bare it is the state borrowed 'mut', which the method has
// alone while it runs; '&' and '&mut' are taken as written; an initializer's
// is '&new' and the finalizer's '&uni', as any type's are.
static int parseActorSelf(StructNode *state, FnDclNode *fn) {
    VarDclNode *self = (VarDclNode *)nodesGet(((FnSigNode *)fn->vtype)->parms, 0);
    INode *type = self->vtype;
    if (isNameUseNode(type) && ((NameUseNode *)type)->namesym == selfTypeName) {
        self->vtype = (INode *)newRefNodeFull(RefTag, (INode *)self, borrowRef, newPermUseNode(mutPerm), type);
        return 1;
    }
    if (type->tag == RefTag && ((RefNode *)type)->region == borrowRef) {
        PermNode *perm = parseActorPerm((RefNode *)type);
        if (perm == mutPerm || perm == roPerm)
            return 1;
        if (perm == newPerm && parseActorIsInit(fn))
            return 1;
        if (perm == uniPerm && fn->namesym == finalName)
            return 1;
    }
    errorMsgNode((INode *)self, ErrorActorMember,
        "%s's 'self' is actor %s's state, lent to the method for the message it handles: write 'self', which has it 'mut', or 'self &' to only read it (an initializer's is 'self &new', the finalizer's 'self &uni').",
        &fn->namesym->namestr, &state->namesym->namestr);
    return 0;
}

// Check the parameters of a message or an initializer, which cross to the
// actor's thread, and record each for the thread check (actorCheckAll). What
// cannot cross whatever it turns out to be -- a borrow, a lifetime, the
// state itself -- is refused here. 'crossing' takes the function and each
// parameter in pairs. Returns whether all of them can be written again.
static int parseActorParms(DclTexts *texts, StructNode *state, Name *actorname, FnDclNode *fn, Nodes **crossing) {
    int ok = 1;
    int isinit = parseActorIsInit(fn);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(((FnSigNode *)fn->vtype)->parms, cnt, nodesp)) {
        VarDclNode *parm = (VarDclNode *)*nodesp;
        if (parm->namesym == selfName)
            continue;
        DclText *text = parseActorText(texts, (INode *)parm);
        char *crossesto = isinit
            ? "An actor's state is made from its initializer's arguments and then run on other threads"
            : "A message carries its arguments to the thread the actor runs on";
        if (text == NULL || text->type == NULL
            || parseActorTextHas(text->type, text->typeend, "Self")) {
            errorMsgNode((INode *)parm, ErrorNotSendable,
                "Actor %s's %s %s takes %s, the actor's own state, which never leaves the actor. %s.",
                &actorname->namestr, isinit ? "initializer" : "message", &fn->namesym->namestr,
                &parm->namesym->namestr, crossesto);
            ok = 0;
            continue;
        }
        if (parseActorIsBorrow(parm->vtype) || memchr(text->type, '\'', text->typeend - text->type)) {
            errorMsgNode((INode *)parm, ErrorNotSendable,
                "Actor %s's %s %s takes %s, a borrowed reference, which is not Sendable: no borrow may leave its thread. %s; pass an owner that may cross, such as a 'uni' one, or an 'Arc'.",
                &actorname->namestr, isinit ? "initializer" : "message", &fn->namesym->namestr,
                &parm->namesym->namestr, crossesto);
            ok = 0;
            continue;
        }
        nodesAdd(crossing, (INode *)fn);
        nodesAdd(crossing, (INode *)parm);
    }
    return ok;
}

// The members of an actor's body: which are messages, which initializers, and
// what may not be there at all
typedef struct ActorMembers {
    Nodes *messages;     // The 'pub' methods, each a message
    Nodes *inits;        // Its initializers, 'init' and those joining the overload name
    Nodes *crossing;     // Each argument that crosses to the actor (actorRegister)
    uint32_t declared;   // How many initializers it declares, refused ones too
} ActorMembers;

static void parseActorMembers(DclTexts *texts, StructNode *state, Name *actorname, ActorMembers *members) {
    INode **nodesp;
    uint32_t cnt;

    // Its fields are its private state
    for (nodelistFor(&state->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        if (field->flags & IsMixin)
            continue;
        if (field->flags & FlagPub)
            errorMsgNode((INode *)field, ErrorActorMember,
                "%s is part of actor %s's state, which only its own methods reach: a field of an actor is never 'pub'. Send the actor a message that uses it.",
                &field->namesym->namestr, &actorname->namestr);
        if (field->fold)
            errorMsgNode(field->fold->at, ErrorActorMember,
                "An actor's field does not fold names in: nothing outside the actor reaches its state, so there is nothing to fold them for.");
    }
    if (state->siblings)
        errorMsgNode(nodesGet(state->siblings, 0), ErrorActorMember,
            "An actor's body does not fold in a sibling: an actor has no base to share.");

    for (nodelistFor(&state->nodelist, cnt, nodesp)) {
        INode *node = *nodesp;
        if (node->tag == MacroDclTag) {
            errorMsgNode(node, ErrorActorMember, "An actor's body does not declare a macro yet.");
            continue;
        }
        if (node->tag == VarDclTag) {
            errorMsgNode(node, ErrorActorMember,
                "An actor has no 'static': a variable every actor of the kind shared would be reached from every worker thread at once. Keep it in the actor's fields, or in a global made for sharing.");
            continue;
        }
        if (node->tag != FnDclTag)
            continue;
        FnDclNode *fn = (FnDclNode *)node;
        if (fn->flags & FlagExtern) {
            errorMsgNode(node, ErrorActorMember, "An actor's method is written with its body: 'extern' declares one defined elsewhere, which an actor's never is.");
            continue;
        }
        // A function without 'self' is a helper of the state's, reached from its
        // methods; it has no state to run on, so it is no message
        if (!(fn->flags & FlagMethFld)) {
            if (fn->flags & FlagPub)
                errorMsgNode(node, ErrorActorMember,
                    "%s has no 'self', so it would not run on actor %s: only a method is a message, and an actor is made with 'new'. Declare a function that needs no actor outside it.",
                    &fn->namesym->namestr, &actorname->namestr);
            continue;
        }
        if (!parseActorSelf(state, fn))
            continue;
        if (fn->namesym == finalName)
            continue;
        if (parseActorIsInit(fn)) {
            ++members->declared;
            if (parseActorParms(texts, state, actorname, fn, &members->crossing))
                nodesAdd(&members->inits, node);
            continue;
        }
        if (!(fn->flags & FlagPub))
            continue;

        // A message
        int ok = 1;
        FnSigNode *sig = (FnSigNode *)fn->vtype;
        if (sig->rettype->tag != VoidTag) {
            errorMsgNode(sig->rettype, ErrorActorReturn,
                "Actor %s's message %s returns nothing: a send queues the message and returns at once, before the method runs. To answer, send a message back, to a handle passed along.",
                &actorname->namestr, &fn->namesym->namestr);
            ok = 0;
        }
        if (fn->genericinfo) {
            errorMsgNode(node, ErrorActorMember,
                "Actor %s's message %s is generic, which a message cannot be yet: each message is one variant of the actor's message type.",
                &actorname->namestr, &fn->namesym->namestr);
            ok = 0;
        }
        VarDclNode *self = (VarDclNode *)nodesGet(sig->parms, 0);
        PermNode *selfperm = parseActorPerm((RefNode *)self->vtype);
        if (selfperm != mutPerm && selfperm != roPerm) {
            errorMsgNode((INode *)self, ErrorActorMember,
                "A message's 'self' is the actor's state, lent for the message: 'self', 'self &' or 'self &mut'.");
            ok = 0;
        }
        if (parseActorParms(texts, state, actorname, fn, &members->crossing) && ok)
            nodesAdd(&members->messages, node);
    }
}

// What the declaration itself may not say
static void parseActorHeader(StructNode *state, Name *actorname) {
    if (state->genericinfo || state->lifeparms)
        errorMsgNode((INode *)state, ErrorActorMember,
            "Actor %s takes type parameters, and a generic actor is not built yet.", &actorname->namestr);
    if (state->basetrait || state->extendsbase) {
        errorMsgNode((INode *)state, ErrorActorMember,
            "Actor %s names an abstraction or a base, which an actor does not have yet: 'actor trait' is not built.",
            &actorname->namestr);
        return;
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&state->fields, cnt, nodesp)) {
        if ((*nodesp)->flags & IsMixin) {
            errorMsgNode((INode *)state, ErrorActorMember,
                "Actor %s names an abstraction or a base, which an actor does not have yet: 'actor trait' is not built.",
                &actorname->namestr);
            return;
        }
    }
    if (state->flags & DeclaredOpaque)
        errorMsgNode((INode *)state, ErrorActorMember,
            "Actor %s is '@opaque', which says nothing of an actor: its state is reached by nothing outside it already.",
            &actorname->namestr);
}

// ---- The runtime it runs on --------------------------------------------------

static ModuleNode *parseActorFindImport(Nodes *imports, char *name) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(imports, cnt, nodesp)) {
        ModuleNode *mod = ((ImportNode *)*nodesp)->module;
        if (mod && strcmp(&mod->namesym->namestr, name) == 0)
            return mod;
    }
    return NULL;
}

// Bind a name no source can spell to a declaration of another module, as an
// import binds a module's name: privately, the binding's target resolved here
static void parseActorBind(ModuleNode *mod, Name *name, INode *dcl, uint16_t flags) {
    NameUseNode *target = newNameUseNode(name);
    inodeLexCopy((INode *)target, (INode *)mod);
    target->dclnode = dcl;
    AliasDclNode *alias = newNameAliasDclNode(name, (INode *)target);
    inodeLexCopy((INode *)alias, (INode *)mod);
    alias->flags |= flags;
    modAddNamedNode(mod, name, (INode *)alias);
}

// The actors package this module imports, and what the generated text needs of
// it, bound once per module: 0 where it is not there, 1 where the module's
// first actor bound it already, 2 where it is bound now and the helper all its
// actors share is the generated text's first declaration
static int parseActorRuntime(ParseState *parse, StructNode *at, Name *actorname, GenText *g) {
    ModuleNode *mod = parse->mod;
    if (genShared[GenActors] == NULL) {
        genShared[GenActors] = nametblPrivate("actors", 6);
        genShared[GenSync] = nametblPrivate("sync", 4);
        genShared[GenSendable] = nametblPrivate("Sendable", 8);
        genShared[GenTake] = nametblPrivate("actors.take", 11);
        genShared[GenMailbox] = nametblPrivate("mailbox", 7);
        genShared[GenNone] = nametblPrivate("none", 4);
    }
    if (namespaceFind(&mod->namespace, genShared[GenActors]))
        return 1;

    ModuleNode *actorsmod = parseActorFindImport(mod->imports, "actors");
    if (actorsmod == NULL) {
        errorMsgNode((INode *)at, ErrorActorRuntime,
            "Actor %s runs on the actors package, which module %s does not import: write 'import actors;' after the 'mod' line.",
            &actorname->namestr, &mod->namesym->namestr);
        return 0;
    }
    ModuleNode *syncmod = parseActorFindImport(actorsmod->imports, "sync");
    if (syncmod == NULL || namespaceFind(&actorsmod->namespace, nametblFind("startActor", 10)) == NULL) {
        errorMsgNode((INode *)at, ErrorActorRuntime,
            "Actor %s runs on the actors package, and the module %s imports as actors is not that package: it has no startActor, or does not import sync.",
            &actorname->namestr, &mod->namesym->namestr);
        return 0;
    }
    parseActorBind(mod, genShared[GenActors], (INode *)actorsmod, FlagImportName);
    parseActorBind(mod, genShared[GenSync], (INode *)syncmod, FlagImportName);
    parseActorBind(mod, genShared[GenSendable], (INode *)sendableTrait, 0);

    // The dispatch function moves each argument out of the message where it
    // lies in its node, which is then freed without being finalized: so every
    // field is moved out exactly once, a field that may only move as well as
    // one that is copied
    genPuts(g, "fn ");
    genSlot(g, GenTake);
    genPuts(g, "[T](p &T) T {mem.readRaw[T](p as *T);}\n");
    return 2;
}

// ---- The declaration ---------------------------------------------------------

void parseActor(ParseState *parse, uint16_t pubflag) {
    ModuleNode *mod = parse->mod;

    // 'actor trait', the abstraction of an actor, is admitted and not built
    if (lexNextIsWord("trait")) {
        errorMsgLex(ErrorUnbuiltKind,
            "'actor trait' is the abstraction of an actor, and the compiler does not build it yet.");
        lexNextToken();
        lexNextToken();
        if (lexIsToken(IdentToken))
            lexNextToken();
        parseSkipDclBody();
        return;
    }

    // The body, read as a struct's, is the state. Where each field's and
    // parameter's type and default are written is kept, to be written again
    DclTexts texts = {NULL, 0, 0};
    DclTexts *svtexts = parse->dcltexts;
    parse->dcltexts = &texts;
    StructNode *state = (StructNode *)parseStruct(parse, 0);
    parse->dcltexts = svtexts;
    Name *actorname = state->namesym;
    if (actorname == anonName)
        return;

    parseActorHeader(state, actorname);
    ActorMembers members;
    members.messages = newNodes(8);
    members.inits = newNodes(2);
    members.crossing = newNodes(8);
    members.declared = 0;
    parseActorMembers(&texts, state, actorname, &members);

    GenText g = {NULL, 0, 0};
    genPuts(&g, "");
    int runtime = parseActorRuntime(parse, state, actorname, &g);
    if (runtime == 0)
        return;

    // The names this actor's declarations are known by
    Name *names[GenSlots];
    for (int i = 0; i < GenState; ++i)
        names[i] = genShared[i];
    char buf[300];
    snprintf(buf, sizeof(buf), "%s.State", &actorname->namestr);
    names[GenState] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.Msg", &actorname->namestr);
    names[GenMsg] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.dispatch", &actorname->namestr);
    names[GenDispatch] = nametblPrivate(buf, strlen(buf));
    state->namesym = names[GenState];

    INode **nodesp;
    uint32_t cnt;

    // The message enum: a variant per message, its fields the parameters
    genPuts(&g, "enum ");
    genSlot(&g, GenMsg);
    genPuts(&g, " is ");
    genSlot(&g, GenSendable);
    genPuts(&g, " {\n");
    if (members.messages->used == 0) {
        genPuts(&g, "  ");
        genSlot(&g, GenNone);
        genPuts(&g, ";\n");
    }
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        genPuts(&g, "  struct ");
        genName(&g, fn->namesym);
        genPuts(&g, " {");
        INode **parmp;
        uint32_t parmcnt;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            DclText *text = parseActorText(&texts, (INode *)parm);
            genName(&g, parm->namesym);
            genPuts(&g, " ");
            genPutn(&g, text->type, text->typeend - text->type);
            genPuts(&g, "; ");
        }
        genPuts(&g, "}\n");
    }
    genPuts(&g, "}\n");

    // The handle: its one field, an initializer for each of the state's, and
    // a method per message, which sends it
    genPuts(&g, "struct ");
    genName(&g, actorname);
    genPuts(&g, " {\n  ");
    genSlot(&g, GenMailbox);
    genPuts(&g, " ");
    genSlot(&g, GenSync);
    genPuts(&g, ".Arc[opaq, ");
    genSlot(&g, GenActors);
    genPuts(&g, ".Mailbox[");
    genSlot(&g, GenMsg);
    genPuts(&g, "]];\n");
    if (members.declared == 0) {
        // The state's implicit initializer: its fields with their types written,
        // in order, the rest taking their defaults. Public where nothing private
        // is set by it: a pub actor with no fields
        int hasfields = 0;
        for (nodelistFor(&state->fields, cnt, nodesp))
            hasfields |= !((*nodesp)->flags & IsMixin);
        genPuts(&g, (pubflag && !hasfields) ? "  pub fn init(self &new" : "  fn init(self &new");
        for (nodelistFor(&state->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            DclText *text = parseActorText(&texts, (INode *)field);
            if ((field->flags & IsMixin) || text == NULL || text->type == NULL)
                continue;
            genPuts(&g, ", ");
            genParm(&g, text, field->namesym);
        }
        genPuts(&g, ") {*self = new Self(");
        genSlot(&g, GenMailbox);
        genPuts(&g, ": ");
        genSlot(&g, GenActors);
        genPuts(&g, ".startActor[");
        genSlot(&g, GenState);
        genPuts(&g, ", ");
        genSlot(&g, GenMsg);
        genPuts(&g, "](new ");
        genSlot(&g, GenState);
        genPuts(&g, "(");
        int first = 1;
        for (nodelistFor(&state->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            DclText *text = parseActorText(&texts, (INode *)field);
            if ((field->flags & IsMixin) || text == NULL || text->type == NULL)
                continue;
            if (!first)
                genPuts(&g, ", ");
            first = 0;
            genName(&g, field->namesym);
            genPuts(&g, ": ");
            genName(&g, field->namesym);
        }
        genPuts(&g, "), &");
        genSlot(&g, GenDispatch);
        genPuts(&g, "));}\n");
    }
    for (nodesFor(members.inits, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        genPuts(&g, (fn->flags & FlagPub) ? "  pub fn " : "  fn ");
        genName(&g, fn->namesym);
        if (fn->overloadsym) {
            genPuts(&g, " overload ");
            genName(&g, fn->overloadsym);
        }
        genPuts(&g, "(self &new");
        INode **parmp;
        uint32_t parmcnt;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            genPuts(&g, ", ");
            genParm(&g, parseActorText(&texts, (INode *)parm), parm->namesym);
        }
        genPuts(&g, ") {*self = new Self(");
        genSlot(&g, GenMailbox);
        genPuts(&g, ": ");
        genSlot(&g, GenActors);
        genPuts(&g, ".startActor[");
        genSlot(&g, GenState);
        genPuts(&g, ", ");
        genSlot(&g, GenMsg);
        genPuts(&g, "](new ");
        genSlot(&g, GenState);
        genPuts(&g, "(");
        int first = 1;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            if (!first)
                genPuts(&g, ", ");
            first = 0;
            genName(&g, parm->namesym);
        }
        genPuts(&g, "), &");
        genSlot(&g, GenDispatch);
        genPuts(&g, "));}\n");
    }
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        genPuts(&g, "  pub fn ");
        genName(&g, fn->namesym);
        if (fn->overloadsym) {
            genPuts(&g, " overload ");
            genName(&g, fn->overloadsym);
        }
        genPuts(&g, "(self &");
        INode **parmp;
        uint32_t parmcnt;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            genPuts(&g, ", ");
            genParm(&g, parseActorText(&texts, (INode *)parm), parm->namesym);
        }
        genPuts(&g, ") {");
        genSlot(&g, GenActors);
        genPuts(&g, ".send[");
        genSlot(&g, GenMsg);
        genPuts(&g, "](");
        genSlot(&g, GenMailbox);
        genPuts(&g, ", ");
        genSlot(&g, GenMsg);
        genPuts(&g, ".");
        genName(&g, fn->namesym);
        genPuts(&g, "[");
        int first = 1;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            if (!first)
                genPuts(&g, ", ");
            first = 0;
            genName(&g, parm->namesym);
        }
        genPuts(&g, "]);}\n");
    }
    genPuts(&g, "}\n");

    // The dispatch function: the message where it lies in its node, each
    // argument moved out of it into the method's call
    genPuts(&g, "fn ");
    genSlot(&g, GenDispatch);
    genPuts(&g, "(self &mut ");
    genSlot(&g, GenState);
    genPuts(&g, ", msg *");
    genSlot(&g, GenMsg);
    genPuts(&g, ") {\n");
    int firstarm = 1;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        genPuts(&g, firstarm ? "  if &*msg is &" : "  elif &*msg is &");
        firstarm = 0;
        genSlot(&g, GenMsg);
        genPuts(&g, ".");
        genName(&g, fn->namesym);
        genPuts(&g, " {imm p = msg as *");
        genSlot(&g, GenMsg);
        genPuts(&g, ".");
        genName(&g, fn->namesym);
        genPuts(&g, "; self.");
        genName(&g, fn->namesym);
        genPuts(&g, "(");
        int first = 1;
        INode **parmp;
        uint32_t parmcnt;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            if (!first)
                genPuts(&g, ", ");
            first = 0;
            genSlot(&g, GenTake);
            genPuts(&g, "(&(*p).");
            genName(&g, parm->namesym);
            genPuts(&g, ")");
        }
        genPuts(&g, ");}\n");
    }
    genPuts(&g, "}\n");

    // Parse it, as the actor's own source, its diagnostics reported at its name
    Lexer *gen = lexNew(g.text, lex->url);
    gen->genat = (INode *)state;
    gen->gennames = (Name **)memAllocBlk(sizeof(names));
    memcpy(gen->gennames, names, sizeof(names));
    gen->ngennames = GenSlots;
    uint32_t firstnode = mod->nodes->used;
    lexPush(gen);
    if (runtime == 2)
        parseFnOrVar(parse, 0);
    StructNode *msg = (StructNode *)parseStruct(parse, TraitType | SameSize | EnumType);
    modAddNode(mod, names[GenMsg], (INode *)msg);
    modAddNode(mod, names[GenState], (INode *)state);
    StructNode *handle = (StructNode *)parseStruct(parse, pubflag);
    modAddNode(mod, handle->namesym, (INode *)handle);
    parseFnOrVar(parse, 0);
    if (!lexIsToken(EofToken))
        errorMsgLex(ErrorNoEof, "The declarations generated for actor %s did not parse whole.", &actorname->namestr);
    lexPop();

    // Every declaration generated here, exported by a library compile whatever
    // its visibility (dclIsExported), since an importer generates them again
    for (uint32_t i = firstnode; i < mod->nodes->used; ++i) {
        INode *node = nodesGet(mod->nodes, i);
        DclInfo *dclinfo = inodeGetDclInfo(node);
        if (dclinfo && (node->tag == StructTag || node->tag == FnDclTag))
            dclinfo->facts |= DclActorGen;
    }

    actorRegister(handle, state, members.crossing);
}
