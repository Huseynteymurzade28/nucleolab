#include "scpi.h"
#include <string.h>

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* ---------- errors and status ---------- */
static const struct { int16_t code; const char *text; } errors[] = {
    {0, "No error"}, {-101, "Invalid character"}, {-102, "Syntax error"},
    {-104, "Data type error"}, {-108, "Parameter not allowed"},
    {-109, "Missing parameter"}, {-112, "Program mnemonic too long"},
    {-113, "Undefined header"}, {-114, "Header suffix out of range"},
    {-131, "Invalid suffix"}, {-138, "Suffix not allowed"},
    {-151, "Invalid string data"}, {-160, "Block data error"}, {-161, "Invalid block data"},
    {-211, "Trigger ignored"}, {-213, "Init ignored"}, {-214, "Trigger deadlock"},
    {-230, "Data corrupt or stale"},
    {-221, "Settings conflict"},
    {-222, "Data out of range"}, {-224, "Illegal parameter value"},
    {-240, "Hardware error"}, {-350, "Queue overflow"},
    {-363, "Input buffer overrun"},
};

const char *scpi_error_text(int code)
{
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++)
        if (errors[i].code == code) return errors[i].text;
    return "Unknown error";
}

void scpi_error(scpi_t *s, int code)
{
    s->failed = true;
    if (code <= -100 && code > -200) { s->esr |= ESR_CME; s->cmd_error = true; }
    else if (code <= -200 && code > -300) s->esr |= ESR_EXE;
    else if (code <= -400 && code > -500) s->esr |= ESR_QYE;
    else s->esr |= ESR_DDE;
    if (s->err_count == SCPI_ERRQ_LEN) { /* newest entry becomes -350 */
        s->errq[(s->err_head + SCPI_ERRQ_LEN - 1) % SCPI_ERRQ_LEN] = -350;
        return;
    }
    s->errq[(s->err_head + s->err_count) % SCPI_ERRQ_LEN] = (int16_t)code;
    s->err_count++;
}

uint8_t scpi_stb(const scpi_t *s)
{
    uint8_t b = 0;
    if (s->err_count) b |= 0x04;
    if (s->ques.event & s->ques.enable) b |= 0x08;
    if (s->out_units) b |= 0x10;
    if (s->esr & s->ese) b |= 0x20;
    if (s->oper.event & s->oper.enable) b |= 0x80;
    if (b & s->sre & 0xBF) b |= 0x40;
    return b;
}

/* ---------- output ---------- */
void scpi_out_raw(scpi_t *s, const char *p, size_t n) { s->port->write(p, n); }

static void out_begin(scpi_t *s)
{
    if (s->out_items) scpi_out_raw(s, ",", 1);
    else if (s->out_units) scpi_out_raw(s, ";", 1);
    if (!s->out_items) s->out_units++;
    s->out_items++;
}

static size_t utoa10(uint32_t v, char *b)
{
    char t[10]; size_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) b[i] = t[n - 1 - i];
    return n;
}

static void put_int(scpi_t *s, int32_t v)
{
    char b[12]; size_t n = 0;
    uint32_t u = (uint32_t)v;
    if (v < 0) { b[n++] = '-'; u = 0u - u; }
    n += utoa10(u, b + n);
    scpi_out_raw(s, b, n);
}

void scpi_out_int(scpi_t *s, int32_t v) { out_begin(s); put_int(s, v); }

void scpi_out_num(scpi_t *s, double v)
{
    out_begin(s);
    if (v != v) { scpi_out_raw(s, "9.91E+37", 8); return; }  /* SCPI NaN */
    bool neg = v < 0;
    if (neg) v = -v;
    if (v > 9.9e37) { scpi_out_raw(s, neg ? "-9.9E+37" : "9.9E+37", neg ? 8 : 7); return; }
    int e = 0;
    if (v > 0 && v < 1e-37) v = 0;
    if (v > 0) {
        while (v >= 10.0) { v /= 10.0; e++; }
        while (v < 1.0) { v *= 10.0; e--; }
    }
    uint32_t m = (uint32_t)(v * 1e6 + 0.5);
    if (m >= 10000000u) { m /= 10u; e++; }
    char b[16]; size_t n = 0;
    if (neg) b[n++] = '-';
    b[n++] = (char)('0' + m / 1000000u);
    b[n++] = '.';
    for (uint32_t d = 100000u; d; d /= 10u) b[n++] = (char)('0' + m / d % 10u);
    b[n++] = 'E';
    b[n++] = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    b[n++] = (char)('0' + e / 10);
    b[n++] = (char)('0' + e % 10);
    scpi_out_raw(s, b, n);
}

