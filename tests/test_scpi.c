#include "scpi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char out[65536]; static size_t on; static scpi_t s; static double volt; static int fails, checks;
static void wr(const char *p, size_t n) { if (on + n < sizeof out) { memcpy(out + on, p, n); on += n; } }
static void c_v(scpi_t *x) { double d; if (scpi_nparams(x,1,1) && scpi_number(x,0,"V",0,3.3,1,&d)) volt = d; }
static void q_v(scpi_t *x) { scpi_out_num(x, volt); }
static void c_f(scpi_t *x) { double d; if (scpi_nparams(x,1,1) && scpi_number(x,0,"HZ",1,1e7,1e3,&d)) volt = d; }
static void q_ch(scpi_t *x) { if (x->suffix[0] < 1 || x->suffix[0] > 4) scpi_error(x,-114); else scpi_out_int(x,(int)x->suffix[0]); }
static void q_cl(scpi_t *x) { uint32_t m; if (scpi_nparams(x,1,1) && scpi_chanlist(x,0,1,6,&m)) scpi_out_int(x,(int32_t)m); }
static size_t bn; static uint32_t bsum;
static void c_b(scpi_t*x){const uint8_t*p;size_t n;if(scpi_block(x,0,&p,&n)){bn=n;bsum=0;while(n--)bsum+=*p++;}}
static const scpi_cmd_t cmds[] = {{"[SOURce]:VOLTage[:LEVel]",c_v},{"[SOURce]:VOLTage[:LEVel]?",q_v},
 {"SOURce:FREQuency",c_f},{"CHANnel#:NUMber?",q_ch},{"DATA",c_b},{"ROUTe:LIST?",q_cl},{NULL,NULL}};
static const scpi_cmd_t *const tabs[]={cmds,NULL};
static const scpi_port_t port = {wr,NULL,NULL,"A,B,C,D",NULL};
static void raw(const char*p,size_t n){on=0;while(n--)scpi_input(&s,*p++);out[on]=0;}
static void feed(const char *t) { while (*t) scpi_input(&s, *t++); }
static void ex(const char *in, const char *want) {
  on = 0; feed(in); feed("\n"); out[on] = 0; checks++;
  if (strcmp(out, want)) { fails++; printf("FAIL [%s]\n got [%s]\nwant [%s]\n", in, out, want); } }
static void err(const char *in, int code) {
  char w[80]; snprintf(w, sizeof w, "%d,\"%s\"\n", code, scpi_error_text(code));
  ex("*CLS", ""); ex(in, ""); ex("SYST:ERR?", w); }
int main(void) {
  scpi_init(&s,&port,tabs);
  ex("*ESR?","128\n"); ex("*idn?\r","A,B,C,D\n"); ex("","");
  ex("SYST:ERR?","0,\"No error\"\n"); ex("system:error:next?","0,\"No error\"\n");
  ex("VOLT 1.5;VOLT?","1.500000E+00\n"); ex("SOUR:VOLT:LEV 250 mV;LEV?","2.500000E-01\n");
  ex("VOLTAGE MAX;:SOUR:VOLT?","3.300000E+00\n"); ex("VOLT 1e-1;*IDN?;VOLT?","A,B,C,D;1.000000E-01\n");
  ex("SOUR:FREQ 2 MHZ;VOLT?","2.000000E+06\n"); ex("SYST:ERR:COUN?;NEXT?","0;0,\"No error\"\n");
  ex("CHAN3:NUM?;:CHAN:NUM?","3;1\n"); ex("*OPC?;*SRE 255;*SRE?","1;191\n");
  err("VOLTA 1",-113); err("VOLT",-109); err("VOLT 1,2",-108); err("VOLT 9",-222); err("VOLT abc",-104);
  err("VOLT 1 A",-131); err("*ESE 1V",-138); err("CHAN9:NUM?",-114); err("VOLT ,1",-102); err("V$ 1",-101);
  err("VOLT 'x",-151); err("SYST:",-113); err("*IDN? 1",-108); err("VOLT 1e999",-222); err("VOLT NAN",-104); err("?",-102);
  ex("ROUT:LIST? (@1)","2\n"); ex("ROUT:LIST? (@1,3:5)","58\n"); ex("rout:list? (@ 6 : 4 , 1 )","114\n");
  err("ROUT:LIST? (@0)",-222); err("ROUT:LIST? (@7)",-222); err("ROUT:LIST? (@)",-104); err("ROUT:LIST? (@1,)",-104);
  err("ROUT:LIST? 1",-104); err("ROUT:LIST? (@1:)",-104); err("ROUT:LIST? (@1;2)",-102);
  ex("*CLS;BOGUS;*IDN?",""); ex("*ESR?","32\n"); ex("*CLS;VOLT 9;*IDN?","A,B,C,D\n"); ex("*ESR?","16\n");
  ex("*CLS",""); for (int i=0;i<30;i++) ex("BOGUS",""); ex("SYST:ERR:COUN?","16\n");
  for (int i=0;i<15;i++) { ex("SYST:ERR?","-113,\"Undefined header\"\n"); } ex("SYST:ERR?","-350,\"Queue overflow\"\n");
  ex("*CLS",""); for (int i=0;i<9000;i++) scpi_input(&s,'A'); ex("",""); ex("SYST:ERR?","-363,\"Input buffer overrun\"\n");
  ex("*CLS;*ESE 32;*SRE 32",""); ex("BOGUS",""); ex("*STB?","100\n");
  /* binary block with newline, quote, separators and trailing space inside */
  #define BLK(str) raw(str, sizeof(str)-1)
  ex("*CLS",""); BLK("DATA #16\n;'\", ;*IDN?\n"); checks++; if (bn!=6||bsum!='\n'+';'+'\''+'"'+','+' '||strcmp(out,"A,B,C,D\n")) {fails++;printf("FAIL block %zu %u [%s]\n",bn,(unsigned)bsum,out);}
  ex("SYST:ERR?","0,\"No error\"\n"); err("DATA 5",-104); err("DATA #2",-161); err("DATA #15abcdeX",-160);
  ex("*CLS",""); BLK("DATA #15ab"); scpi_input_timeout(&s); ex("SYST:ERR?","-161,\"Invalid block data\"\n");
  ex("DATA #9999999999",""); ex("SYST:ERR?","-363,\"Input buffer overrun\"\n"); ex("*IDN?","A,B,C,D\n");
  static char big[8300]; int bl=sprintf(big,"DATA #48192"); memset(big+bl,'\n',8192); big[bl+8192]='\n'; raw(big,(size_t)bl+8193);
  checks++; if(bn!=8192){fails++;puts("FAIL big block");}
  /* fuzz: mutated valid fragments plus raw bytes, run under ASan/UBSan */
  static const char *frag[] = {"*IDN?","SYST:ERR?","VOLT ","SOUR:","CHAN","99999999999",":NUM?","1e","-",".5","mV",";",":",",","\"","'","(",")","#","?"," ","MAX","\n","STAT:OPER:ENAB ","SYST:HELP:HEAD?","DATA #15","#14","#9",",#18","ROUT:LIST? ","(@","1:3",")"};
  srand(1); long n = 2000000;
  for (long i=0;i<n;i++) { if (rand()%4) feed(frag[(size_t)rand()%(sizeof frag/sizeof*frag)]); else scpi_input(&s,(char)rand());
    if (i%16==0) on=0; }
  scpi_input_timeout(&s); feed("\n"); ex("*CLS",""); ex("*IDN?","A,B,C,D\n");
  printf("%d checks, %d failures, %ld fuzz tokens\n",checks,fails,n); return fails!=0; }
