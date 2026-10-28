// Alan Makhmudov 251ADB106
// (optional) https://github.com/Aln-m17/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: AI: Claude Code (Anthropic, model Claude Opus 5.5)

// opendir/readdir/mkdir/stat are POSIX, not plain C17. With -std=c17 glibc
// hides POSIX declarations unless we ask for them explicitly.
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

// Used to build the output file and folder names (see section 4).
#define FIRST_NAME "alan"
#define LAST_NAME  "makhmudov"
#define STUDENT_ID "251ADB106"

/* ------------------------------------------------------------------------ */
/* Growable byte buffer                                                     */
/* ------------------------------------------------------------------------ */

// Both the input file and the output text live in one of these. The capacity
// doubles whenever it runs out, so appending n bytes in total costs O(n)
// amortised: a multi-megabyte file is copied only log2(size) times while
// reading, never once per line or per result.
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) {
        // Nothing sensible can be done without memory; stop cleanly.
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    b->data = p;
    b->cap = cap;
}

static void buf_append(Buf *b, const char *s, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

// Reads the whole file in one go. Having all of it in memory lets the
// tokenizer use plain indexes into the buffer as positions, and an error
// position can then be reported as "index + 1" without any extra counting.
static int read_file(const char *path, Buf *b)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    for (;;) {
        buf_reserve(b, 65536);
        size_t n = fread(b->data + b->len, 1, b->cap - b->len, f);
        b->len += n;
        if (n == 0)
            break;
    }
    int ok = !ferror(f);
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Tokenizer                                                                */
/* ------------------------------------------------------------------------ */

typedef enum {
    T_INT, T_FLOAT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN,
    T_END,  // end of the current line
    T_BAD   // a character that cannot start any token
} TokKind;

typedef struct {
    TokKind kind;
    size_t start;  // 0-based index into the whole file of the token's first char
    long long ival;
    double fval;
} Token;

// A value is either an integer or a double. Keeping the flag instead of
// converting everything to double is what makes 7 / 2 give 3 and lets big
// integers stay exact.
typedef struct {
    int is_float;
    long long i;
    double f;
} Value;

// One operand of a chain a ** b ** c, see parse_power().
typedef struct {
    Value v;
    size_t start;  // where this operand starts (for "negative exponent")
    size_t op;     // where the ** after it starts (for "result is inf/nan")
} PowItem;

typedef struct {
    const char *src;  // the whole file
    size_t pos;       // next unread index in src
    size_t end;       // index of this line's '\n', or file length if none
    Token tok;        // current token (one token of look-ahead)

    int failed;
    size_t err;       // 0-based index of the first error

    PowItem *pw;      // shared operand stack for ** chains
    size_t pw_len;
    size_t pw_cap;
} Parser;

static int is_blank(char c)
{
    // '\r' is treated as blank so files saved with Windows line endings work.
    return c == ' ' || c == '\t' || c == '\r';
}

// strtod needs a NUL-terminated string, but the file buffer is not
// terminated after each number, so the token is copied out first. The
// tokenizer has already checked the "digits . digits" shape, so strtod only
// does the decimal-to-binary rounding, which is easy to get wrong by hand.
static double parse_float(const char *s, size_t n)
{
    char small[64];
    char *tmp = n < sizeof small ? small : malloc(n + 1);
    if (!tmp) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    double d = strtod(tmp, NULL);
    if (tmp != small)
        free(tmp);
    return d;
}

// Reads the next token of the current line into p->tok.
//
// Position tracking: p->pos is an index into the whole file, not into the
// line, so it already counts every earlier character, including all '\n's
// and comment lines. Each token stores the index of its first character;
// the 1-based position the rules ask for is just that index + 1.
//
// The end of the line is a token too (T_END). Its start is the index of the
// '\n', or the file length when the last line has no '\n'; +1 then gives
// exactly "the line's \n (file length + 1 if no \n)" from the rules.
//
// A bad character is not reported here. It becomes a T_BAD token and the
// error is raised only when the parser actually looks at it and cannot use
// it, which is what "the token the parser was looking at" means. So in
// "1 / 0 &" the division by zero is found first.
static void next_token(Parser *p)
{
    const char *s = p->src;
    while (p->pos < p->end && is_blank(s[p->pos]))
        p->pos++;

    Token *t = &p->tok;
    t->start = p->pos;
    if (p->pos >= p->end) {
        t->kind = T_END;
        return;
    }

    char c = s[p->pos];
    if (isdigit((unsigned char)c)) {
        size_t i = p->pos;
        // Unsigned so that the accumulation itself can never be undefined
        // behaviour; the tests promise the value fits in long long.
        unsigned long long v = 0;
        while (i < p->end && isdigit((unsigned char)s[i])) {
            v = v * 10 + (unsigned long long)(s[i] - '0');
            i++;
        }
        if (i < p->end && s[i] == '.') {
            if (i + 1 < p->end && isdigit((unsigned char)s[i + 1])) {
                size_t j = i + 1;
                while (j < p->end && isdigit((unsigned char)s[j]))
                    j++;
                t->kind = T_FLOAT;
                t->fval = parse_float(s + p->pos, j - p->pos);
                p->pos = j;
                return;
            }
            // "5." has no digits after the point. The rules put this error
            // at the '.', so the bad token starts there, not at the '5'.
            t->kind = T_BAD;
            t->start = i;
            p->pos = i;
            return;
        }
        t->kind = T_INT;
        t->ival = (long long)v;
        p->pos = i;
        return;
    }

    // Everything else is a one-character token, except "**". Note that ".5"
    // and "1.5e3" need no special case: '.' and 'e' are simply bad
    // characters, which puts the error exactly where the rules want it.
    p->pos++;
    switch (c) {
    case '+': t->kind = T_PLUS; break;
    case '-': t->kind = T_MINUS; break;
    case '/': t->kind = T_SLASH; break;
    case '(': t->kind = T_LPAREN; break;
    case ')': t->kind = T_RPAREN; break;
    case '*':
        if (p->pos < p->end && s[p->pos] == '*') {
            p->pos++;
            t->kind = T_POW;
        } else {
            t->kind = T_STAR;
        }
        break;
    default:
        // Includes '#' in the middle of a line: comments are only
        // recognised at the start of a line, in eval_buffer().
        t->kind = T_BAD;
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Arithmetic                                                               */
/* ------------------------------------------------------------------------ */

// Records the first error and returns 0 so callers can write
// "return fail(...)" and every parse function unwinds immediately. Only the
// first error matters because the output then contains nothing else.
static int fail(Parser *p, size_t at)
{
    p->failed = 1;
    p->err = at;
    return 0;
}

// x ** e for e >= 0 by repeated squaring (O(log e) multiplications instead
// of e). The arithmetic is done in unsigned long long because unsigned
// overflow is defined (wraps mod 2^64), so the sanitizer never complains,
// and when the true result fits in long long the wrapped result is the same
// bit pattern anyway. x ** 0 is 1 for every x, including 0 ** 0.
static long long int_pow(long long x, long long e)
{
    unsigned long long result = 1;
    unsigned long long base = (unsigned long long)x;
    while (e > 0) {
        if (e & 1)
            result *= base;
        e >>= 1;
        if (e > 0)
            base *= base;
    }
    return (long long)result;
}

// Applies one binary operator. op_pos is where the operator starts and
// rhs_start where its right operand starts; they are passed in because the
// rules report different errors at different places:
//   - division by zero and a negative integer exponent are a problem with
//     the right operand, so they are reported at its start;
//   - an infinite/NaN float result is caused by the operation itself, so it
//     is reported at the operator.
//
// Integer or float: if both operands are integers the operation stays in
// long long (C semantics, so / truncates toward zero). If either one is a
// float, both are converted to double first. This is the usual C rule
// ("usual arithmetic conversions") and matches 7 / 2.0 == 3.5.
static int apply(Parser *p, char op, Value a, Value b,
                 size_t op_pos, size_t rhs_start, Value *out)
{
    if (!a.is_float && !b.is_float) {
        // +, -, * via unsigned for the same reason as in int_pow(): no
        // undefined behaviour even on overflow (which the tests never do).
        unsigned long long x = (unsigned long long)a.i;
        unsigned long long y = (unsigned long long)b.i;
        long long r;
        switch (op) {
        case '+': r = (long long)(x + y); break;
        case '-': r = (long long)(x - y); break;
        case '*': r = (long long)(x * y); break;
        case '/':
            if (b.i == 0)
                return fail(p, rhs_start);
            // LLONG_MIN / -1 overflows and is undefined in C; dividing by
            // -1 is just negation, which is safe in unsigned arithmetic.
            r = b.i == -1 ? (long long)(0 - x) : a.i / b.i;
            break;
        default: // '^' stands for **
            if (b.i < 0)
                return fail(p, rhs_start);
            r = int_pow(a.i, b.i);
            break;
        }
        out->is_float = 0;
        out->i = r;
        return 1;
    }

    double x = a.is_float ? a.f : (double)a.i;
    double y = b.is_float ? b.f : (double)b.i;
    double r;
    switch (op) {
    case '+': r = x + y; break;
    case '-': r = x - y; break;
    case '*': r = x * y; break;
    case '/':
        if (y == 0.0)
            return fail(p, rhs_start);
        r = x / y;
        break;
    default:
        // A negative exponent is fine with floats: 0.5 ** (0 - 1) == 2.
        r = pow(x, y);
        break;
    }
    // Catches 10.0 ** 400 (inf), (0 - 8) ** 0.5 (nan), 0.0 ** (0 - 1)
    // (inf), and also overflow from + - * /.
    if (!isfinite(r))
        return fail(p, op_pos);
    out->is_float = 1;
    out->f = r;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Parser (recursive descent, evaluates while parsing)                      */
/* ------------------------------------------------------------------------ */

// Grammar, one function per precedence level:
//
//   expr    := term  { ('+' | '-') term }
//   term    := power { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
//
// Why this gives the right precedence: a lower-precedence function can only
// see the results of the higher-precedence one below it. In 3 + 2 * 5,
// expr asks term for its left operand (3), then for its right operand, and
// term greedily takes the whole "2 * 5" before expr gets to add. So * binds
// tighter than +, and ** tighter than both, without any precedence table.
//
// Why + - * / are left-associative: expr and term use a loop that folds
// each new operand into the running result, so 8 - 3 - 2 = (8 - 3) - 2.
//
// There are no unary operators: primary accepts only a number or '(', so a
// leading '-' (as in "-5" or "2 * -3") is reported at that '-'. The same
// check also produces "two operators in a row" (error at the second one)
// and "missing operand at end of line" (error at the T_END token).
//
// Values are computed while parsing, so the parser stops at the first
// problem of any kind, syntax or arithmetic, in reading order.

static int parse_expr(Parser *p, Value *out);

static void push_pow(Parser *p, Value v, size_t start)
{
    if (p->pw_len == p->pw_cap) {
        size_t cap = p->pw_cap ? p->pw_cap * 2 : 16;
        PowItem *n = realloc(p->pw, cap * sizeof *n);
        if (!n) {
            fprintf(stderr, "calc: out of memory\n");
            exit(1);
        }
        p->pw = n;
        p->pw_cap = cap;
    }
    p->pw[p->pw_len].v = v;
    p->pw[p->pw_len].start = start;
    p->pw[p->pw_len].op = 0;
    p->pw_len++;
}

static int parse_primary(Parser *p, Value *out)
{
    Token t = p->tok;
    switch (t.kind) {
    case T_INT:
        out->is_float = 0;
        out->i = t.ival;
        next_token(p);
        return 1;
    case T_FLOAT:
        out->is_float = 1;
        out->f = t.fval;
        next_token(p);
        return 1;
    case T_LPAREN:
        next_token(p);
        // Recursion here is what lets parentheses override precedence: the
        // inner expression is fully evaluated before anything outside sees
        // it. Depth is one C call chain per '(' (1000 deep is fine).
        if (!parse_expr(p, out))
            return 0;
        // Missing ')': we are looking at whatever came instead, usually
        // the end of the line, so the error goes there.
        if (p->tok.kind != T_RPAREN)
            return fail(p, p->tok.start);
        next_token(p);
        return 1;
    default:
        // We needed an operand and got something else: an operator, ')',
        // a bad character or the end of the line. That token is where the
        // parser could not continue, so that is the error position.
        return fail(p, t.start);
    }
}

// power := primary [ '**' power ]
//
// ** is right-associative: 2 ** 3 ** 2 must be 2 ** (3 ** 2) = 512. The
// grammar says power calls itself for the exponent, so the rightmost **
// would be evaluated first. Written literally, that recursion goes one C
// call deeper per '**', and a multi-megabyte line like "1 ** 1 ** 1 ..."
// would overflow the stack.
//
// So this does the same thing with a loop: first read all operands of the
// chain left to right (exactly the order the recursive version reads them),
// pushing them on p->pw, then fold from the right: c, then b ** c, then
// a ** (b ** c). The result and the error found first are identical to the
// recursive version, because that one also reads every operand before
// doing its first **, and then evaluates from the right as calls return.
//
// The stack is shared by all nested calls: a primary in parentheses may
// contain its own ** chain, which pushes above our items and pops back to
// its own base before we push our next item, so the items never mix.
static int parse_power(Parser *p, Value *out)
{
    size_t base = p->pw_len;
    for (;;) {
        size_t start = p->tok.start;
        Value v;
        if (!parse_primary(p, &v))
            return 0;  // pw_len is reset for every line, no cleanup needed
        push_pow(p, v, start);
        if (p->tok.kind != T_POW)
            break;
        p->pw[p->pw_len - 1].op = p->tok.start;
        next_token(p);
    }

    Value r = p->pw[p->pw_len - 1].v;
    for (size_t k = p->pw_len - 1; k-- > base;) {
        // The exponent of the k-th ** is everything to its right, which
        // starts where operand k+1 starts: that is "the start of the
        // exponent" for the negative-exponent error.
        if (!apply(p, '^', p->pw[k].v, r, p->pw[k].op, p->pw[k + 1].start, &r))
            return 0;
    }
    p->pw_len = base;
    *out = r;
    return 1;
}

// term := power { ('*' | '/') power }
static int parse_term(Parser *p, Value *out)
{
    if (!parse_power(p, out))
        return 0;
    while (p->tok.kind == T_STAR || p->tok.kind == T_SLASH) {
        char op = p->tok.kind == T_STAR ? '*' : '/';
        size_t op_pos = p->tok.start;
        next_token(p);
        size_t rhs_start = p->tok.start;
        Value rhs;
        if (!parse_power(p, &rhs))
            return 0;
        if (!apply(p, op, *out, rhs, op_pos, rhs_start, out))
            return 0;
    }
    return 1;
}

// expr := term { ('+' | '-') term }
static int parse_expr(Parser *p, Value *out)
{
    if (!parse_term(p, out))
        return 0;
    while (p->tok.kind == T_PLUS || p->tok.kind == T_MINUS) {
        char op = p->tok.kind == T_PLUS ? '+' : '-';
        size_t op_pos = p->tok.start;
        next_token(p);
        size_t rhs_start = p->tok.start;
        Value rhs;
        if (!parse_term(p, &rhs))
            return 0;
        if (!apply(p, op, *out, rhs, op_pos, rhs_start, out))
            return 0;
    }
    return 1;
}

// Evaluates the line src[start .. end), where end is the '\n' index (or the
// file length).
static int eval_line(Parser *p, size_t start, size_t end, Value *out)
{
    p->pos = start;
    p->end = end;
    p->pw_len = 0;
    next_token(p);
    if (!parse_expr(p, out))
        return 0;
    // A whole expression was read but the line goes on, e.g. "(1 + 2))" or
    // "1 2": the leftover token is what the parser could not use.
    if (p->tok.kind != T_END)
        return fail(p, p->tok.start);
    return 1;
}

// Evaluates every line of a file and puts the text of the output file in
// out. Results are collected in memory and not written as we go, because
// one error anywhere means the file must contain only "ERROR:<pos>":
// dropping the partial results is then just out->len = 0.
static void eval_buffer(const char *src, size_t len, Buf *out)
{
    Parser p = {0};
    p.src = src;
    char text[64];

    size_t pos = 0;
    while (pos < len) {
        // memchr finds the end of the line in one fast pass; each character
        // of the file is looked at a constant number of times overall, so
        // the whole run is linear in the file size.
        const char *nl = memchr(src + pos, '\n', len - pos);
        size_t end = nl ? (size_t)(nl - src) : len;

        size_t first = pos;
        while (first < end && is_blank(src[first]))
            first++;
        // Blank lines and comment lines produce no output, but they are
        // still part of src, so positions after them stay correct.
        if (first < end && src[first] != '#') {
            Value v;
            if (!eval_line(&p, pos, end, &v)) {
                out->len = 0;
                int n = snprintf(text, sizeof text, "ERROR:%zu\n", p.err + 1);
                buf_append(out, text, (size_t)n);
                break;
            }
            int n = v.is_float ? snprintf(text, sizeof text, "%.15g\n", v.f)
                               : snprintf(text, sizeof text, "%lld\n", v.i);
            buf_append(out, text, (size_t)n);
        }
        pos = end + 1;
    }
    free(p.pw);
}

/* ------------------------------------------------------------------------ */
/* Files, folders and the command line                                      */
/* ------------------------------------------------------------------------ */

static char *xstrdup_n(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (!r) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

// Last path component, ignoring trailing slashes ("labs/" -> "labs").
static char *path_name(const char *path)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    size_t b = n;
    while (b > 0 && path[b - 1] != '/')
        b--;
    return xstrdup_n(path + b, n - b);
}

// "labs/task1.txt" -> "task1"
static char *input_base(const char *path)
{
    char *name = path_name(path);
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".txt") == 0)
        name[n - 4] = '\0';
    return name;
}

static int ends_with_txt(const char *name)
{
    size_t n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, ".txt") == 0;
}

static int is_regular_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Creates the output folder if needed. Its parent is guaranteed to exist,
// so a single mkdir is enough (no "mkdir -p").
static int ensure_dir(const char *path)
{
    if (is_directory(path))
        return 1;
    if (mkdir(path, 0777) != 0) {
        fprintf(stderr, "calc: cannot create output folder '%s': %s\n",
                path, strerror(errno));
        return 0;
    }
    return 1;
}

// "<base>_alan_<id>": the default output folder when -o is not given.
static char *default_out_dir(const char *base)
{
    size_t n = strlen(base) + strlen(FIRST_NAME) + strlen(STUDENT_ID) + 3;
    char *r = malloc(n);
    if (!r) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    snprintf(r, n, "%s_%s_%s", base, FIRST_NAME, STUDENT_ID);
    return r;
}

// Evaluates one input file and writes <out_dir>/<base>_alan_makhmudov_<id>.txt.
static int process_file(const char *in_path, const char *out_dir)
{
    Buf in = {0}, out = {0};
    if (!read_file(in_path, &in)) {
        fprintf(stderr, "calc: cannot read input file '%s': %s\n",
                in_path, strerror(errno));
        free(in.data);
        return 0;
    }
    eval_buffer(in.data ? in.data : "", in.len, &out);
    free(in.data);

    char *base = input_base(in_path);
    size_t n = strlen(out_dir) + strlen(base) + strlen(FIRST_NAME)
               + strlen(LAST_NAME) + strlen(STUDENT_ID) + 9;
    char *path = malloc(n);
    if (!path) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    snprintf(path, n, "%s/%s_%s_%s_%s.txt",
             out_dir, base, FIRST_NAME, LAST_NAME, STUDENT_ID);
    free(base);

    int ok = 1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "calc: cannot write output file '%s': %s\n",
                path, strerror(errno));
        ok = 0;
    } else {
        if (out.len > 0 && fwrite(out.data, 1, out.len, f) != out.len)
            ok = 0;
        // fclose flushes, so a full disk may only show up here.
        if (fclose(f) != 0)
            ok = 0;
        if (!ok)
            fprintf(stderr, "calc: error writing output file '%s'\n", path);
    }
    free(path);
    free(out.data);
    return ok;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// calc -d DIR: every regular *.txt file directly inside DIR. The names are
