#ifndef DVM_H
#define DVM_H
#include "scpi.h"
/* Multi-channel DC voltmeter with plug detection on A0..A5 */
void dvm_init(void);
void dvm_poll(scpi_t *s);
void dvm_measure(scpi_t *s);   /* MEASure:VOLTage[:DC]? (@list) */
extern const scpi_cmd_t dvm_cmds[];
#endif