void scpi_out_text(scpi_t *s, const char *t) { out_begin(s); scpi_out_raw(s, t, strlen(t)); }

void scpi_out_quoted(scpi_t *s, const char *t)
{
    out_begin(s);
    scpi_out_raw(s, "\"", 1);
    for (; *t; t++) {
        if (*t == '"') scpi_out_raw(s, "\"", 1);
        scpi_out_raw(s, t, 1);
    }
    scpi_out_raw(s, "\"", 1);
}

void scpi_out_block(scpi_t *s, size_t n)
{
    char b[14] = "#";
    size_t d = utoa10((uint32_t)n, b + 2);
    b[1] = (char)('0' + d);
    out_begin(s);
    scpi_out_raw(s, b, d + 2);
}

/* ---------- matching ---------- */
/* Pattern word "VOLTage" accepts exactly VOLT or VOLTAGE, in any case. */
static bool match_word(const char *p, size_t pn, const char *h, size_t hn)
{
    size_t sn = 0;
    while (sn < pn && !(p[sn] >= 'a' && p[sn] <= 'z')) sn++;
    if (hn != sn && hn != pn) return false;
    for (size_t i = 0; i < hn; i++) if (up(p[i]) != up(h[i])) return false;
    return true;
}

bool scpi_match_keyword(scpi_str_t v, const char *pattern)
{
    return match_word(pattern, strlen(pattern), v.p, v.n);
}

/* h points at the next node (no leading colon); p may begin with [ or : */
static bool match_path(scpi_t *s, const char *p, const char *h, const char *he)
{
    bool opt = *p == '[';
    if (opt) p++;
    if (*p == ':') p++;
    if (!*p || *p == '?') return h == he;
    const char *pe = p;
    while (*pe && !strchr(":[]#?", *pe)) pe++;
    const char *rest = pe;
    bool num = *rest == '#';
    if (num) rest++;
    if (opt && *rest == ']') rest++;

    if (h < he) {
        const char *ne = h;
        while (ne < he && *ne != ':') ne++;
        const char *we = ne;
        uint32_t val = 1;
        if (num) {
            while (we > h && is_digit(we[-1])) we--;
            if (we < ne) {
                val = 0;
                for (const char *q = we; q < ne; q++) {
                    val = val * 10u + (uint32_t)(*q - '0');
                    if (val > 99999u) val = 99999u;
                }
            }
        }
        if (match_word(p, (size_t)(pe - p), h, (size_t)(we - h))) {
            int saved = s->nsuffix;
            if (num && s->nsuffix < SCPI_SUFFIX_MAX) s->suffix[s->nsuffix++] = val;
            const char *nh = ne < he ? ne + 1 : ne;
            if (!(ne < he && nh == he) && match_path(s, rest, nh, he)) return true;
            s->nsuffix = saved;
        }
    }
    if (!opt) return false;
    int saved = s->nsuffix;
    if (num && s->nsuffix < SCPI_SUFFIX_MAX) s->suffix[s->nsuffix++] = 1;
    if (match_path(s, rest, h, he)) return true;
    s->nsuffix = saved;
    return false;
}

static bool match_cmd(scpi_t *s, const char *pat, const char *h, size_t n, bool query)
{
    if ((pat[strlen(pat) - 1] == '?') != query) return false;
    s->nsuffix = 0;
    return match_path(s, pat, h, h + n);
}

/* ---------- parameters ---------- */
bool scpi_nparams(scpi_t *s, int min, int max)
{
    if (s->nparam < min) { scpi_error(s, -109); return false; }
    if (s->nparam > max) { scpi_error(s, -108); return false; }
    return true;
}

