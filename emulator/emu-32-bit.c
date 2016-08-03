/* =================================== */
/* REDUCERON EMULATOR WITH SPECULATIVE */
/* EVALUATION OF PRIMITIVE REDEXES     */
/* Matthew N                           */
/* 23 September 2009                   */
/* Tommy Thorn 2014-07-28              */
/* =================================== */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>

/* Compile-time options */

#define MAXPUSH 8
#define APSIZE  4 // This version will not work with any other value
#define MAXAPS  4
#define MAXLUTS 2
#define MAXREGS 8

#define MAXHEAPAPPS 32000
#define MAXSTACKELEMS 8000
#define MAXTEMPLATES 8000

#define NAMELEN 128

#define perform(action) (action, 1)

#include "red_atom.h"

typedef struct
  {
    Char name[NAMELEN];
    Int arity;
    Int numLuts;
    Lut luts[MAXLUTS];
    Int numPushs;
    Atom pushs[MAXPUSH];
    Int numApps;
    App apps[MAXAPS];
  } Template;

typedef struct { Int saddr; Int haddr; } Update;

Atom falseAtom, trueAtom, mainAtom;



/* Globals */

App* heap;
App* heap2;
Atom* stack;
Update* ustack;
Lut* lstack;
Template* code;
Atom *registers;

Int hp, gcLow, gcHigh, sp, usp, lsp, end, gcCount;

Int numTemplates;

/* Profiling info */

Long swapCount, primCount, applyCount, unwindCount,
     updateCount, selectCount, prsCandidateCount, prsSuccessCount;

Bool tracingEnabled = 0;

typedef struct
  {
    Bool seen;
    Int callCount;
  } ProfEntry;

ProfEntry *profTable;

static const char *__restrict program_name;

static void __attribute__ ((__noreturn__))
    error(const char *__restrict fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: error: ", program_name);

    va_start(ap, fmt);
    (void) vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    exit(EXIT_FAILURE);
}

/* Display profiling table */

void displayProfTable()
{
  Int i, j, ticksPerCall;
  printf("\nPROFILING TABLE:\n");
  printf("+----------------------------------+------+----------+\n");
  printf("| %-32s | %4s | %8s |\n", "FUNCTION", "SIZE", "%TIME");
  printf("+----------------------------------+------+----------+\n");
  for (i = 0; i < numTemplates; i++) {
    if (! profTable[i].seen) {
      ticksPerCall = 0;
      for (j = i; j < numTemplates; j++) {
        if (!strcmp(code[j].name,code[i].name)) {
          ticksPerCall++;
          profTable[j].seen = 1;
        }
      }
      printf("| %-32s |   %2i | %8.2f |\n",
        code[i].name, ticksPerCall,
        (100*(double)(profTable[i].callCount*ticksPerCall))/
        (double)applyCount);
    }
  }
  printf("+----------------------------------+------+----------+\n");
}

/* Dashing */

Atom dash(Bool sh, Atom a)
{
    if (sh && isPTR(a) && !getPTRShared(a))
        return mkPTR(1, getPTRId(a));
    else
        return a;
}

void dashApp(App* app)
{
  Int i;
  for (i = 0; i < getAppSize(*app); i++)
      if (isPTR(getAppAtom(*app, i)))
          shareAppAtom(app, i);
}

/* Unwinding */

static inline Bool nf(App* app)
{
    return getAppTag(*app) == CASE ? 0 : getAppNF(*app);
}

void pushAtoms(App app)
{
  Int i;
  for (i = getAppSize(app)-1; i >= 0; i--)
      stack[sp++] = getAppAtom(app, i);
}

void unwind(Bool sh, Int addr)
{
  App app = heap[addr];
  if (sh && !nf(&app)) {
    Update u; u.saddr = sp; u.haddr = addr;
    ustack[usp++] = u;
  }
  if (sh)
      dashApp(&app);
  if (getAppTag(app) == CASE)
      lstack[lsp++] = getAppLUT(app);
  sp--;
  assert(getAppSize(app));
  pushAtoms(app);
}

