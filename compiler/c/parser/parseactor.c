/** Parse an actor declaration, and generate what it stands for
 *
 * 'actor Counter { count: u64; fn init(self: &new, n: u64) {...}; pub async fn
 * bump(self, n: u64) {...} }' declares an actor: state that lives on, reached by
 * nothing but messages, each a call of one of its behaviours (its methods
 * declared 'async fn'), run later, one at a time, on whichever worker thread of
 * the actors package takes it up. Its methods declared 'fn' -- its 'init', its
 * 'final', and helpers -- are synchronous, run inside the actor by whatever
 * calls them. No node stands for an actor. The parser turns its declaration
 * into the four declarations the actors package runs an actor by, generated as
 * Cone source and parsed here, in this order:
 *
 *   struct Counter.State {count: u64; fn init...; fn bump...} the body as written
 *   struct Counter {                                         the handle
 *     mailbox: Arc[opaq, actors.Mailbox[Counter.Msg]];
 *     fn init(self: &new, n: u64) {
 *       let st = actors.makeActor[Counter.State, Counter.Msg](&Counter.dispatch);
 *       mem.writeRaw[Counter.State](st, Counter.State(n));
 *       *self = Self(mailbox: actors.startActor[Counter.State, Counter.Msg](st));}
 *     pub fn bump(self: &, n: u64) {actors.send[Counter.Msg](mailbox, Counter.Msg.bump[n]);}
 *   }
 *   enum Counter.Msg is Sendable {struct bump {n: u64;}}    one variant per behaviour
 *   fn Counter.dispatch(self: &uni Counter.State, msg: *Counter.Msg) {
 *     if &*msg is &Counter.Msg.bump {let p = (msg as *u8 as usize) as *Counter.Msg.bump; self.bump(actors.take(&(*p).n));}
 *   }
 *
 * The handle's initializer makes the actor's block first and constructs the
 * state in it in place (a construction written raw is filled where it is
 * written, genlFnCall), so the state's own 'init' runs on the state where it
 * stays, in an actor that is real already: 'selfactor' and a send to itself
 * are good there, and the messages they send run once the init has returned.
 *
 * The handle's method sending a behaviour is 'pub' where the behaviour is. In
 * the state the behaviour is an ordinary method, which only the dispatcher
 * calls: 'self.bump(n)' written in the actor's own methods sends it, through
 * the actor's handle (selfActorSend).
 *
 * and, once per module, the helper 'actors.take' (a function of the module's
 * own, named as no source can spell), which moves a field out of a message:
 * 'fn actors.take[T + 'static](p: &T) T {mem.readRaw[T]((p as *T as usize) as *T)}':
 * what is moved out holds only borrows of the whole program, the only borrows a
 * message carries, and none of the borrow of the node it is read from, which the
 * trip through a number says to the loan walk.
 *
 * What an 'await' and a message's reply need (compiler/c/doc/phases/parse.md,
 * section 6, and generation.md, "A message's reply"):
 *
 * - a behaviour that returns a value, 'pub async fn get(self, k: u64) u64', has
 *   a second variant, its request awaited, 'struct get'ask {k u64; reply'
 *   actors.Reply;}', the handle a second method sending it, and the dispatch
 *   an arm that answers it; and the handle a third method, 'get'future',
 *   which makes an actors.Future of what it returns and sends the request
 *   awaited with the future's producer's reference as its envelope, for a
 *   call whose value is kept (actorFutureCall);
 * - an actor whose body holds an 'await' (DclTexts.awaiting) has hidden
 *   fields in its state, its pending table and, where a behaviour returning a
 *   value awaits, its Answer slot; and 'Counter.reply'' and
 *   'Counter.replyid'', which make a request's envelope (one call of
 *   actors.replyFor: a reply is no variant of the message enum, but a slot of
 *   its own kind in the actor's mailbox ring);
 * - an actor with a behaviour, or writing 'selfactor', has 'Counter.self'',
 *   its handle from its state.
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
 * The order is a reader's, and nothing depends on it: the message enum holds
 * handles by value, and a handle reaches the enum again only through its
 * owning reference to the Mailbox, which lays out nothing of what it points
 * at, so the handle is laid out whole before the enum is.
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

// What the generated text spells '`#n`'. The names up to GenState are the same
// in every actor of every module; the rest are each actor's own, and after
// them, from GenSlots on, the name of each of its messages that returns a
// value, sent awaited ('fetch'ask')
enum GenSlot {
    GenActors,      // The actors package, as this module imports it
    GenSync,        // The sync package, whose Arc counts a handle's owners
    GenSendable,    // The built-in marker the message enum declares
    GenTake,        // The module's helper that moves a message's field out of its node
    GenMailbox,     // The handle's one field
    GenNone,        // The one variant of the message enum of an actor with no messages
    GenPending,     // The state's hidden pending table, where the actor awaits
    GenAnswer,      // The state's hidden Answer slot, where a message returning a value awaits
    GenEnvelope,    // A request variant's envelope, its reply
    GenMem,         // Core's 'mem', whatever the actor's own names hide it
    GenAt,          // The handle's initializer's local: where the state is made
    GenState,      // 'Counter.State'
    GenMsg,         // 'Counter.Msg'
    GenDispatch,    // 'Counter.dispatch'
    GenHidden,      // The struct the state's hidden fields are read from
    GenReply,       // 'Counter.reply'': a request's envelope, its resume message naming no record
    GenReplyId,     // 'Counter.replyid'': the same, naming the record's id
    GenSelf,        // 'Counter.self'': 'selfactor', the handle from the state
    GenAllocs,      // The struct the state's allocator is read from
    GenSlots
};

static Name *genShared[GenState];

// The names of the handle methods that send a behaviour awaited ('fetch'ask')
// and for a future ('fetch'future'), which no source can spell. One pair for
// each behaviour name in the whole compile, not one for each actor: an actor
// trait's handle, and the virtual trait it dispatches through, declare methods
// by these names and an actor's handle meets them by the same names
typedef struct BehaviourName {
    Name *method;
    Name *ask;
    Name *future;
} BehaviourName;

static BehaviourName *behaviourNames = NULL;
static uint32_t behaviourNameCnt = 0;
static uint32_t behaviourNameMax = 0;

static Name *parseBehaviourName(Name *method, int future) {
    for (uint32_t i = 0; i < behaviourNameCnt; ++i) {
        if (behaviourNames[i].method == method)
            return future ? behaviourNames[i].future : behaviourNames[i].ask;
    }
    if (behaviourNameCnt == behaviourNameMax) {
        uint32_t newmax = behaviourNameMax ? behaviourNameMax * 2 : 32;
        BehaviourName *grown = (BehaviourName *)memAllocBlk(newmax * sizeof(BehaviourName));
        if (behaviourNameCnt)
            memcpy(grown, behaviourNames, behaviourNameCnt * sizeof(BehaviourName));
        behaviourNames = grown;
        behaviourNameMax = newmax;
    }
    BehaviourName *entry = &behaviourNames[behaviourNameCnt++];
    char buf[300];
    entry->method = method;
    snprintf(buf, sizeof(buf), "%s'ask", &method->namestr);
    entry->ask = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s'future", &method->namestr);
    entry->future = nametblPrivate(buf, strlen(buf));
    return future ? entry->future : entry->ask;
}

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

// ---- 'async fn' ----------------------------------------------------------------

// Pass over a declaration that will not be read: up to its ';', or through its
// '{ ... }', whichever comes first
static void parseBehaviourSkip() {
    while (!lexIsToken(SemiToken) && !lexIsToken(LCurlyToken) && !lexIsToken(RCurlyToken) && !lexIsToken(EofToken))
        lexNextToken();
    if (lexIsToken(LCurlyToken))
        parseSkipDclBody();
    else if (lexIsToken(SemiToken))
        lexNextToken();
}

int parseBehaviourWords(char *where) {
    if (!lexNextIsWord("fn")) {
        errorMsgLex(ErrorBehaviourWords,
            "'async' means nothing alone: an actor's behaviour is declared 'async fn name(self, ...)'.");
        parseBehaviourSkip();
        return 0;
    }
    if (where) {
        errorMsgLex(ErrorBehaviourWords,
            "'async fn' declares an actor's behaviour, which only an actor's body holds, and this is %s. A function here is declared 'fn'.",
            where);
        parseBehaviourSkip();
        return 0;
    }
    lexNextToken();
    return 1;
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
static void genSlot(GenText *g, uint32_t slot) {
    char buf[16];
    snprintf(buf, sizeof(buf), "`#%d`", (int)slot);
    genPuts(g, buf);
}

// A parameter as the author wrote it: its name, its type, and its default
static void genParm(GenText *g, DclText *text, Name *name) {
    genName(g, name);
    genPuts(g, ": ");
    genPutn(g, text->type, text->typeend - text->type);
    if (text->value) {
        genPuts(g, " = ");
        genPutn(g, text->value, text->valueend - text->value);
    }
}

// The body of one of the handle's initializers, round the state's construction,
// whose arguments the caller writes between the two: the actor's block made,
// the state constructed in it in place, so that its init runs on the state
// where it stays, in an actor that is real already, then the actor started
// (the actors package, A NEW ACTOR)
static void genStartOpen(GenText *g) {
    genPuts(g, ") {let ");
    genSlot(g, GenAt);
    genPuts(g, " = ");
    genSlot(g, GenActors);
    genPuts(g, ".makeActor[");
    genSlot(g, GenState);
    genPuts(g, ", ");
    genSlot(g, GenMsg);
    genPuts(g, "](&");
    genSlot(g, GenDispatch);
    genPuts(g, "); ");
    genSlot(g, GenMem);
    genPuts(g, ".writeRaw[");
    genSlot(g, GenState);
    genPuts(g, "](");
    genSlot(g, GenAt);
    genPuts(g, ", ");
    genSlot(g, GenState);
    genPuts(g, "(");
}

static void genStartClose(GenText *g) {
    genPuts(g, ")); *self = Self(");
    genSlot(g, GenMailbox);
    genPuts(g, ": ");
    genSlot(g, GenActors);
    genPuts(g, ".startActor[");
    genSlot(g, GenState);
    genPuts(g, ", ");
    genSlot(g, GenMsg);
    genPuts(g, "](");
    genSlot(g, GenAt);
    genPuts(g, "));}\n");
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

// Is this type, as parsed, a borrow written ''static', which lives for the
// whole program? Whether it may cross is the thread check's (actorCheckAll):
// 'imm', 'opaq' or 'uni', and what it points at Sendable
static int parseActorIsStaticBorrow(INode *type) {
    return parseActorIsBorrow(type) && ((RefNode *)type)->lifename == staticLifeName;
}

// Does every lifetime the type's text names read ''static'? Any other is a
// lifetime of the actor's own, which no message carries
static int parseActorOnlyStatic(char *from, char *to) {
    for (char *at = from; at < to; ++at) {
        if (*at != '\'')
            continue;
        if (to - at < 7 || memcmp(at + 1, "static", 6) != 0
            || (to - at > 7 && ((at[7] >= 'a' && at[7] <= 'z') || (at[7] >= 'A' && at[7] <= 'Z')
                || (at[7] >= '0' && at[7] <= '9') || at[7] == '_')))
            return 0;
        at += 6;
    }
    return 1;
}

// The permissions a method's 'self' may be written with
static int parseActorSelfPerm(PermNode *perm) {
    return perm == uniPerm || perm == mutPerm || perm == roPerm || perm == immPerm || perm == opaqPerm;
}

static int parseActorIsInit(FnDclNode *fn) {
    return fn->namesym == initName || fn->overloadsym == initName;
}

// Was this function of the actor's body declared 'async fn', a behaviour?
static int parseActorIsBehaviour(DclTexts *texts, FnDclNode *fn) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(texts->behaviours, cnt, nodesp)) {
        if (*nodesp == (INode *)fn)
            return 1;
    }
    return 0;
}

// Does this behaviour return a value, as written?
static int parseActorReturns(FnDclNode *fn) {
    INode *rettype = ((FnSigNode *)fn->vtype)->rettype;
    return rettype != NULL && rettype->tag != VoidTag;
}

// Does this method's body hold an 'await' (parseFn noted it)?
static int parseActorAwaits(DclTexts *texts, FnDclNode *fn) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(texts->awaiting, cnt, nodesp)) {
        if (*nodesp == (INode *)fn)
            return 1;
    }
    return 0;
}

// Check a method's 'self': the state, lent to a behaviour for the message it
// handles, and to a synchronous method by its caller. Written bare it is the
// state borrowed 'uni', which the method has alone while it runs; '&', '&mut',
// '&imm', '&opaq' and '&uni' are taken as written; an initializer's is '&new'
// and the finalizer's '&uni', as any type's are.
static int parseActorSelf(StructNode *state, FnDclNode *fn) {
    VarDclNode *self = (VarDclNode *)nodesGet(((FnSigNode *)fn->vtype)->parms, 0);
    INode *type = self->vtype;
    if (isNameUseNode(type) && ((NameUseNode *)type)->namesym == selfTypeName) {
        self->vtype = (INode *)newRefNodeFull(RefTag, (INode *)self, borrowRef, newPermUseNode(uniPerm), type);
        return 1;
    }
    if (type->tag == RefTag && ((RefNode *)type)->region == borrowRef) {
        PermNode *perm = parseActorPerm((RefNode *)type);
        if (parseActorSelfPerm(perm))
            return 1;
        if (perm == newPerm && parseActorIsInit(fn))
            return 1;
    }
    errorMsgNode((INode *)self, ErrorActorMember,
        "%s's 'self' is actor %s's state, lent to the method for the message it handles: write 'self', which has it 'uni', or 'self: &', 'self: &mut', 'self: &imm', 'self: &opaq' or 'self: &uni' (an initializer's is 'self: &new').",
        &fn->namesym->namestr, &state->namesym->namestr);
    return 0;
}

// What a message about the declaration being read calls it: "Actor", and
// "Actor trait" while an actor trait's behaviours are read and its runtime found
static char *parseActorKind = "Actor";

// Check the parameters of a behaviour or an initializer, which cross to the
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
                "%s %s's %s %s takes %s, the actor's own state, which never leaves the actor. %s.",
                parseActorKind, &actorname->namestr, isinit ? "initializer" : "behaviour", &fn->namesym->namestr,
                &parm->namesym->namestr, crossesto);
            ok = 0;
            continue;
        }
        if ((parseActorIsBorrow(parm->vtype) && !parseActorIsStaticBorrow(parm->vtype))
            || !parseActorOnlyStatic(text->type, text->typeend)) {
            errorMsgNode((INode *)parm, ErrorNotSendable,
                "%s %s's %s %s takes %s, a borrowed reference, which is not Sendable: no borrow may leave its thread. %s; pass an owner that may cross, such as a 'uni' one, or an 'Arc', or a borrow that lives for the whole program, written &'static imm, &'static opaq or &'static uni.",
                parseActorKind, &actorname->namestr, isinit ? "initializer" : "behaviour", &fn->namesym->namestr,
                &parm->namesym->namestr, crossesto);
            ok = 0;
            continue;
        }
        nodesAdd(crossing, (INode *)fn);
        nodesAdd(crossing, (INode *)parm);
    }
    return ok;
}

// The members of an actor's body: which are behaviours, which initializers,
// and what may not be there at all
typedef struct ActorMembers {
    Nodes *messages;     // The behaviours that may be sent, each a message
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
        int behaviour = parseActorIsBehaviour(texts, fn);
        if (fn->flags & FlagExtern) {
            errorMsgNode(node, ErrorActorMember, "An actor's method is written with its body: 'extern' declares one defined elsewhere, which an actor's never is.");
            continue;
        }
        // A function without 'self' is a helper of the state's, reached from its
        // methods; it has no state to run on, so it is no behaviour
        if (!(fn->flags & FlagMethFld)) {
            if (behaviour)
                errorMsgNode(node, ErrorActorMember,
                    "%s is declared 'async fn', a behaviour of actor %s's, and has no 'self': a behaviour runs on the actor's state, which is its first parameter, 'self'.",
                    &fn->namesym->namestr, &actorname->namestr);
            else if (fn->flags & FlagPub)
                errorMsgNode(node, ErrorActorMember,
                    "%s has no 'self', so it would not run on actor %s: only a behaviour is a message, and an actor is made by its name with parentheses. Declare a function that needs no actor outside it.",
                    &fn->namesym->namestr, &actorname->namestr);
            continue;
        }
        if (!parseActorSelf(state, fn))
            continue;
        if (behaviour && (fn->namesym == finalName || parseActorIsInit(fn))) {
            errorMsgNode(node, ErrorActorMember,
                "An actor's %s is synchronous, declared 'fn': %s. A behaviour, 'async fn', is a message the actor runs when it is scheduled.",
                fn->namesym == finalName ? "'final'" : "initializer",
                fn->namesym == finalName ? "it runs as the actor dies" : "it runs on the thread making the actor, before the actor takes any message");
            continue;
        }
        if (fn->namesym == finalName)
            continue;
        if (parseActorIsInit(fn)) {
            ++members->declared;
            if (parseActorParms(texts, state, actorname, fn, &members->crossing))
                nodesAdd(&members->inits, node);
            continue;
        }
        // A method declared 'fn' is synchronous: run inside the actor, by the
        // method that calls it, never sent. Nothing outside the actor reaches it
        if (!behaviour) {
            if (fn->flags & FlagPub)
                errorMsgNode(node, ErrorActorMember,
                    "%s is a 'fn' of actor %s's: synchronous, run inside the actor by the method that calls it, so nothing outside the actor reaches it, and it is never 'pub'. A message the actor's handle sends is a behaviour, declared 'async fn': 'pub async fn %s(...)'.",
                    &fn->namesym->namestr, &actorname->namestr, &fn->namesym->namestr);
            continue;
        }

        // A behaviour, sent as a message. What it returns goes back in a reply
        // to an 'await' that sent it, and is dropped where it was sent with none
        int ok = 1;
        FnSigNode *sig = (FnSigNode *)fn->vtype;
        if (fn->genericinfo) {
            errorMsgNode(node, ErrorActorMember,
                "Actor %s's behaviour %s is generic, which a behaviour cannot be yet: each is one variant of the actor's message type.",
                &actorname->namestr, &fn->namesym->namestr);
            ok = 0;
        }
        VarDclNode *self = (VarDclNode *)nodesGet(sig->parms, 0);
        PermNode *selfperm = parseActorPerm((RefNode *)self->vtype);
        if (!parseActorSelfPerm(selfperm)) {
            errorMsgNode((INode *)self, ErrorActorMember,
                "A behaviour's 'self' is the actor's state, lent for the message: 'self' (which has it 'uni'), 'self: &', 'self: &mut', 'self: &imm', 'self: &opaq' or 'self: &uni'.");
            ok = 0;
        }
        // The reply goes back in a message too: a borrow of the whole program
        // may, written ''static' (which the thread check then judges,
        // actorCheckAll); a lifetime of any other name never
        if (parseActorReturns(fn)) {
            DclText *rettext = parseActorText(texts, fn->vtype);
            if (rettext && rettext->type && !parseActorOnlyStatic(rettext->type, rettext->typeend)) {
                errorMsgNode(sig->rettype, ErrorNotSendable,
                    "Actor %s's behaviour %s returns a type naming a lifetime other than 'static, which is not Sendable: no borrow but one that lives for the whole program, written &'static imm, &'static opaq or &'static uni, goes back in a reply.",
                    &actorname->namestr, &fn->namesym->namestr);
                ok = 0;
            }
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
            "Actor %s names an abstraction or a base, which an actor does not have yet. It conforms to an actor trait by having its behaviours, with nothing written on the actor.",
            &actorname->namestr);
        return;
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&state->fields, cnt, nodesp)) {
        if ((*nodesp)->flags & IsMixin) {
            errorMsgNode((INode *)state, ErrorActorMember,
                "Actor %s names an abstraction or a base, which an actor does not have yet. It conforms to an actor trait by having its behaviours, with nothing written on the actor.",
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
    // A GPU has no threads, and an actor is mailboxes that worker threads run
    if (flowGpu) {
        errorMsgNode((INode *)at, ErrorGpuUnavailable,
            "In GPU code no actor is declared, '%s': an actor is run by worker threads, which a GPU has none of. A kernel is one function that thousands of invocations run at once.",
            &actorname->namestr);
        return 0;
    }
    if (genShared[GenActors] == NULL) {
        genShared[GenActors] = nametblPrivate("actors", 6);
        genShared[GenSync] = nametblPrivate("sync", 4);
        genShared[GenSendable] = nametblPrivate("Sendable", 8);
        genShared[GenTake] = nametblPrivate("actors.take", 11);
        genShared[GenMailbox] = nametblPrivate("mailbox", 7);
        genShared[GenNone] = nametblPrivate("none", 4);
        genShared[GenPending] = nametblPrivate("pending'", 8);
        genShared[GenAnswer] = nametblPrivate("answer'", 7);
        genShared[GenEnvelope] = nametblPrivate("reply'", 6);
        genShared[GenMem] = nametblPrivate("mem", 3);
        genShared[GenAt] = nametblPrivate("state'", 6);
    }
    if (namespaceFind(&mod->namespace, genShared[GenActors]))
        return 1;

    ModuleNode *actorsmod = parseActorFindImport(mod->imports, "actors");
    if (actorsmod == NULL) {
        errorMsgNode((INode *)at, ErrorActorRuntime,
            "%s %s runs on the actors package, which module %s does not import: write 'import actors;' after the 'mod' line.",
            parseActorKind, &actorname->namestr, &mod->namesym->namestr);
        return 0;
    }
    ModuleNode *syncmod = parseActorFindImport(actorsmod->imports, "sync");
    if (syncmod == NULL || namespaceFind(&actorsmod->namespace, nametblFind("startActor", 10)) == NULL
        || namespaceFind(&actorsmod->namespace, nametblFind("makeActor", 9)) == NULL) {
        errorMsgNode((INode *)at, ErrorActorRuntime,
            "%s %s runs on the actors package, and the module %s imports as actors is not that package: it has no makeActor or startActor, or does not import sync.",
            parseActorKind, &actorname->namestr, &mod->namesym->namestr);
        return 0;
    }
    // Core's 'mem', reached by a name of its own, so that an initializer's
    // parameter named 'mem' does not hide it from the text written in that
    // initializer
    ModuleNode *coremod = parseActorFindImport(mod->imports, "core");
    INode *memdcl = coremod ? namespaceFind(&coremod->namespace, nametblFind("mem", 3)) : NULL;
    if (memdcl == NULL) {
        errorMsgNode((INode *)at, ErrorActorRuntime,
            "%s %s runs on core's 'mem', which module %s does not reach.",
            parseActorKind, &actorname->namestr, &mod->namesym->namestr);
        return 0;
    }
    parseActorBind(mod, genShared[GenActors], (INode *)actorsmod, FlagImportName);
    parseActorBind(mod, genShared[GenSync], (INode *)syncmod, FlagImportName);
    parseActorBind(mod, genShared[GenSendable], (INode *)sendableTrait, 0);
    parseActorBind(mod, genShared[GenMem], memdcl, 0);

    // What generation calls for a split method's seams, which no source names
    for (int i = 0; i < ActorRtCount; ++i) {
        INode *fn = namespaceFind(&actorsmod->namespace, nametblFind(actorRuntimeNames[i], strlen(actorRuntimeNames[i])));
        actorRuntime[i] = fn && fn->tag == FnDclTag ? (FnDclNode *)fn : NULL;
    }
    // What an 'await' on an operation awaits (ir/exp/await.c, awaitOperation)
    INode *awaitable = namespaceFind(&actorsmod->namespace, nametblFind("Awaitable", 9));
    actorAwaitable = awaitable && awaitable->tag == StructTag ? (StructNode *)awaitable : NULL;
    // A behaviour's future, which a call whose value is used gives, and an
    // 'await' may wait for (actorFutureCall, awaitFuture)
    INode *future = namespaceFind(&actorsmod->namespace, nametblFind("Future", 6));
    actorFuture = future && future->tag == StructTag ? (StructNode *)future : NULL;

    // The dispatch function moves each argument out of the message where it
    // lies in its node, which is then freed without being finalized: so every
    // field is moved out exactly once, a field that may only move as well as
    // one that is copied
    genPuts(g, "fn ");
    genSlot(g, GenTake);
    genPuts(g, "[T + 'static](p: &T) T {mem.readRaw[T]((p as *T as usize) as *T)}\n");
    return 2;
}

// ---- 'actor trait' -----------------------------------------------------------
//
// 'actor trait ChunkSink { pub async fn accept(self, c: Chunk); }' declares what
// a sender needs of an actor, behaviours only: nothing outside an actor can run
// code inside it, so a caller can only send. An actor conforms where it has each
// behaviour, 'pub', under the name, with the parameter and returned types
// (actorTraitCoerce asks, where a handle of an actor is converted). The trait's
// name, used as a type, is a handle of two words, generated as Cone source and
// parsed here, in this order:
//
//   struct trait ChunkSink.vt {                     an ordinary trait, with the three
//     pub fn accept(self: &, c: Chunk);             methods an actor's handle has for
//   }                                               each behaviour (send, and for one
//                                                   returning a value, ask and future)
//   struct ChunkSink.fat {data: *u8; vt: *u8;}      the two words of a virtual reference
//   struct ChunkSink is Sendable {
//     mailbox: Arc[opaq, actors.Mailbox[u8]];       the actor's mailbox, counted
//     table: *u8;                                   the vtable of the actor's handle as vt
//     pub fn accept(self: &, c: Chunk) {            sends through the table: one indirect
//       let v = <the virtual reference made of mailbox and table>;   call of the actor's own
//       v.accept(c);}                                                 sending method
//     fn wrap(v: &<ChunkSink.vt) ChunkSink {...}    the conversion, called on a borrow of an
//   }                                               actor's handle: the handle's mailbox
//                                                   copied, the virtual reference's table
//
// A generic trait, 'actor trait Sink[T]', declares its type parameters, as written,
// on the virtual trait and the handle (and so on 'wrap', a function of the handle's
// own, which an instance has at its own arguments); the bracketed names follow
// each use of either.
//
// The table is the compiler's own vtable of the actor's handle as 'vt': one for
// each (actor, trait) pair, made where the handle is converted, as any trait's
// is, its methods the handle's own that send a message. The mailbox's type is
// erased to 'Mailbox[u8]': what a handle of any actor owns is the one allocation
// whose first part is the Mailbox, which holds no message of its own type, and
// the handle's copy and drop count it and give it back without naming that
// type. The virtual reference is rebuilt from the mailbox's place in the
// handle, which is where an actor's handle is, and the table.
//
// The trait as written is kept too ('ChunkSink.behaviours', a trait of the
// requirements), as the state of an actor is, for the parameter types.

enum TraitSlot {
    TrVt = GenState,    // 'ChunkSink.vt'
    TrFat,              // 'ChunkSink.fat'
    TrWrap,             // 'ChunkSink.wrap'
    TrBody,             // 'ChunkSink.behaviours'
    TrTable,            // The handle's second field
    TrWords,            // A handle's method's local: the virtual reference's two words
    TrVia,              // and the virtual reference made of them
    TrSlots             // From here, as GenSlots: each behaviour returning a value, its ask name, then its future's
};

// Check a behaviour's 'self': an actor trait's is not lent to anything, so any
// borrowing form is read as the one a sender needs, '&'
static int parseActorTraitSelf(StructNode *body, FnDclNode *fn) {
    VarDclNode *self = (VarDclNode *)nodesGet(((FnSigNode *)fn->vtype)->parms, 0);
    INode *type = self->vtype;
    if (isNameUseNode(type) && ((NameUseNode *)type)->namesym == selfTypeName) {
        self->vtype = (INode *)newRefNodeFull(RefTag, (INode *)self, borrowRef, newPermUseNode(roPerm), type);
        return 1;
    }
    if (type->tag == RefTag && ((RefNode *)type)->region == borrowRef
        && parseActorSelfPerm(parseActorPerm((RefNode *)type)))
        return 1;
    errorMsgNode((INode *)self, ErrorActorTraitMember,
        "%s's 'self' is whatever actor the trait's handle holds, which a sender only sends to: write 'self'.",
        &fn->namesym->namestr);
    return 0;
}

// The behaviours of an actor trait's body, and what it may not hold at all
static void parseActorTraitMembers(DclTexts *texts, StructNode *body, Name *traitname, ActorMembers *members) {
    INode **nodesp;
    uint32_t cnt;
    if (body->lifeparms)
        errorMsgNode((INode *)body, ErrorActorTraitMember,
            "Actor trait %s names a lifetime among its type parameters, which a message cannot carry: no borrow leaves its thread.", &traitname->namestr);
    if (body->basetrait || body->extendsbase)
        errorMsgNode((INode *)body, ErrorActorTraitMember,
            "Actor trait %s names an abstraction or a base, which an actor trait does not have yet.", &traitname->namestr);
    for (nodelistFor(&body->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        if (field->flags & IsMixin) {
            errorMsgNode((INode *)body, ErrorActorTraitMember,
                "Actor trait %s names an abstraction or a base, which an actor trait does not have yet.", &traitname->namestr);
            break;
        }
        errorMsgNode((INode *)field, ErrorActorTraitMember,
            "%s is a field, and an actor trait declares behaviours only: the actors that conform have their own state, which only their own methods reach, so a sender can only send them a message.",
            &field->namesym->namestr);
    }
    for (nodelistFor(&body->nodelist, cnt, nodesp)) {
        INode *node = *nodesp;
        if (node->tag == MacroDclTag) {
            errorMsgNode(node, ErrorActorTraitMember, "An actor trait declares behaviours only, and a macro is not one.");
            continue;
        }
        if (node->tag == VarDclTag) {
            errorMsgNode(node, ErrorActorTraitMember, "An actor trait declares behaviours only, and a 'static' is not one.");
            continue;
        }
        if (node->tag != FnDclTag)
            continue;
        FnDclNode *fn = (FnDclNode *)node;
        if (!parseActorIsBehaviour(texts, fn)) {
            errorMsgNode(node, ErrorActorTraitMember,
                "%s is a 'fn', and an actor trait declares behaviours only: code cannot be run inside an actor by a caller outside it, so all a sender has is a message to send. Declare it 'pub async fn %s(self, ...);' if an actor should take it as one.",
                &fn->namesym->namestr, &fn->namesym->namestr);
            continue;
        }
        if (fn->value != NULL) {
            errorMsgNode(node, ErrorActorTraitMember,
                "Behaviour %s has a body, and an actor trait declares signatures only: each actor that conforms writes the behaviour itself, with its own state to run it on.",
                &fn->namesym->namestr);
            continue;
        }
        if (!(fn->flags & FlagMethFld)) {
            errorMsgNode(node, ErrorActorTraitMember,
                "Behaviour %s has no 'self': a behaviour is sent to an actor, which is its first parameter, 'self'.",
                &fn->namesym->namestr);
            continue;
        }
        if (!parseActorTraitSelf(body, fn))
            continue;
        int ok = 1;
        if (!(fn->flags & FlagPub)) {
            errorMsgNode(node, ErrorActorTraitMember,
                "Behaviour %s is not 'pub', and an actor trait's behaviours are what a sender outside the actor sends: declare it 'pub async fn %s(...)'.",
                &fn->namesym->namestr, &fn->namesym->namestr);
            ok = 0;
        }
        if (fn->overloadsym) {
            errorMsgNode(node, ErrorActorTraitMember,
                "Behaviour %s is declared 'overload', which an actor trait's behaviour is not yet.", &fn->namesym->namestr);
            ok = 0;
        }
        if (fn->genericinfo) {
            errorMsgNode(node, ErrorActorTraitMember,
                "Behaviour %s is generic, which a behaviour cannot be yet: each is one variant of the actor's message type.",
                &fn->namesym->namestr);
            ok = 0;
        }
        FnSigNode *sig = (FnSigNode *)fn->vtype;
        if (parseActorReturns(fn)) {
            DclText *rettext = parseActorText(texts, fn->vtype);
            if (rettext && rettext->type && !parseActorOnlyStatic(rettext->type, rettext->typeend)) {
                errorMsgNode(sig->rettype, ErrorNotSendable,
                    "Actor trait %s's behaviour %s returns a type naming a lifetime other than 'static, which is not Sendable: no borrow but one that lives for the whole program, written &'static imm, &'static opaq or &'static uni, goes back in a reply.",
                    &traitname->namestr, &fn->namesym->namestr);
                ok = 0;
            }
        }
        parseActorKind = "Actor trait";
        int parmsok = parseActorParms(texts, body, traitname, fn, &members->crossing);
        parseActorKind = "Actor";
        if (parmsok && ok)
            nodesAdd(&members->messages, node);
    }
}

// The parameters of a behaviour as a method of the handle or of the virtual
// trait declares them, 'self' left out: as the author wrote them, or with their
// defaults too
static void genTraitParms(GenText *g, DclTexts *texts, FnDclNode *fn, int defaults) {
    INode **parmp;
    uint32_t parmcnt;
    for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
        VarDclNode *parm = (VarDclNode *)*parmp;
        if (parm->namesym == selfName)
            continue;
        DclText *text = parseActorText(texts, (INode *)parm);
        genPuts(g, ", ");
        if (defaults)
            genParm(g, text, parm->namesym);
        else {
            genName(g, parm->namesym);
            genPuts(g, ": ");
            genPutn(g, text->type, text->typeend - text->type);
        }
    }
}

// The arguments of a call passing a behaviour's parameters on, each by its name
static void genTraitArgs(GenText *g, FnDclNode *fn) {
    int first = 1;
    INode **parmp;
    uint32_t parmcnt;
    for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
        VarDclNode *parm = (VarDclNode *)*parmp;
        if (parm->namesym == selfName)
            continue;
        if (!first)
            genPuts(g, ", ");
        first = 0;
        genName(g, parm->namesym);
    }
}

// What a behaviour returns, as the actors package's Future[..] holds it
static void genTraitFuture(GenText *g, DclTexts *texts, FnDclNode *fn) {
    DclText *rettext = parseActorText(texts, fn->vtype);
    int tuple = ((FnSigNode *)fn->vtype)->rettype->tag == TupleTag;
    genSlot(g, GenActors);
    genPuts(g, ".Future[");
    genPuts(g, tuple ? "(" : "");
    if (rettext)
        genPutn(g, rettext->type, rettext->typeend - rettext->type);
    genPuts(g, tuple ? ")]" : "]");
}

// The virtual reference a handle's method calls through, made of the mailbox's
// place in the handle (where an actor's handle holds its mailbox, the handle's
// whole) and the table
static void genTraitUse(GenText *g, StructNode *body) {
    if (body->genericinfo == NULL)
        return;
    genPuts(g, "[");
    INode **parmp;
    uint32_t cnt;
    int first = 1;
    for (nodesFor(body->genericinfo->parms, cnt, parmp)) {
        if (!first)
            genPuts(g, ", ");
        first = 0;
        genName(g, ((GenVarDclNode *)*parmp)->namesym);
    }
    genPuts(g, "]");
}

// A generic trait's type parameters as written, to declare the same on each
// generated type
static void genTraitDecl(GenText *g, DclTexts *texts) {
    if (texts->tparms == NULL)
        return;
    genPuts(g, "[");
    genPutn(g, texts->tparms, texts->tparmsend - texts->tparms);
    genPuts(g, "]");
}

static void genTraitVia(GenText *g, StructNode *body) {
    genPuts(g, " {var ");
    genSlot(g, TrWords);
    genPuts(g, " = ");
    genSlot(g, TrFat);
    genPuts(g, "(&self.");
    genSlot(g, GenMailbox);
    genPuts(g, " as *u8, self.");
    genSlot(g, TrTable);
    genPuts(g, "); let ");
    genSlot(g, TrVia);
    genPuts(g, " = ");
    genSlot(g, GenMem);
    genPuts(g, ".readRaw[&<");
    genSlot(g, TrVt);
    genTraitUse(g, body);
    genPuts(g, "](&mut ");
    genSlot(g, TrWords);
    genPuts(g, " as *(&<");
    genSlot(g, TrVt);
    genTraitUse(g, body);
    genPuts(g, "));");
}

static void parseActorTrait(ParseState *parse, uint16_t pubflag) {
    ModuleNode *mod = parse->mod;

    // The body, read as a trait's with its behaviours noted, and where each
    // parameter's and return type is written kept, to be written again
    DclTexts texts = {NULL, 0, 0, 0, NULL, 0, NULL};
    texts.awaiting = newNodes(4);
    texts.behaviours = newNodes(8);
    DclTexts *svtexts = parse->dcltexts;
    parse->dcltexts = &texts;
    StructNode *body = (StructNode *)parseStruct(parse, 0);
    parse->dcltexts = svtexts;
    Name *traitname = body->namesym;
    if (traitname == anonName)
        return;

    ActorMembers members;
    members.messages = newNodes(8);
    members.inits = newNodes(2);
    members.crossing = newNodes(8);
    members.declared = 0;
    parseActorTraitMembers(&texts, body, traitname, &members);

    GenText g = {NULL, 0, 0};
    genPuts(&g, "");
    parseActorKind = "Actor trait";
    int runtime = parseActorRuntime(parse, body, traitname, &g);
    parseActorKind = "Actor";
    if (runtime == 0)
        return;

    INode **nodesp;
    uint32_t cnt;
    uint32_t nasks = 0;
    for (nodesFor(members.messages, cnt, nodesp)) {
        if (parseActorReturns((FnDclNode *)*nodesp))
            ++nasks;
    }

    uint32_t nnames = TrSlots + 2 * nasks;
    Name **names = (Name **)memAllocBlk(nnames * sizeof(Name *));
    for (int i = 0; i < GenState; ++i)
        names[i] = genShared[i];
    char buf[300];
    snprintf(buf, sizeof(buf), "%s.vt", &traitname->namestr);
    names[TrVt] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.fat", &traitname->namestr);
    names[TrFat] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.wrap", &traitname->namestr);
    names[TrWrap] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.behaviours", &traitname->namestr);
    names[TrBody] = nametblPrivate(buf, strlen(buf));
    names[TrTable] = nametblPrivate("table", 5);
    names[TrWords] = nametblPrivate("words'", 6);
    names[TrVia] = nametblPrivate("via'", 4);
    uint32_t ask = TrSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        if (parseActorReturns(fn)) {
            names[ask] = parseBehaviourName(fn->namesym, 0);
            names[ask + nasks] = parseBehaviourName(fn->namesym, 1);
            ++ask;
        }
    }
    body->namesym = names[TrBody];

    // The virtual trait: the methods of an actor's handle for each behaviour
    genPuts(&g, "struct trait ");
    genSlot(&g, TrVt);
    genTraitDecl(&g, &texts);
    genPuts(&g, " {\n");
    ask = TrSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        int returns = parseActorReturns(fn);
        genPuts(&g, "  pub fn ");
        genName(&g, fn->namesym);
        genPuts(&g, "(self: &");
        genTraitParms(&g, &texts, fn, 0);
        genPuts(&g, ");\n");
        if (returns) {
            genPuts(&g, "  pub fn ");
            genSlot(&g, ask);
            genPuts(&g, "(self: &");
            genTraitParms(&g, &texts, fn, 0);
            genPuts(&g, ", ");
            genSlot(&g, GenEnvelope);
            genPuts(&g, ": ");
            genSlot(&g, GenActors);
            genPuts(&g, ".Reply);\n  pub fn ");
            genSlot(&g, ask + nasks);
            genPuts(&g, "(self: &");
            genTraitParms(&g, &texts, fn, 0);
            genPuts(&g, ") ");
            genTraitFuture(&g, &texts, fn);
            genPuts(&g, ";\n");
            ++ask;
        }
    }
    genPuts(&g, "}\n");

    // The two words of a virtual reference
    genPuts(&g, "struct ");
    genSlot(&g, TrFat);
    genPuts(&g, " {data: *u8; vt: *u8;}\n");

    // The handle
    genPuts(&g, "struct ");
    genName(&g, traitname);
    genTraitDecl(&g, &texts);
    genPuts(&g, " is ");
    genSlot(&g, GenSendable);
    genPuts(&g, " {\n  ");
    genSlot(&g, GenMailbox);
    genPuts(&g, ": ");
    genSlot(&g, GenSync);
    genPuts(&g, ".Arc[opaq, ");
    genSlot(&g, GenActors);
    genPuts(&g, ".Mailbox[u8]];\n  ");
    genSlot(&g, TrTable);
    genPuts(&g, ": *u8;\n  pub fn `==`(self: &, o: ");
    genName(&g, traitname);
    genTraitUse(&g, body);
    genPuts(&g, ") bool {*(&self.");
    genSlot(&g, GenMailbox);
    genPuts(&g, " as *usize) == *(&o.");
    genSlot(&g, GenMailbox);
    genPuts(&g, " as *usize)}\n");
    ask = TrSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        int returns = parseActorReturns(fn);
        genPuts(&g, "  pub fn ");
        genName(&g, fn->namesym);
        genPuts(&g, "(self: &");
        genTraitParms(&g, &texts, fn, 1);
        genPuts(&g, ")");
        genTraitVia(&g, body);
        genPuts(&g, " ");
        genSlot(&g, TrVia);
        genPuts(&g, ".");
        genName(&g, fn->namesym);
        genPuts(&g, "(");
        genTraitArgs(&g, fn);
        genPuts(&g, ");}\n");
        if (returns) {
            genPuts(&g, "  pub fn ");
            genSlot(&g, ask);
            genPuts(&g, "(self: &");
            genTraitParms(&g, &texts, fn, 0);
            genPuts(&g, ", ");
            genSlot(&g, GenEnvelope);
            genPuts(&g, ": ");
            genSlot(&g, GenActors);
            genPuts(&g, ".Reply)");
            genTraitVia(&g, body);
            genPuts(&g, " ");
            genSlot(&g, TrVia);
            genPuts(&g, ".");
            genSlot(&g, ask);
            genPuts(&g, "(");
            genTraitArgs(&g, fn);
            genPuts(&g, ((FnSigNode *)fn->vtype)->parms->used > 1 ? ", " : "");
            genSlot(&g, GenEnvelope);
            genPuts(&g, ");}\n  pub fn ");
            genSlot(&g, ask + nasks);
            genPuts(&g, "(self: &");
            genTraitParms(&g, &texts, fn, 0);
            genPuts(&g, ") ");
            genTraitFuture(&g, &texts, fn);
            genTraitVia(&g, body);
            genPuts(&g, " ");
            genSlot(&g, TrVia);
            genPuts(&g, ".");
            genSlot(&g, ask + nasks);
            genPuts(&g, "(");
            genTraitArgs(&g, fn);
            genPuts(&g, ")}\n");
            ++ask;
        }
    }
    // The conversion's function, called on a borrow of an actor's handle: a
    // function of the handle, so that an instance of a generic trait has its own
    genPuts(&g, "  fn ");
    genSlot(&g, TrWrap);
    genPuts(&g, "(v: &<");
    genSlot(&g, TrVt);
    genTraitUse(&g, body);
    genPuts(&g, ") ");
    genName(&g, traitname);
    genTraitUse(&g, body);
    genPuts(&g, " {let f = ");
    genSlot(&g, GenMem);
    genPuts(&g, ".readRaw[");
    genSlot(&g, TrFat);
    genPuts(&g, "](&v as *");
    genSlot(&g, TrFat);
    genPuts(&g, "); let r = f.data as &");
    genSlot(&g, GenSync);
    genPuts(&g, ".Arc[opaq, ");
    genSlot(&g, GenActors);
    genPuts(&g, ".Mailbox[u8]]; ");
    genName(&g, traitname);
    genTraitUse(&g, body);
    genPuts(&g, "(*r, f.vt)}\n}\n");

    // Parse it, as the trait's own source, its diagnostics reported at its name
    Lexer *gen = lexNew(g.text, lex->url);
    gen->genat = (INode *)body;
    gen->gennames = names;
    gen->ngennames = nnames;
    uint32_t firstnode = mod->nodes->used;
    lexPush(gen);
    if (runtime == 2)
        parseFnOrVar(parse, 0);
    modAddNode(mod, names[TrBody], (INode *)body);
    StructNode *vt = (StructNode *)parseStruct(parse, 0);
    modAddNode(mod, names[TrVt], (INode *)vt);
    StructNode *fat = (StructNode *)parseStruct(parse, 0);
    modAddNode(mod, names[TrFat], (INode *)fat);
    StructNode *handle = (StructNode *)parseStruct(parse, pubflag);
    modAddNode(mod, handle->namesym, (INode *)handle);
    FnDclNode *wrap = NULL;
    INode **wp;
    uint32_t wcnt;
    for (nodelistFor(&handle->nodelist, wcnt, wp)) {
        if ((*wp)->tag == FnDclTag && ((FnDclNode *)*wp)->namesym == names[TrWrap])
            wrap = (FnDclNode *)*wp;
    }
    if (!lexIsToken(EofToken))
        errorMsgLex(ErrorNoEof, "The declarations generated for actor trait %s did not parse whole.", &traitname->namestr);
    lexPop();

    // Every declaration generated here, exported by a library compile whatever
    // its visibility, since an importer generates them again (dclIsExported)
    for (uint32_t i = firstnode; i < mod->nodes->used; ++i) {
        INode *node = nodesGet(mod->nodes, i);
        DclInfo *dclinfo = inodeGetDclInfo(node);
        if (dclinfo && (node->tag == StructTag || node->tag == FnDclTag))
            dclinfo->facts |= DclActorGen;
    }

    // Each behaviour, and the handle's methods that send it
    ActorTraitInfo *info = (ActorTraitInfo *)memAllocBlk(sizeof(ActorTraitInfo));
    memset(info, 0, sizeof(ActorTraitInfo));
    info->handle = handle;
    info->body = body;
    info->vt = vt;
    info->wrap = wrap;
    info->crossing = members.crossing;
    info->nmsgs = members.messages->used;
    info->msgs = (ActorMessage *)memAllocBlk((info->nmsgs ? info->nmsgs : 1) * sizeof(ActorMessage));
    ask = TrSlots;
    uint32_t m = 0;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        ActorMessage *am = &info->msgs[m++];
        am->method = fn;
        am->send = NULL;
        am->ask = NULL;
        am->future = NULL;
        am->generic = 0;
        Name *futurename = parseActorReturns(fn) ? names[ask + nasks] : NULL;
        Name *askname = parseActorReturns(fn) ? names[ask++] : NULL;
        INode **hp;
        uint32_t hcnt;
        for (nodelistFor(&handle->nodelist, hcnt, hp)) {
            if ((*hp)->tag != FnDclTag)
                continue;
            FnDclNode *hfn = (FnDclNode *)*hp;
            if (hfn->namesym == fn->namesym && am->send == NULL && (hfn->flags & FlagMethFld))
                am->send = hfn;
            else if (askname && hfn->namesym == askname)
                am->ask = hfn;
            else if (futurename && hfn->namesym == futurename)
                am->future = hfn;
        }
    }
    actorTraitRegister(info);
}

// ---- The declaration ---------------------------------------------------------

void parseActor(ParseState *parse, uint16_t pubflag) {
    ModuleNode *mod = parse->mod;

    // 'actor trait', the abstraction of an actor
    if (lexNextIsWord("trait")) {
        parseActorTrait(parse, pubflag);
        return;
    }

    // The body, read as a struct's, is the state. Where each field's and
    // parameter's type and default are written is kept, to be written again
    DclTexts texts = {NULL, 0, 0, 0, NULL, 0, NULL};
    texts.awaiting = newNodes(4);
    texts.behaviours = newNodes(8);
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

    INode **nodesp;
    uint32_t cnt;

    // What the actor's seams need generated. A message holding an 'await' is
    // split (ir/exp/await.c): its actor gets a pending table, a resume variant
    // and arm for each shape of continuation, and the functions that make a
    // request's envelope; one that returns a value as well answers through
    // the state's Answer slot, which its seams carry
    int awaits = texts.awaiting->used > 0;
    int answers = 0;
    uint32_t nasks = 0;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        if (parseActorReturns(fn)) {
            ++nasks;
            answers |= parseActorAwaits(&texts, fn);
        }
    }

    // The names this actor's declarations are known by: the shared ones, its
    // own, each request variant's (from GenSlots on, in message order), and
    // each handle method's that stores a request's future (from GenSlots +
    // nasks on)
    uint32_t nnames = GenSlots + 2 * nasks;
    Name **names = (Name **)memAllocBlk(nnames * sizeof(Name *));
    for (int i = 0; i < GenState; ++i)
        names[i] = genShared[i];
    char buf[300];
    snprintf(buf, sizeof(buf), "%s.State", &actorname->namestr);
    names[GenState] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.Msg", &actorname->namestr);
    names[GenMsg] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.dispatch", &actorname->namestr);
    names[GenDispatch] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.hidden'", &actorname->namestr);
    names[GenHidden] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.reply'", &actorname->namestr);
    names[GenReply] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.replyid'", &actorname->namestr);
    names[GenReplyId] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.self'", &actorname->namestr);
    names[GenSelf] = nametblPrivate(buf, strlen(buf));
    snprintf(buf, sizeof(buf), "%s.allocs'", &actorname->namestr);
    names[GenAllocs] = nametblPrivate(buf, strlen(buf));
    uint32_t ask = GenSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        if (parseActorReturns(fn)) {
            names[ask] = parseBehaviourName(fn->namesym, 0);
            names[ask + nasks] = parseBehaviourName(fn->namesym, 1);
            ++ask;
        }
    }
    state->namesym = names[GenState];

    // The state's hidden fields, read as a struct's of their own and moved
    // into the state, after the fields written: each has a default, so a
    // construction of the state as written leaves them to it
    if (awaits) {
        genPuts(&g, "struct ");
        genSlot(&g, GenHidden);
        genPuts(&g, " {\n  ");
        genSlot(&g, GenPending);
        genPuts(&g, ": ");
        genSlot(&g, GenActors);
        genPuts(&g, ".Pending = ");
        genSlot(&g, GenActors);
        genPuts(&g, ".Pending();\n");
        if (answers) {
            genPuts(&g, "  ");
            genSlot(&g, GenAnswer);
            genPuts(&g, ": ");
            genSlot(&g, GenActors);
            genPuts(&g, ".Answer = ");
            genSlot(&g, GenActors);
            genPuts(&g, ".Answer();\n");
        }
        genPuts(&g, "}\n");
    }

    // The state's way to its own region, which every actor carries (in its
    // Header, made with it): a method 'allocRegion' giving the region the
    // actor's values are allocated in, which is what makes 'new self[T](v)' mean
    // 'self.allocRegion().alloc(T(v))' (typelit.c, a value that leads to a
    // region). Read as a struct's of its own and moved into the state like the
    // hidden fields; the region is found from the state by the actors runtime
    // (actors.regionOf)
    genPuts(&g, "struct ");
    genSlot(&g, GenAllocs);
    genPuts(&g, " {\n  fn allocRegion(self: &mut ");
    genSlot(&g, GenState);
    genPuts(&g, ") &'static mut ");
    genSlot(&g, GenActors);
    genPuts(&g, ".ActorRegion {\n    ");
    genSlot(&g, GenActors);
    genPuts(&g, ".regionOf[");
    genSlot(&g, GenState);
    genPuts(&g, ", ");
    genSlot(&g, GenMsg);
    genPuts(&g, "](self)\n  }\n}\n");

    // The message enum: a variant per message, its fields the parameters,
    // written after the handle (a reference to it lays out nothing of it).
    // A message that returns a value has a second, its request awaited, which
    // carries the reply's envelope too. An actor that awaits has the two
    // variants of a reply: which second half, the record's id where it has a
    // record, and where the returned value is, in the reply's node
    GenText ge = {NULL, 0, 0};
    genPuts(&ge, "enum ");
    genSlot(&ge, GenMsg);
    genPuts(&ge, " is ");
    genSlot(&ge, GenSendable);
    genPuts(&ge, " {\n");
    if (members.messages->used == 0) {
        genPuts(&ge, "  ");
        genSlot(&ge, GenNone);
        genPuts(&ge, ";\n");
    }
    ask = GenSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        int returns = parseActorReturns(fn);
        for (int asked = 0; asked <= returns; ++asked) {
            genPuts(&ge, "  struct ");
            if (asked)
                genSlot(&ge, ask++);
            else
                genName(&ge, fn->namesym);
            genPuts(&ge, " {");
            INode **parmp;
            uint32_t parmcnt;
            for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
                VarDclNode *parm = (VarDclNode *)*parmp;
                if (parm->namesym == selfName)
                    continue;
                DclText *text = parseActorText(&texts, (INode *)parm);
                genName(&ge, parm->namesym);
                genPuts(&ge, ": ");
                genPutn(&ge, text->type, text->typeend - text->type);
                genPuts(&ge, "; ");
            }
            if (asked) {
                genSlot(&ge, GenEnvelope);
                genPuts(&ge, ": ");
                genSlot(&ge, GenActors);
                genPuts(&ge, ".Reply; ");
            }
            genPuts(&ge, "}\n");
        }
    }
    genPuts(&ge, "}\n");

    // The handle: its one field, an initializer for each of the state's, and
    // a method per message, which sends it
    genPuts(&g, "struct ");
    genName(&g, actorname);
    genPuts(&g, " {\n  ");
    genSlot(&g, GenMailbox);
    genPuts(&g, ": ");
    genSlot(&g, GenSync);
    genPuts(&g, ".Arc[opaq, ");
    genSlot(&g, GenActors);
    genPuts(&g, ".Mailbox[");
    genSlot(&g, GenMsg);
    genPuts(&g, "]];\n");
    // Two handles are equal when they are of the one actor: the one mailbox
    genPuts(&g, "  pub fn `==`(self: &, o: ");
    genName(&g, actorname);
    genPuts(&g, ") bool {*(&self.");
    genSlot(&g, GenMailbox);
    genPuts(&g, " as *usize) == *(&o.");
    genSlot(&g, GenMailbox);
    genPuts(&g, " as *usize)}\n");
    if (members.declared == 0) {
        // The state's implicit initializer: its fields with their types written,
        // in order, the rest taking their defaults. Public where nothing private
        // is set by it: a pub actor with no fields
        int hasfields = 0;
        for (nodelistFor(&state->fields, cnt, nodesp))
            hasfields |= !((*nodesp)->flags & IsMixin);
        genPuts(&g, (pubflag && !hasfields) ? "  pub fn init(self: &new" : "  fn init(self: &new");
        for (nodelistFor(&state->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            DclText *text = parseActorText(&texts, (INode *)field);
            if ((field->flags & IsMixin) || text == NULL || text->type == NULL)
                continue;
            genPuts(&g, ", ");
            genParm(&g, text, field->namesym);
        }
        genStartOpen(&g);
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
        genStartClose(&g);
    }
    for (nodesFor(members.inits, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        genPuts(&g, (fn->flags & FlagPub) ? "  pub fn " : "  fn ");
        genName(&g, fn->namesym);
        if (fn->overloadsym) {
            genPuts(&g, " overload ");
            genName(&g, fn->overloadsym);
        }
        genPuts(&g, "(self: &new");
        INode **parmp;
        uint32_t parmcnt;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            genPuts(&g, ", ");
            genParm(&g, parseActorText(&texts, (INode *)parm), parm->namesym);
        }
        genStartOpen(&g);
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
        genStartClose(&g);
    }
    // A behaviour that returns a value is sent awaited by a second method,
    // which an 'await' calls in place of the first (awaitTypeCheck): it takes
    // the reply's envelope too, which goes in the message, beside the
    // arguments. Each is as public as the behaviour
    ask = GenSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        int returns = parseActorReturns(fn);
        for (int asked = 0; asked <= returns; ++asked) {
            uint32_t variant = asked ? ask++ : 0;
            genPuts(&g, (fn->flags & FlagPub) ? "  pub fn " : "  fn ");
            if (asked)
                genSlot(&g, variant);
            else {
                genName(&g, fn->namesym);
                if (fn->overloadsym) {
                    genPuts(&g, " overload ");
                    genName(&g, fn->overloadsym);
                }
            }
            genPuts(&g, "(self: &");
            INode **parmp;
            uint32_t parmcnt;
            for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
                VarDclNode *parm = (VarDclNode *)*parmp;
                if (parm->namesym == selfName)
                    continue;
                genPuts(&g, ", ");
                // The awaited one is called with every argument the first
                // was, defaults appended already, so it takes none
                DclText *text = parseActorText(&texts, (INode *)parm);
                if (asked) {
                    genName(&g, parm->namesym);
                    genPuts(&g, ": ");
                    genPutn(&g, text->type, text->typeend - text->type);
                }
                else
                    genParm(&g, text, parm->namesym);
            }
            if (asked) {
                genPuts(&g, ", ");
                genSlot(&g, GenEnvelope);
                genPuts(&g, ": ");
                genSlot(&g, GenActors);
                genPuts(&g, ".Reply");
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
            if (asked)
                genSlot(&g, variant);
            else
                genName(&g, fn->namesym);
            // A variant holding values is constructed with parentheses; one that
            // holds none, by its name alone
            int anyparm = 0;
            for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp))
                if (((VarDclNode *)*parmp)->namesym != selfName)
                    anyparm = 1;
            int holds = anyparm || asked;
            int first = 1;
            if (holds)
                genPuts(&g, "(");
            for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
                VarDclNode *parm = (VarDclNode *)*parmp;
                if (parm->namesym == selfName)
                    continue;
                if (!first)
                    genPuts(&g, ", ");
                first = 0;
                genName(&g, parm->namesym);
            }
            if (asked) {
                if (!first)
                    genPuts(&g, ", ");
                genSlot(&g, GenEnvelope);
            }
            genPuts(&g, holds ? "));}\n" : ");}\n");
        }
    }
    // A behaviour that returns a value is called for its future by a third
    // method, which a call whose value is used calls in place of the first
    // (actorFutureCall): it makes the future, Pending, and sends the request
    // awaited, its envelope the future's producer's reference, and gives back
    // the future, the consumer's. It is as public as the behaviour, and is
    // called with every argument the first was, defaults appended already
    ask = GenSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        if (!parseActorReturns(fn))
            continue;
        uint32_t variant = ask++;
        DclText *rettext = parseActorText(&texts, fn->vtype);
        int tuple = ((FnSigNode *)fn->vtype)->rettype->tag == TupleTag;
        GenText rt = {NULL, 0, 0};
        genPuts(&rt, tuple ? "(" : "");
        if (rettext)
            genPutn(&rt, rettext->type, rettext->typeend - rettext->type);
        genPuts(&rt, tuple ? ")" : "");
        // The same, as an expression reads it, where a type argument is written
        // among a call's: no lifetime can be named there, and a generic is
        // instanced with its type arguments' lifetimes erased in any case
        GenText rtx = {NULL, 0, 0};
        for (uint32_t at = 0; at < rt.len; ++at) {
            if (rt.len - at >= 7 && memcmp(rt.text + at, "'static", 7) == 0) {
                at += 6;
                if (at + 1 < rt.len && rt.text[at + 1] == ' ')
                    ++at;
                continue;
            }
            genPutn(&rtx, rt.text + at, 1);
        }
        genPuts(&g, (fn->flags & FlagPub) ? "  pub fn " : "  fn ");
        genSlot(&g, variant + nasks);
        genPuts(&g, "(self: &");
        INode **parmp;
        uint32_t parmcnt;
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            DclText *text = parseActorText(&texts, (INode *)parm);
            genPuts(&g, ", ");
            genName(&g, parm->namesym);
            genPuts(&g, ": ");
            genPutn(&g, text->type, text->typeend - text->type);
        }
        genPuts(&g, ") ");
        genSlot(&g, GenActors);
        genPuts(&g, ".Future[");
        genPutn(&g, rt.text, rt.len);
        genPuts(&g, "] {\n    let f = ");
        genSlot(&g, GenActors);
        genPuts(&g, ".futureFor[");
        genPutn(&g, rtx.text, rtx.len);
        genPuts(&g, ", ");
        genSlot(&g, GenMsg);
        genPuts(&g, "](");
        genSlot(&g, GenMailbox);
        genPuts(&g, ");\n    ");
        genSlot(&g, GenActors);
        genPuts(&g, ".send[");
        genSlot(&g, GenMsg);
        genPuts(&g, "](");
        genSlot(&g, GenMailbox);
        genPuts(&g, ", ");
        genSlot(&g, GenMsg);
        genPuts(&g, ".");
        genSlot(&g, variant);
        genPuts(&g, "(");
        for (nodesFor(((FnSigNode *)fn->vtype)->parms, parmcnt, parmp)) {
            VarDclNode *parm = (VarDclNode *)*parmp;
            if (parm->namesym == selfName)
                continue;
            genName(&g, parm->namesym);
            genPuts(&g, ", ");
        }
        genSlot(&g, GenActors);
        genPuts(&g, ".futureReply[");
        genPutn(&g, rtx.text, rtx.len);
        genPuts(&g, "](&f)));\n    f\n  }\n");
    }
    genPuts(&g, "}\n");

    genPutn(&g, ge.text, ge.len);

    // The dispatch function: the message where it lies in its node, each
    // argument moved out of it into the method's call
    genPuts(&g, "fn ");
    genSlot(&g, GenDispatch);
    genPuts(&g, "(self: &uni ");
    genSlot(&g, GenState);
    genPuts(&g, ", msg: *");
    genSlot(&g, GenMsg);
    genPuts(&g, ") {\n");
    //
    // A message that returns a value: sent with no 'await', its value is
    // dropped; awaited, it goes back in the reply. One that holds an 'await'
    // too may be parked by a seam and return no value: its request's envelope
    // waits in the state's Answer slot, which its seams carry, and the value
    // is taken only where the slot says the message returned one (answerAt)
    int firstarm = 1;
    ask = GenSlots;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        int returns = parseActorReturns(fn);
        int slotted = returns && parseActorAwaits(&texts, fn);
        for (int asked = 0; asked <= returns; ++asked) {
            uint32_t variant = asked ? ask++ : 0;
            genPuts(&g, firstarm ? "  if &*msg is &" : "  else if &*msg is &");
            firstarm = 0;
            genSlot(&g, GenMsg);
            genPuts(&g, ".");
            if (asked)
                genSlot(&g, variant);
            else
                genName(&g, fn->namesym);
            genPuts(&g, " {let p = (msg as *u8 as usize) as *");
            genSlot(&g, GenMsg);
            genPuts(&g, ".");
            if (asked)
                genSlot(&g, variant);
            else
                genName(&g, fn->namesym);
            genPuts(&g, "; ");
            if (slotted) {
                genSlot(&g, GenActors);
                if (asked) {
                    genPuts(&g, ".ask(&mut self.");
                    genSlot(&g, GenAnswer);
                    genPuts(&g, ", ");
                    genSlot(&g, GenTake);
                    genPuts(&g, "(&(*p).");
                    genSlot(&g, GenEnvelope);
                    genPuts(&g, ")); let r = ");
                }
                else {
                    genPuts(&g, ".askNone(&mut self.");
                    genSlot(&g, GenAnswer);
                    genPuts(&g, "); let r = ");
                }
                genSlot(&g, GenActors);
                genPuts(&g, ".keep(&");
            }
            else if (asked) {
                genSlot(&g, GenActors);
                genPuts(&g, ".answerNow(");
                genSlot(&g, GenTake);
                genPuts(&g, "(&(*p).");
                genSlot(&g, GenEnvelope);
                genPuts(&g, "), ");
            }
            genPuts(&g, "self.");
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
            if (slotted) {
                genPuts(&g, ")); ");
                genSlot(&g, GenActors);
                genPuts(&g, ".answerAt(&mut self.");
                genSlot(&g, GenAnswer);
                genPuts(&g, ", r);}\n");
            }
            else
                genPuts(&g, asked ? "));}\n" : ");}\n");
        }
    }
    // (A reply to one of the actor's own requests is no message of the enum: it
    // arrives as a slot of its own kind in the mailbox's ring, which the actors
    // package's runBatch resumes through the seam's resume function.)
    genPuts(&g, "}\n");

    // A request's envelope, made by this actor as it awaits: 'run' is the
    // seam's resume function, 'id' its record's place in the pending table, and
    // the returned value 'size' bytes aligned to 'align' (genlawait.c,
    // genlAwaitReply). Nothing is allocated: the answer claims a slot in this
    // actor's ring when it comes
    for (uint32_t withid = 0; awaits && withid <= 1; ++withid) {
        genPuts(&g, "fn ");
        genSlot(&g, withid ? GenReplyId : GenReply);
        genPuts(&g, "(st: &");
        genSlot(&g, GenState);
        genPuts(&g, withid ? ", run: *u8, id: u64, size: usize, align: usize) " : ", run: *u8, size: usize, align: usize) ");
        genSlot(&g, GenActors);
        genPuts(&g, ".Reply {\n  ");
        genSlot(&g, GenActors);
        genPuts(&g, ".replyFor[");
        genSlot(&g, GenState);
        genPuts(&g, ", ");
        genSlot(&g, GenMsg);
        genPuts(&g, withid ? "](st, run, id, true, size, align)\n}\n" : "](st, run, 0u64, false, size, align)\n}\n");
    }

    // 'selfactor': the actor's own handle, another owner of its mailbox, which
    // 'self.m()' sends a behaviour through as well
    int selfs = texts.selfactors > 0 || members.messages->used > 0;
    if (selfs) {
        genPuts(&g, "fn ");
        genSlot(&g, GenSelf);
        genPuts(&g, "(st: &");
        genSlot(&g, GenState);
        genPuts(&g, ") ");
        genName(&g, actorname);
        genPuts(&g, " {");
        genName(&g, actorname);
        genPuts(&g, "(");
        genSlot(&g, GenMailbox);
        genPuts(&g, ": ");
        genSlot(&g, GenActors);
        genPuts(&g, ".selfOf[");
        genSlot(&g, GenState);
        genPuts(&g, ", ");
        genSlot(&g, GenMsg);
        genPuts(&g, "](st))}\n");
    }

    // Parse it, as the actor's own source, its diagnostics reported at its name
    Lexer *gen = lexNew(g.text, lex->url);
    gen->genat = (INode *)state;
    gen->gennames = names;
    gen->ngennames = nnames;
    uint32_t firstnode = mod->nodes->used;
    lexPush(gen);
    if (runtime == 2)
        parseFnOrVar(parse, 0);
    ActorInfo *info = (ActorInfo *)memAllocBlk(sizeof(ActorInfo));
    memset(info, 0, sizeof(ActorInfo));
    if (awaits) {
        StructNode *hidden = (StructNode *)parseStruct(parse, 0);
        for (nodelistFor(&hidden->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            structAddField(state, field);
            if (field->namesym == names[GenPending])
                info->pending = field;
            else if (field->namesym == names[GenAnswer])
                info->answer = field;
        }
    }
    StructNode *allocs = (StructNode *)parseStruct(parse, 0);
    for (nodelistFor(&allocs->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag)
            iNsTypeAddFn((INsTypeNode *)state, (FnDclNode *)*nodesp);
    }
    modAddNode(mod, names[GenState], (INode *)state);
    StructNode *handle = (StructNode *)parseStruct(parse, pubflag);
    modAddNode(mod, handle->namesym, (INode *)handle);
    StructNode *msg = (StructNode *)parseStruct(parse, TraitType | SameSize | EnumType);
    modAddNode(mod, names[GenMsg], (INode *)msg);
    info->dispatch = (FnDclNode *)parseFnOrVar(parse, 0);
    if (awaits) {
        info->replyfn = (FnDclNode *)parseFnOrVar(parse, 0);
        info->replyidfn = (FnDclNode *)parseFnOrVar(parse, 0);
    }
    if (selfs)
        info->selffn = (FnDclNode *)parseFnOrVar(parse, 0);
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

    // Each message, and the handle's methods that send it
    info->handle = handle;
    info->state = state;
    info->crossing = members.crossing;
    info->awaiting = texts.awaiting;
    info->behaviours = texts.behaviours;
    info->nmsgs = members.messages->used;
    info->msgs = (ActorMessage *)memAllocBlk((info->nmsgs ? info->nmsgs : 1) * sizeof(ActorMessage));
    ask = GenSlots;
    uint32_t m = 0;
    for (nodesFor(members.messages, cnt, nodesp)) {
        FnDclNode *fn = (FnDclNode *)*nodesp;
        ActorMessage *am = &info->msgs[m++];
        am->method = fn;
        am->send = NULL;
        am->ask = NULL;
        am->future = NULL;
        am->generic = 0;
        Name *futurename = parseActorReturns(fn) ? names[ask + nasks] : NULL;
        Name *askname = parseActorReturns(fn) ? names[ask++] : NULL;
        INode **hp;
        uint32_t hcnt;
        for (nodelistFor(&handle->nodelist, hcnt, hp)) {
            if ((*hp)->tag != FnDclTag)
                continue;
            FnDclNode *hfn = (FnDclNode *)*hp;
            if (hfn->namesym == fn->namesym && am->send == NULL && (hfn->flags & FlagMethFld)
                && hfn->overloadsym == fn->overloadsym)
                am->send = hfn;
            else if (askname && hfn->namesym == askname)
                am->ask = hfn;
            else if (futurename && hfn->namesym == futurename)
                am->future = hfn;
        }
    }
    actorRegister(info);
}