/* Decimal numeric prefix of v; returns characters used, 0 if not a number. */
static size_t parse_num(scpi_str_t v, double *out)
{
    size_t i = 0; bool neg = false, any = false; double m = 0; int e = 0;
    if (i < v.n && (v.p[i] == '+' || v.p[i] == '-')) neg = v.p[i++] == '-';
    for (; i < v.n && is_digit(v.p[i]); i++) { m = m * 10 + (v.p[i] - '0'); any = true; }
    if (i < v.n && v.p[i] == '.')
        for (i++; i < v.n && is_digit(v.p[i]); i++) { m = m * 10 + (v.p[i] - '0'); e--; any = true; }
    if (!any) return 0;
    if (i + 1 < v.n && up(v.p[i]) == 'E') {
        size_t j = i + 1; bool en = false; int x = 0;
        if (v.p[j] == '+' || v.p[j] == '-') en = v.p[j++] == '-';
        if (j < v.n && is_digit(v.p[j])) {
            for (; j < v.n && is_digit(v.p[j]); j++) if (x < 1000) x = x * 10 + (v.p[j] - '0');
            e += en ? -x : x;
            i = j;
        }
    }
    while (e > 0 && m != 0 && m < 1e300) { m *= 10; e--; }
    if (e < -400) m = 0;
    while (e < 0 && m != 0) { m /= 10; e++; }
    *out = neg ? -m : m;
    return i;
}

static bool unit_scale(scpi_str_t u, const char *unit, double *k)
{
    size_t un = strlen(unit);
    if (u.n < un) return false;
    for (size_t i = 0; i < un; i++)
        if (up(u.p[u.n - un + i]) != up(unit[i])) return false;
    size_t pn = u.n - un;
    char a = pn ? up(u.p[0]) : 0;
    if (pn == 0) *k = 1;
    else if (pn == 2 && a == 'M' && up(u.p[1]) == 'A') *k = 1e6;
    else if (pn != 1) return false;
    else if (a == 'G') *k = 1e9;
    else if (a == 'K') *k = 1e3;
    else if (a == 'M') *k = strcmp(unit, "HZ") == 0 ? 1e6 : 1e-3; /* 488.2: MHZ is mega */
    else if (a == 'U') *k = 1e-6;
    else if (a == 'N') *k = 1e-9;
    else return false;
    return true;
}

bool scpi_number(scpi_t *s, int i, const char *unit,
                 double min, double max, double def, double *out)
{
    if (i >= s->nparam) { scpi_error(s, -109); return false; }
    scpi_str_t v = s->param[i];
    if (scpi_match_keyword(v, "MINimum")) { *out = min; return true; }
    if (scpi_match_keyword(v, "MAXimum")) { *out = max; return true; }
    if (scpi_match_keyword(v, "DEFault")) { *out = def; return true; }
    double x;
    size_t n = parse_num(v, &x);
    if (!n) { scpi_error(s, -104); return false; }
    while (n < v.n && is_space(v.p[n])) n++;
    if (n < v.n) {
        double k;
        scpi_str_t u = {v.p + n, v.n - n};
        if (!unit) { scpi_error(s, -138); return false; }
        if (!unit_scale(u, unit, &k)) { scpi_error(s, -131); return false; }
        x *= k;
    }
    if (!(x >= min && x <= max)) { scpi_error(s, -222); return false; }
    *out = x;
    return true;
}

bool scpi_bool(scpi_t *s, int i, bool *out)
{
    if (i >= s->nparam) { scpi_error(s, -109); return false; }
    scpi_str_t v = s->param[i];
    if (scpi_match_keyword(v, "ON")) { *out = true; return true; }
    if (scpi_match_keyword(v, "OFF")) { *out = false; return true; }
    double x;
    if (parse_num(v, &x) != v.n || !v.n) { scpi_error(s, -224); return false; }
    *out = x >= 0.5 || x <= -0.5;
    return true;
}