/* Updating */

static inline Int arity(Atom a)
{
    if (isINT(a))
        return 1;
    else if (isCON(a))
        return getCONArity(a) + 1;
    else if (isFUN(a))
        return getFUNArity(a);
    else if (isPRI(a))
        return getPRIArity(a);
    else
        error("arity(): invalid tag");
    return 0;
}

Bool updateCheck(Atom top, Update utop)
{
  return (arity(top) > sp - utop.saddr);
}

void upd(Atom top, Int sp, Int len, Int hp)
{
  Int i, j;
  Atom atoms[APSIZE];

  atoms[0] = top;
  for (i = 1, j = sp; i < len; i++, j--) {
    atoms[i] = stack[j] = dash(1, stack[j]);
  }

  heap[hp] = mkApp(AP, len, 1, 0, atoms);
}

void update(Atom top, Int saddr, Int haddr)
{
    Int len = 1 + sp - saddr;
    Int p = sp-2;

    for (;;) {
        if (len < APSIZE) {
            if (len <= 0)
                error("zero sized app updated");
            else
                upd(top, p, len, haddr);
            usp--;
        } else {
            upd(top, p, APSIZE, hp);
            p -= APSIZE-1; len -= APSIZE-1;
            top = mkPTR(1, hp);
            hp++;
        }
    }
}

/* Primitive reduction */

Atom prim_ld32(Int addr)
{
    /* For now, a quick hack:
       [0] - serial in
    */
    Atom res = mkINT(666);

    if (addr == 0)
        res = mkINT(getchar());

    if (tracingEnabled) {
        printf("[[ld32 (%d) -> %d]]", addr, getINTValue(res));
        fflush(stdout);
    }

    /* This is a hack to terminate otherwise infinite processes */
    if (getINTValue(res) < 0)
        exit(0);

    return res;
}

Atom prim_st32(Int addr, Int value, Atom k)
{
    /* For now, a quick hack:
       [0] - serial out
    */

    if (addr == 0)
        putchar(value);

    if (tracingEnabled) {
        printf("[[st32 (%d)=%d]]", addr, value);
        fflush(stdout);
    }

    return k;
}

Atom prim(Prim p, Atom a, Atom b, Atom c)
{
  Atom result = 0;
  Int n, m;
  n = getINTValue(a);
  m = getINTValue(b);
  switch (p) {
    case ADD: result = mkINT(n+m); break;
    case SUB: result = mkINT(n-m); break;
    case EQ: result = n == m ? trueAtom : falseAtom; break;
    case NEQ: result = n != m ? trueAtom : falseAtom; break;
    case LEQ: result = n <= m ? trueAtom : falseAtom; break;
    case EMIT: printf("%c", n); fflush(stdout); result = b; break;
    case EMITINT: printf("%i", n); fflush(stdout); result = b; break;
    case AND: result = mkINT(n & m); break;
    case ST32: result = prim_st32(n, m, c); break;
    case LD32: result = prim_ld32(n); break;
    default: assert(0);
  }

  return result;
}

void applyPrim()
{
  Atom p = stack[sp-2];
  Prim pid = getPRIId(p);
  if (pid == SEQ) {
    stack[sp-2] = stack[sp-3];
    stack[sp-3] = stack[sp-1];
    sp-=1;
    primCount++;
  }
  else if (isINT(stack[sp-3])
        || pid == EMIT || pid == EMITINT) {
    if (getPRISwap(p))
        stack[sp-3] = prim(pid, stack[sp-3], stack[sp-1], stack[4 <= sp ? sp-4 : 0]);
    else
        stack[sp-3] = prim(pid, stack[sp-1], stack[sp-3], stack[4 <= sp ? sp-4 : 0]);
    sp -= getPRIArity(p);
    primCount++;
  }
  else {
      Atom tmp;
      stack[sp-2] = togglePRISwap(stack[sp-2]);
      tmp = stack[sp-1];
      stack[sp-1] = stack[sp-3];
      stack[sp-3] = tmp;
      swapCount++;
  }
}