// collected (and sorted, for a predictable order) before any output is
// written, so output files are never picked up as inputs even when OUTDIR
// is DIR itself.
static int process_dir(const char *dir, const char *out_dir_opt)
{
    if (!is_directory(dir)) {
        fprintf(stderr, "calc: input folder '%s' does not exist\n", dir);
        return 0;
    }
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n",
                dir, strerror(errno));
        return 0;
    }

    char **paths = NULL;
    size_t count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with_txt(e->d_name))
            continue;
        size_t n = strlen(dir) + strlen(e->d_name) + 2;
        char *path = malloc(n);
        if (!path) {
            fprintf(stderr, "calc: out of memory\n");
            exit(1);
        }
        snprintf(path, n, "%s/%s", dir, e->d_name);
        // Subfolders that happen to be called *.txt are skipped too.
        if (!is_regular_file(path)) {
            free(path);
            continue;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            char **np = realloc(paths, cap * sizeof *np);
            if (!np) {
                fprintf(stderr, "calc: out of memory\n");
                exit(1);
            }
            paths = np;
        }
        paths[count++] = path;
    }
    closedir(d);
    if (count > 0)
        qsort(paths, count, sizeof *paths, cmp_names);

    char *out_dir = NULL;
    if (out_dir_opt) {
        out_dir = xstrdup_n(out_dir_opt, strlen(out_dir_opt));
    } else {
        char *base = path_name(dir);
        out_dir = default_out_dir(base);
        free(base);
    }

    int ok = ensure_dir(out_dir);
    for (size_t i = 0; i < count; i++) {
        if (ok && !process_file(paths[i], out_dir))
            ok = 0;
        free(paths[i]);
    }
    free(paths);
    free(out_dir);
    return ok;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR]"
            " [input.txt ...]\n");
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *out_dir = NULL;
    int nfiles = 0;

    if (argc < 2) {
        usage();
        return 2;
    }

    // First pass: options only. Input file names are left in argv and
    // counted, then handled in the second pass below.
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0 ||
            strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs an argument\n", a);
                usage();
                return 2;
            }
            if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0)
                dir = argv[++i];
            else
                out_dir = argv[++i];
        } else if (strncmp(a, "--dir=", 6) == 0) {
            dir = a + 6;
        } else if (strncmp(a, "--output-dir=", 13) == 0) {
            out_dir = a + 13;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 2;
        } else {
            nfiles++;
        }
    }

    if (!dir && nfiles == 0) {
        fprintf(stderr, "calc: no input file or folder given\n");
        usage();
        return 2;
    }
    if (dir && nfiles > 0) {
        fprintf(stderr, "calc: give either -d DIR or input files, not both\n");
        usage();
        return 2;
    }

    if (dir)
        return process_dir(dir, out_dir) ? 0 : 1;

    int ok = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0 ||
            strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0) {
            i++;  // skip the option's argument
            continue;
        }
        if (a[0] == '-' && a[1] != '\0')
            continue;

        // Checked before creating any folder, so a typo in the input name
        // does not leave an empty output folder behind.
        if (!is_regular_file(a)) {
            fprintf(stderr, "calc: input file '%s' does not exist\n", a);
            ok = 0;
            continue;
        }
        char *dest;
        if (out_dir) {
            dest = xstrdup_n(out_dir, strlen(out_dir));
        } else {
            char *base = input_base(a);
            dest = default_out_dir(base);
            free(base);
        }
        if (!ensure_dir(dest) || !process_file(a, dest))
            ok = 0;
        free(dest);
    }
    return ok ? 0 : 1;
}
