#ifndef LAB_H
#define LAB_H
#include "scpi.h"
extern const scpi_cmd_t lab_cmds[];
void lab_init(void);
void lab_reset(void);
void lab_poll(scpi_t *s);
int  lab_wait(scpi_t *s);
int  lab_selftest(void);
#endif