/* Case-alt selection */

void caseSelect(Int index)
{
  Int lut = lstack[lsp-1];
  stack[sp-1] = mkFUN(1,0,lut+index);
  lsp--;
}

/* Function application */

Atom inst(Int base, Int argPtr, Atom a)
{
    if (isPTR(a)) {
        a = mkPTR(getPTRShared(a), base + getPTRId(a));
  }
  else if (isARG(a)) {
      a = dash(getARGShared(a), stack[argPtr - getARGIndex(a)]);
  }
  else if (isREG(a)) {
      a = dash(getREGShared(a), registers[getREGIndex(a)]);
  }
  return a;
}

Atom getPrimArg(Int argPtr, Atom a)
{
    if (isARG(a)) return stack[argPtr - getARGIndex(a)];
    else if (isREG(a)) return registers[getREGIndex(a)];
  else return a;
}

void instApp(Int base, Int argPtr, App *app)
{
  Int i;
  Atom a, b;
  App* new = &heap[hp];
  Int rid;

  if (getAppTag(*app) == PRIM) {
    prsCandidateCount++;
    a = getAppAtom(*app, 0);
    b = getAppAtom(*app, 2);
    a = getPrimArg(argPtr, a);
    b = getPrimArg(argPtr, b);
    rid = getAppRegId(*app);
    if (isINT(a) && isINT(b)) {
      prsSuccessCount++;
      registers[rid] = prim(getPRIId(getAppAtom(*app, 1)), a, b, b);
    }
    else {
      Atom atoms[APSIZE];
      registers[rid] = mkPTR(0, hp);

      for (i = 0; i < getAppSize(*app); i++)
          atoms[i] = inst(base, argPtr, getAppAtom(*app, i));

      *new = mkApp(AP, getAppSize(*app), 0, 0, atoms);

      hp++;
    }
  }
  else {
    Atom atoms[APSIZE];

    for (i = 0; i < getAppSize(*app); i++)
        atoms[i] = inst(base, argPtr, getAppAtom(*app, i));

    *new = mkApp(getAppTag(*app),
                 getAppSize(*app),
                 getAppNF(*app),
                 getAppLUT(*app),
                 atoms);

    hp++;
  }
}

void slide(Int p, Int n)
{
  Int i;
  for (i = p; i < sp; i++) stack[i-n] = stack[i];
  sp -= n;
}

void apply(Template* t)
{
  Int i;
  Int base = hp;
  Int spOld = sp;

  for (i = t->numLuts-1; i >= 0; i--) lstack[lsp++] = t->luts[i];
  for (i = 0; i < t->numApps; i++)
    instApp(base, spOld-2, &(t->apps[i]));
  for (i = t->numPushs-1; i >= 0; i--)
    stack[sp++] = inst(base, spOld-2, t->pushs[i]);

  slide(spOld, t->arity+1);
}

/* Garbage collection */

Bool isSimple(App *app)
{
    return (getAppSize(*app) == 1 && getAppTag(*app) != CASE &&
            (isINT(getAppAtom(*app, 0)) || isCON(getAppAtom(*app, 0))));
}

Atom copyChild(Atom child)
{
  App app;
  if (isPTR(child)) {
    app = heap[getPTRId(child)];
    if (isAppCollected(app))
        return getAppCollectedAtom(app);
    else if (isSimple(&app))
        return getAppAtom(app, 0);
    else {
      Int addr = getPTRId(child);
      child = setPTRId(child, gcHigh);
      heap[addr] = mkAppCollected(child);
      heap2[gcHigh++] = app;
      return child;
    }
  }
  return child;
}

