/* Portable SCPI / IEEE 488.2 core. No heap, no hardware dependencies. */
#ifndef SCPI_H
#define SCPI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCPI_LINE_MAX   8448   /* room for a 4096 x uint16 block */
#define SCPI_HEADER_MAX 128
#define SCPI_PARAM_MAX  16
#define SCPI_ERRQ_LEN   16
#define SCPI_SUFFIX_MAX 4

enum { /* Standard Event Status Register bits */
    ESR_OPC = 0x01, ESR_QYE = 0x04, ESR_DDE = 0x08,
    ESR_EXE = 0x10, ESR_CME = 0x20, ESR_PON = 0x80
};

typedef struct scpi scpi_t;
typedef void (*scpi_fn)(scpi_t *);
typedef struct { const char *pattern; scpi_fn fn; } scpi_cmd_t;
typedef struct { const char *p; size_t n; } scpi_str_t;
typedef struct { uint16_t cond, event, enable; } scpi_reg_t;

typedef struct {
    void (*write)(const char *s, size_t n);
    void (*reset)(void);        /* *RST */
    int  (*selftest)(void);     /* *TST?, 0 = pass */
    const char *idn;
    int  (*wait)(scpi_t *);     /* finish pending work; 0 or error code */
} scpi_port_t;

struct scpi {
    const scpi_port_t *port;
    const scpi_cmd_t *const *cmds; /* NULL-terminated list of tables */
    char line[SCPI_LINE_MAX];
    size_t len;
    bool overrun;
    char inq;                   /* input: open quote */
    uint8_t blk;                /* input: 0 text, 1 after #, 2 length digits, 3 data */
    uint8_t blk_digits;
    size_t blk_left;
    int16_t errq[SCPI_ERRQ_LEN];
    uint8_t err_head, err_count;
    uint8_t esr, ese, sre;
    scpi_reg_t oper, ques;
    /* state of the command being executed */
    scpi_str_t param[SCPI_PARAM_MAX];
    int nparam;
    uint32_t suffix[SCPI_SUFFIX_MAX];
    int nsuffix;
    int out_units, out_items;
    bool failed, cmd_error;
};

void scpi_init(scpi_t *s, const scpi_port_t *port, const scpi_cmd_t *const *cmds);
void scpi_input_timeout(scpi_t *s);  /* call when input pauses: drops a stalled block */
void scpi_input(scpi_t *s, char c);
void scpi_error(scpi_t *s, int code);
const char *scpi_error_text(int code);
uint8_t scpi_stb(const scpi_t *s);

/* Parameter helpers: return false after queueing the matching error. */
bool scpi_nparams(scpi_t *s, int min, int max);
bool scpi_bool(scpi_t *s, int i, bool *out);
bool scpi_number(scpi_t *s, int i, const char *unit,
                 double min, double max, double def, double *out);
bool scpi_block(scpi_t *s, int i, const uint8_t **p, size_t *n);
bool scpi_match_keyword(scpi_str_t v, const char *pattern);
/* Channel list "(@1,3:5)": sets bit n of *mask for each channel n (lo..hi <= 31). */
bool scpi_chanlist(scpi_t *s, int i, unsigned lo, unsigned hi, uint32_t *mask);

/* Result helpers; separators are inserted automatically. */
void scpi_out_int(scpi_t *s, int32_t v);
void scpi_out_num(scpi_t *s, double v);      /* NR3 */
void scpi_out_text(scpi_t *s, const char *t);
void scpi_out_quoted(scpi_t *s, const char *t);
void scpi_out_block(scpi_t *s, size_t n);    /* header only; data follows */
void scpi_out_raw(scpi_t *s, const char *p, size_t n);
#endif