/* ---------- built-in commands ---------- */
static bool reg_arg(scpi_t *s, double max, double *x)
{
    return scpi_nparams(s, 1, 1) && scpi_number(s, 0, NULL, 0, max, 0, x);
}
static void c_idn(scpi_t *s) { scpi_out_text(s, s->port->idn); }
static void c_rst(scpi_t *s) { if (s->port->reset) s->port->reset(); }
static void c_tst(scpi_t *s) { scpi_out_int(s, s->port->selftest ? s->port->selftest() : 0); }
static void c_cls(scpi_t *s)
{
    s->err_count = 0; s->esr = 0; s->oper.event = 0; s->ques.event = 0;
}
static void c_ese(scpi_t *s)  { double x; if (reg_arg(s, 255, &x)) s->ese = (uint8_t)x; }
static void q_ese(scpi_t *s)  { scpi_out_int(s, s->ese); }
static void q_esr(scpi_t *s)  { scpi_out_int(s, s->esr); s->esr = 0; }
static void c_sre(scpi_t *s)  { double x; if (reg_arg(s, 255, &x)) s->sre = (uint8_t)x & 0xBF; }
static void q_sre(scpi_t *s)  { scpi_out_int(s, s->sre); }
static void q_stb(scpi_t *s)  { scpi_out_int(s, scpi_stb(s)); }
static bool wait_done(scpi_t *s)
{
    int e = s->port->wait ? s->port->wait(s) : 0;
    if (e) scpi_error(s, e);
    return !e;
}
static void c_opc(scpi_t *s)  { if (wait_done(s)) s->esr |= ESR_OPC; }
static void q_opc(scpi_t *s)  { if (wait_done(s)) scpi_out_int(s, 1); }
static void c_wai(scpi_t *s)  { wait_done(s); }
static void q_err(scpi_t *s)
{
    int code = 0;
    if (s->err_count) {
        code = s->errq[s->err_head];
        s->err_head = (uint8_t)((s->err_head + 1) % SCPI_ERRQ_LEN);
        s->err_count--;
    }
    scpi_out_int(s, code);
    scpi_out_quoted(s, scpi_error_text(code));
}
static void q_errcount(scpi_t *s) { scpi_out_int(s, s->err_count); }
static void q_vers(scpi_t *s) { scpi_out_text(s, "1999.0"); }
static void q_ev(scpi_t *s, scpi_reg_t *r) { scpi_out_int(s, r->event); r->event = 0; }
static void c_en(scpi_t *s, scpi_reg_t *r) { double x; if (reg_arg(s, 65535, &x)) r->enable = (uint16_t)x & 0x7FFF; }
static void q_oev(scpi_t *s) { q_ev(s, &s->oper); }
static void q_oco(scpi_t *s) { scpi_out_int(s, s->oper.cond); }
static void c_oen(scpi_t *s) { c_en(s, &s->oper); }
static void q_oen(scpi_t *s) { scpi_out_int(s, s->oper.enable); }
static void q_qev(scpi_t *s) { q_ev(s, &s->ques); }
static void q_qco(scpi_t *s) { scpi_out_int(s, s->ques.cond); }
static void c_qen(scpi_t *s) { c_en(s, &s->ques); }
static void q_qen(scpi_t *s) { scpi_out_int(s, s->ques.enable); }
static void c_pres(scpi_t *s) { s->oper.enable = 0; s->ques.enable = 0; }
static void q_help(scpi_t *s);

static const scpi_cmd_t builtin[] = {
    {"*IDN?", c_idn}, {"*RST", c_rst}, {"*TST?", c_tst}, {"*CLS", c_cls},
    {"*ESE", c_ese}, {"*ESE?", q_ese}, {"*ESR?", q_esr}, {"*SRE", c_sre},
    {"*SRE?", q_sre}, {"*STB?", q_stb}, {"*OPC", c_opc}, {"*OPC?", q_opc},
    {"*WAI", c_wai},
    {"SYSTem:ERRor[:NEXT]?", q_err}, {"SYSTem:ERRor:COUNt?", q_errcount},
    {"SYSTem:VERSion?", q_vers}, {"SYSTem:HELP:HEADers?", q_help},
    {"STATus:OPERation[:EVENt]?", q_oev}, {"STATus:OPERation:CONDition?", q_oco},
    {"STATus:OPERation:ENABle", c_oen}, {"STATus:OPERation:ENABle?", q_oen},
    {"STATus:QUEStionable[:EVENt]?", q_qev}, {"STATus:QUEStionable:CONDition?", q_qco},
    {"STATus:QUEStionable:ENABle", c_qen}, {"STATus:QUEStionable:ENABle?", q_qen},
    {"STATus:PRESet", c_pres},
    {NULL, NULL}
};

static void q_help(scpi_t *s)
{
    size_t n = 0;
    for (int k = -1; k < 0 || s->cmds[k]; k++)
        for (const scpi_cmd_t *c = k < 0 ? builtin : s->cmds[k]; c->pattern; c++) n += strlen(c->pattern) + 1;
    scpi_out_block(s, n);
    for (int k = -1; k < 0 || s->cmds[k]; k++)
        for (const scpi_cmd_t *c = k < 0 ? builtin : s->cmds[k]; c->pattern; c++) {
            scpi_out_raw(s, c->pattern, strlen(c->pattern));
            scpi_out_raw(s, "\n", 1);
        }
}

static const scpi_cmd_t *find(scpi_t *s, const char *h, size_t n, bool query)
{
    for (const scpi_cmd_t *c = builtin; c->pattern; c++) if (match_cmd(s, c->pattern, h, n, query)) return c;
    for (const scpi_cmd_t *const *t = s->cmds; *t; t++)
        for (const scpi_cmd_t *c = *t; c->pattern; c++) if (match_cmd(s, c->pattern, h, n, query)) return c;
    return NULL;
}