void copy()
{
  Int i;
  App app;
  while (gcLow < gcHigh) {
      Atom atoms[APSIZE];
      app = heap2[gcLow];
      for (i = 0; i < getAppSize(app); i++)
          atoms[i] = copyChild(getAppAtom(app, i));
      heap2[gcLow++] = mkApp(getAppTag(app),
                             getAppSize(app),
                             getAppNF(app),
                             getAppLUT(app),
                             atoms);
  }
}

void updateUStack()
{
  Int i, j;
  App app;
  for (i = 0, j = 0; i < usp; i++) {
    app = heap[ustack[i].haddr];
    if (isAppCollected(app)) {
      ustack[j].saddr = ustack[i].saddr;
      ustack[j].haddr = getPTRId(getAppCollectedAtom(app));
      j++;
    }
  }
  usp = j;
}

void collect()
{
  Int i;
  App* tmp;
  gcCount++;
  gcLow = gcHigh = 0;
  for (i = 0; i < sp; i++) stack[i] = copyChild(stack[i]);
  copy();
  updateUStack();
  tmp = heap; heap = heap2; heap2 = tmp;
  hp = gcHigh;
  //printf("After GC: %i\n", hp);
}

/* Allocate memory */

void alloc()
{
  heap = (App*) malloc(sizeof(App) * MAXHEAPAPPS);
  heap2 = (App*) malloc(sizeof(App) * MAXHEAPAPPS);
  stack = (Atom*) malloc(sizeof(Atom) * MAXSTACKELEMS);
  ustack = (Update*) malloc(sizeof(Update) * MAXSTACKELEMS);
  lstack = (Lut*) malloc(sizeof(Lut) * MAXSTACKELEMS);
  code = (Template*) malloc(sizeof(Template) * MAXTEMPLATES);
  registers = (Atom*) malloc(sizeof(Atom) * MAXREGS);
  profTable = (ProfEntry*) malloc(sizeof(ProfEntry) * MAXTEMPLATES);
}

/* Initialise globals */

void initProfTable()
{
  Int i;
  for (i = 0; i < MAXTEMPLATES; i++) {
    profTable[i].seen = 0;
    profTable[i].callCount = 0;
  }
}

void init()
{
  falseAtom = mkCON(0,0);
  trueAtom = mkCON(0,1);
  mainAtom = mkFUN(0,0,0);


  sp = 1;
  usp = lsp = hp = 0;
  stack[0] = mainAtom;
  swapCount = primCount = applyCount =
    unwindCount = updateCount = selectCount =
      prsCandidateCount = prsSuccessCount = gcCount = 0;
  initProfTable();
}

/* Dispatch loop */

static inline Bool canCollect()
{
    return !isFUN(stack[sp-1]) || getFUNOriginal(stack[sp-1]);
}

void stackOverflow(void)
{
    error("Out of stack space.");
}

void dispatch(void)
{
  Atom top;

  while (!(sp == 1 && isINT(stack[0]))) {
    if (sp > MAXSTACKELEMS-100) stackOverflow();
    if (usp > MAXSTACKELEMS-100) stackOverflow();
    if (lsp > MAXSTACKELEMS-100) stackOverflow();
    if (hp > MAXHEAPAPPS-200 && canCollect()) collect();
    top = stack[sp-1];
    if (isPTR(top)) {
      unwind(getPTRShared(top), getPTRId(top));
      unwindCount++;
    }
    else if (usp > 0 && updateCheck(top, ustack[usp-1])) {
      update(top, ustack[usp-1].saddr, ustack[usp-1].haddr);
      updateCount++;
    }
    else {
        if (isINT(top)) {
            assert(isPRI(stack[sp-2]));
            applyPrim();
        }
        else if (isCON(top)) {
            selectCount++;
            caseSelect(getCONIndex(top));
        }
        else if (isFUN(top)) {
            profTable[getFUNId(top)].callCount++;
            applyCount++;
            apply(&code[getFUNId(top)]);
        }
        else
            error("dispatch(): invalid tag.");
    }
  }
}

/* Parser for .red files */

Int strToBool(Char *s)
{
  if (!strcmp(s, "True")) return 1;
  if (!strcmp(s, "False")) return 0;
  error("Parse error: boolean expected; got %s", s);
  return 0;
}

void strToPrim(Char *s, Prim *p, Bool *b)
{
  *b = 0;
  if (!strcmp(s, "emit")) { *p = EMIT; return; }
  if (!strcmp(s, "emitInt")) { *p = EMITINT; return; }
  if (!strcmp(s, "(!)")) { *p = SEQ; return; }
  if (!strncmp(s, "swap:", 5)) {
    *b = 1;
    s = s+5;
  }
  if (!strcmp(s, "(+)")) { *p = ADD; return; }
  if (!strcmp(s, "(-)")) { *p = SUB; return; }
  if (!strcmp(s, "(==)")) { *p = EQ; return; }
  if (!strcmp(s, "(/=)")) { *p = NEQ; return; }
  if (!strcmp(s, "(<=)")) { *p = LEQ; return; }
  if (!strcmp(s, "(.&.)")) { *p = AND; return; }
  if (!strcmp(s, "st32")) { *p = ST32; return; }
  if (!strcmp(s, "ld32")) { *p = LD32; return; }
  error("Parse error: unknown primitive %s", s);
}

Bool parseAtom(FILE *f, Atom* result)
{
  Char str[16];
  Int v;
  Int a, i;
  Bool swap;
  Prim p;

  return (
    (  fscanf(f, " INT%*[ (]%i)", &v) == 1
    && perform(*result = mkINT(v))
    )
    ||
    (  fscanf(f, " ARG %5s%*[ (]%i)", str, &i) == 2
    && perform(*result = mkARG(strToBool(str), i))
    )
    ||
    (  fscanf(f, " VAR %5s%*[ (]%i)", str, &i) == 2
    && perform(*result = mkPTR(strToBool(str), i))
    )
    ||
    (  fscanf(f, " REG %5s%*[ (]%i)", str, &i) == 2
    && perform(*result = mkREG(strToBool(str), i))
    )
    ||
    (  fscanf(f, " CON%*[ (]%i%*[ )(]%i)",
         &a, &i) == 2
    && perform(*result = mkCON(a,i))
    )
    ||
    (  fscanf(f, " FUN %5s%*[ (]%i%*[ )(]%i)",
         str, &a, &i) == 3
    && perform(*result = mkFUN(strToBool(str), a, i))
    )
    ||
    (  fscanf(f, " PRI%*[ (]%i%*[) ]\"%10[^\"]\"", &a, str)
    && perform(strToPrim(str, &p, &swap))
    && perform(*result = mkPRI(a, swap, p))
    )
  );
}

#define makeListParser(fun, p, elem)                                        \
  Int fun(FILE *f, Int n, elem *xs)                                         \
    {                                                                       \
      Char c;                                                               \
      Int i = 0;                                                            \
      if (! (fscanf(f, " %c", &c) == 1 && c == '['))                        \
        error("Parse error: expecting '['");                                \
      for (;;) {                                                            \
        if (i >= n)                                                         \
          error("Parse error: list contains too many elements");            \
        if (p(f, &xs[i])) i++;                                              \
        if (fscanf(f, " %c", &c) == 1 && (c == ',' || c == ']')) {          \
          if (c == ']') return i;                                           \
        }                                                                   \
        else error("Parse error");                                          \
      }                                                                     \
      return 0;                                                             \
    }

makeListParser(parseAtoms, parseAtom, Atom)

Bool parseApp(FILE *f, App *app)
{
  Char str[16];
  Bool success;
  Atom atoms[APSIZE];
  AppTag tag;
  Bool nf = false;
  Int info = 0, size;

  success =
    (  fscanf(f, " APP %5s ", str) == 1
    && perform(tag = AP)
    && perform(nf = strToBool(str))
    )
    ||
    (  fscanf(f, " CASE %i ", &info) == 1
    && perform(tag = CASE)
    )
    ||
    (  fscanf(f, " PRIM %i ", &info) == 1
    && perform(tag = PRIM)
    );
  if (!success) return 0;
  size = parseAtoms(f, APSIZE, atoms);

  if (tag == CASE || tag == PRIM)
      assert(size < 4);

  *app = mkApp(tag, size, nf, info, atoms);

  return 1;
}