/* ---------- message parsing ---------- */
/* A block may start only after a space or comma; same rule as scpi_input(). */
static bool block_opens(const char *line, const char *p)
{
    return *p == '#' && p > line && (is_space(p[-1]) || p[-1] == ',');
}
/* Parse "#<d><len><data>"; NULL unless complete within e. */
static const char *block_end(const char *p, const char *e, const char **data)
{
    if (e - p < 2 || p[1] < '1' || p[1] > '9') return NULL;
    int d = p[1] - '0';
    size_t n = 0;
    p += 2;
    for (; d; d--, p++) {
        if (p == e || !is_digit(*p) || n > SCPI_LINE_MAX) return NULL;
        n = n * 10 + (size_t)(*p - '0');
    }
    if (n > (size_t)(e - p)) return NULL;
    if (data) *data = p;
    return p + n;
}
bool scpi_block(scpi_t *s, int i, const uint8_t **p, size_t *n)
{
    if (i >= s->nparam) { scpi_error(s, -109); return false; }
    scpi_str_t v = s->param[i];
    const char *d, *e = v.n && v.p[0] == '#' ? block_end(v.p, v.p + v.n, &d) : NULL;
    if (!e) { scpi_error(s, v.n && v.p[0] == '#' ? -161 : -104); return false; }
    if (e != v.p + v.n) { scpi_error(s, -160); return false; }
    *p = (const uint8_t *)d;
    *n = (size_t)(e - d);
    return true;
}

bool scpi_chanlist(scpi_t *s, int i, unsigned lo, unsigned hi, uint32_t *mask)
{
    if (i >= s->nparam) { scpi_error(s, -109); return false; }
    scpi_str_t v = s->param[i];
    const char *p = v.p, *e = v.p + v.n;
    if (v.n < 3 || p[0] != '(' || p[1] != '@' || e[-1] != ')') { scpi_error(s, -104); return false; }
    p += 2; e--;
    uint32_t m = 0;
    while (p < e && is_space(*p)) p++;
    if (p == e) { scpi_error(s, -104); return false; }
    while (p < e) {
        unsigned a[2], k = 0;
        for (;;) {
            while (p < e && is_space(*p)) p++;
            if (p == e || !is_digit(*p)) { scpi_error(s, -104); return false; }
            unsigned x = 0;
            while (p < e && is_digit(*p)) { x = x * 10u + (unsigned)(*p++ - '0'); if (x > 999u) x = 999u; }
            a[k++] = x;
            while (p < e && is_space(*p)) p++;
            if (k == 1 && p < e && *p == ':') { p++; continue; }
            break;
        }
        if (k == 1) a[1] = a[0];
        unsigned from = a[0] < a[1] ? a[0] : a[1], to = a[0] < a[1] ? a[1] : a[0];
        if (from < lo || to > hi) { scpi_error(s, -222); return false; }
        for (unsigned c = from; c <= to; c++) m |= 1u << c;
        if (p == e) break;
        if (*p != ',') { scpi_error(s, -104); return false; }
        p++;
        while (p < e && is_space(*p)) p++;
        if (p == e) { scpi_error(s, -104); return false; }  /* trailing comma */
    }
    *mask = m;
    return true;
}

static bool split_params(scpi_t *s, const char *p, const char *e)
{
    s->nparam = 0;
    while (p < e && is_space(*p)) p++;
    if (p == e) return true;
    for (;;) {
        while (p < e && is_space(*p)) p++;
        const char *b = p, *keep = p, *be;
        char q = 0; int depth = 0;
        for (; p < e; p++) {
            if (!q && block_opens(s->line, p) && (be = block_end(p, e, NULL))) { keep = be; p = be - 1; continue; }
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == ',' && depth <= 0) break;
        }
        const char *t = p;
        while (t > keep && is_space(t[-1])) t--;
        if (t == b || depth) { scpi_error(s, -102); return false; }
        if (s->nparam == SCPI_PARAM_MAX) { scpi_error(s, -108); return false; }
        s->param[s->nparam++] = (scpi_str_t){b, (size_t)(t - b)};
        if (p == e) return true;
        p++;
    }
}