makeListParser(parseApps, parseApp, App)

Bool parseLut(FILE *f, Int *i)
{
    return fscanf(f, " %i", i) == 1;
}

makeListParser(parseLuts, parseLut, Lut)

Bool parseString(FILE *f, Int n, Char *str)
{
  Int i;
  Char c;
  if (fscanf(f, " \"") != 0)
      return 0;

  for (i = 0; ; i++) {
    if (i >= n || fscanf(f, "%c", &c) != 1) return 0;
    if (c == '"') {
      str[i] = '\0';
      return 1;
    }
    str[i] = c;
  }
}

Bool parseTemplate(FILE *f, Template *t)
{
  Char c;
  if (fscanf(f, " (") != 0)
      return 0;
  if (parseString(f, NAMELEN, t->name) == 0) return 0;
  if (fscanf(f, " ,%i,", &t->arity) != 1) return 0;
  t->numLuts = parseLuts(f, MAXLUTS, t->luts);
  if (!(fscanf(f, " %c", &c) == 1 && c == ',')) error("Parse error");
  t->numPushs = parseAtoms(f, MAXPUSH, t->pushs);
  if (!(fscanf(f, " %c", &c) == 1 && c == ',')) error("Parse error");
  t->numApps = parseApps(f, MAXAPS, t->apps);
  if (!(fscanf(f, " %c", &c) == 1 && c == ')')) error("Parse error");
  return 1;
}

Int parse(FILE *f, Int n, Template *ts)
{
  Int i = 0;

  for (;;) {
      if (i >= n) error("Parse error: too many templates");
    if (!parseTemplate(f, &ts[i])) return i;
    i++;
  }
}

/* Main function */

int main(int argc, char **argv)
{
  FILE *f;
  Long ticks;
  int ch;
  Bool verbose = 0;

  while ((ch = getopt(argc, argv, "vt")) != -1) {
      switch (ch) {
      case 'v':
          verbose = 1;
          break;
      case 't':
          tracingEnabled = 1;
          break;
      default:
          error("only options v, t and p supported");
          break;
      }
  }

  argc -= optind;
  argv += optind;

  if (argc != 1)
      error("Need .red file or - for stdin");

  if (strcmp(argv[0], "-") == 0)
      f = stdin;
  else
      f = fopen(argv[0], "r");

  if (!f) {
      perror(argv[0]);
      exit(-1);
  }

  alloc();
  numTemplates = parse(f, MAXTEMPLATES, code);
  if (numTemplates <= 0) error("No templates were parsed!");
  init();
  dispatch();

  if (verbose) {
      printf("\n==== EXECUTION REPORT ====\n");
      printf("Result      = %12i\n", getINTValue(stack[0]));
      ticks = swapCount + primCount + applyCount +
          unwindCount + updateCount;
      printf("Ticks       = %12lld\n", ticks);
      printf("Swap        = %11.1f%%\n", (100.0*swapCount)/ticks);
      printf("Prim        = %11.1f%%\n", (100.0*primCount)/ticks);
      printf("Unwind      = %11.1f%%\n", (100.0*unwindCount)/ticks);
      printf("Update      = %11.1f%%\n", (100.0*updateCount)/ticks);
      printf("Apply       = %11.1f%%\n", (100.0*applyCount)/ticks);
      printf("PRS Success = %11.1f%%\n",
             (100.0*prsSuccessCount)/(1+prsCandidateCount));
      printf("#GCs        = %12d\n", gcCount);
      printf("==========================\n");
  }
  else
      printf("%d\n", getINTValue(stack[0]));

  //  displayProfTable();

  return 0;
}