static void exec_unit(scpi_t *s, const char *b, const char *e, char *path, size_t *plen)
{
    while (b < e && is_space(*b)) b++;
    if (b == e) return;
    const char *h = b;
    while (b < e && !is_space(*b)) b++;
    size_t hn = (size_t)(b - h);
    bool query = h[hn - 1] == '?';
    if (query) hn--;
    if (!hn) { scpi_error(s, -102); return; }
    for (size_t i = 0; i < hn; i++) {
        char c = up(h[i]);
        if (!(is_upper(c) || is_digit(c) || c == ':' || c == '_' || (c == '*' && i == 0))) {
            scpi_error(s, (c == '*' || c == '?') ? -102 : -101);
            return;
        }
    }
    char full[SCPI_HEADER_MAX];
    size_t fn = 0;
    bool common = h[0] == '*';
    if (h[0] == ':') { h++; hn--; *plen = 0; }
    else if (!common) { memcpy(full, path, *plen); fn = *plen; }
    if (!hn) { scpi_error(s, -102); return; }
    if (fn + hn > sizeof full) { scpi_error(s, -112); return; }
    memcpy(full + fn, h, hn);
    fn += hn;

    const scpi_cmd_t *c = find(s, full, fn, query);
    if (!c) { scpi_error(s, -113); return; }
    if (!common) {
        size_t k = fn;
        while (k && full[k - 1] != ':') k--;
        memcpy(path, full, k);
        *plen = k;
    }
    if (!split_params(s, b, e)) return;
    /* Built-ins that take no argument reject any they are given. */
    if (c >= builtin && c < builtin + sizeof builtin / sizeof *builtin) {
        bool takes = !query && (c->fn == c_ese || c->fn == c_sre || c->fn == c_oen || c->fn == c_qen);
        if (!takes && s->nparam) { scpi_error(s, -108); return; }
    }
    s->out_items = 0;
    c->fn(s);
}

static void exec_line(scpi_t *s)
{
    char path[SCPI_HEADER_MAX];
    size_t plen = 0;
    const char *p = s->line, *end = s->line + s->len;
    s->out_units = 0;
    s->cmd_error = false;
    while (p <= end) {
        const char *b = p;
        char q = 0;
        const char *be;
        for (; p < end; p++) {
            if (!q && block_opens(s->line, p) && (be = block_end(p, end, NULL))) { p = be - 1; continue; }
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == ';') break;
        }
        s->failed = false;
        if (q) { scpi_error(s, -151); break; }
        exec_unit(s, b, p, path, &plen);
        if (s->cmd_error) break;    /* 488.2: command error drops the rest */
        p++;
    }
    if (s->out_units) scpi_out_raw(s, "\n", 1);
    s->out_units = 0;
}

static void line_reset(scpi_t *s)
{
    s->len = 0; s->overrun = false; s->inq = 0; s->blk = 0;
}

void scpi_input_timeout(scpi_t *s)
{
    if (s->blk != 3) return;
    line_reset(s);
    scpi_error(s, -161);
}

void scpi_input(scpi_t *s, char c)
{
    bool data = s->blk == 3;
    if (c == '\n' && !data) {
        if (s->overrun) scpi_error(s, -363);
        else exec_line(s);
        line_reset(s);
        return;
    }
    if (s->len >= SCPI_LINE_MAX) { s->overrun = true; s->blk = 0; return; }
    char prev = s->len ? s->line[s->len - 1] : 0;
    s->line[s->len++] = c;
    switch (s->blk) {
    case 3:
        if (--s->blk_left == 0) s->blk = 0;
        return;
    case 2:
        if (!is_digit(c)) { s->blk = 0; break; }
        s->blk_left = s->blk_left * 10 + (size_t)(c - '0');
        if (s->blk_left > SCPI_LINE_MAX) { s->overrun = true; s->blk = 0; return; }
        if (--s->blk_digits == 0) s->blk = s->blk_left ? 3 : 0;
        return;
    case 1:
        s->blk = 0;
        if (c >= '1' && c <= '9') { s->blk = 2; s->blk_digits = (uint8_t)(c - '0'); s->blk_left = 0; return; }
        break;
    }
    if (s->inq) { if (c == s->inq) s->inq = 0; }
    else if (c == '"' || c == '\'') s->inq = c;
    else if (c == '#' && (is_space(prev) || prev == ',')) s->blk = 1;
}

void scpi_init(scpi_t *s, const scpi_port_t *port, const scpi_cmd_t *const *cmds)
{
    memset(s, 0, sizeof *s);
    s->port = port;
    s->cmds = cmds;
    s->esr = ESR_PON;
}
