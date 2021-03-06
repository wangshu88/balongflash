// 

// Процедуры обработки цифровых подписей
// 
#include <stdio.h>
#include <stdint.h>
#ifndef WIN32
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#else
#include <windows.h>
#include "getopt.h"
#include "printf.h"
#include "buildno.h"
#endif

#include "hdlcio.h"
#include "ptable.h"
#include "flasher.h"
#include "util.h"
#include "zlib.h"

// таблица параметров ключа -g

struct {
  uint8_t type;
  uint32_t len;
  char* descr;
} signbase[] = {
   {1,2958, "Basic Firmware"},
   {1,2694, "E3372s-stick firmware"},
   {2,1110, "Web Interface + ISO for HLINK-modem"},
   {6,1110, "Web Interface + ISO for HLINK Modem"},
   {2,846, "ISO (dashboard) for stick-modem"},
   {7,3750, "Firmware + ISO + web interface"},
};

#define signbaselen 6

// таблица типов подписей
char* fwtypes[]={
"UNKNOWN",        // 0
"ONLY_FW",        // 1
"ONLY_ISO",       // 2
"FW_ISO",         // 3
"ONLY_WEBUI",     // 4
"FW_WEBUI",       // 5
"ISO_WEBUI",      // 6
"FW_ISO_WEBUI"    // 7
};  


// результирующая строка ^signver-команды
uint8_t signver[200];

// Флаг режима цифровой подписи
extern int gflag;

// Флаг типа прошивки
extern int dflag;

// Параметры текущей цифровой подписи
uint32_t signtype; // тип прошивки
uint32_t signlen;  // длина подписи

int32_t serach_sign();

// Хеш открытого ключа для ^signver
char signver_hash[100]="778A8D175E602B7B779D9E05C330B5279B0661BF2EED99A20445B366D63DD697";

//****************************************************
//* Получение описания типа прошивки по коду
//****************************************************
char* fw_description(uint8_t code) {
  
return fwtypes[code&0x7];  
}

//****************************************************
//* Получение списка типов прошивок
//****************************************************
void dlist() {
  
int i;

printf("\n#  Description\n--------------------------------------");
for(i=1;i<8;i++) {
  printf("\n %i  %s",i,fw_description(i));
}
printf("\n\n");
exit(0);
}

//***************************************************
//* Обработка параметров ключа -d
//***************************************************
void dparm(char* sparm) {
  
if (dflag != 0) {
  printf("\nDuplicate key -d\n\n");
  exit(-1);
}  

if (sparm[0] == 'l') {
  dlist();
  exit(0);
}  
sscanf(sparm,"%x",&dload_id);
if ((dload_id == 0) || (dload_id >7)) {
  printf("\nIncorrect key value -d\n\n");
  exit(-1);
}
dflag=1;
}


//****************************************************
//* Получение списка параметров ключа -g
//****************************************************
void glist() {
  
int i;
printf("\n#  length type description \n--------------------------------------");
for (i=0; i<signbaselen; i++) {
  printf("\n%1i  %5i  %2i   %s",i,signbase[i].len,signbase[i].type,signbase[i].descr);
}
printf("\n\nYou can also specify arbitrary signature parameters in the format:\n  -g *,type,len\n\n");
exit(0);
}

//***************************************************
//* Обработка параметров ключа -g
//***************************************************
void gparm(char* sparm) {
  
int index;  
char* sptr;
char parm[100];


if (gflag != 0) {
  printf("\nDuplicate key -g\n\n");
  exit(-1);
}  

strcpy(parm,sparm); // локальная копия параметров

if (parm[0] == 'l') {
  glist();
  exit(0);
}  

if (parm[0] == 'd') {
  // запрет автоопределения подписи
  gflag = -1;
  return;
} 

if (strncmp(parm,"*,",2) == 0) {
  // произвольные параметры
  // выделяем длину
  sptr=strrchr(parm,',');
  if (sptr == 0) goto perror;
  signlen=atoi(sptr+1);
  *sptr=0;
  // выделяем тип раздела
  sptr=strrchr(parm,',');
  if (sptr == 0) goto perror;
  signtype=atoi(sptr+1);
  if (fw_description(signtype) == 0) {
    printf("\nKey -g: unknown type of firmware - %i\n",signtype);
    exit(-1);
  }  
}
else {  
  index=atoi(parm);
  if (index >= signbaselen) goto perror;
  signlen=signbase[index].len;
  signtype=signbase[index].type;
}

gflag=1;
// printf("\nstr - %s",signver);
return;

perror:
 printf("\nError in key parameters -g\n");
 exit(-1);
} 
  

//***************************************************
//* Отправка цифровой подписи
//***************************************************
void send_signver() {
  
uint32_t res;
// ответ на ^signver
unsigned char SVrsp[]={0x0d, 0x0a, 0x30, 0x0d, 0x0a, 0x0d, 0x0a, 0x4f, 0x4b, 0x0d, 0x0a};
uint8_t replybuf[200];
  
if (gflag == 0) {  
  // автоопределение цифровой подписи
  signtype=dload_id&0x7;
  signlen=serach_sign();
  if (signlen == -1) return; // подпись в файле не найдена
}

printf("\nDigital signature mode: %s (%i byte)",fw_description(signtype),signlen);
sprintf(signver,"^SIGNVER=%i,0,%s,%i",signtype,signver_hash,signlen);
res=atcmd(signver,replybuf);
if ( (res<sizeof(SVrsp)) || (memcmp(replybuf,SVrsp,sizeof(SVrsp)) != 0) ) {
   printf("\nDigital signature verification failed - %02x\n",replybuf[2]);
   exit(-2);
}
}

//***************************************************
//* Поиск цифровой подписи в прошивке
//***************************************************
int32_t serach_sign() {

int i,j;
uint32_t pt;
uint32_t signsize;

for (i=0;i<2;i++) {
  if (i == npart) break;
  pt=*((uint32_t*)&ptable[i].pimage[ptable[i].hd.psize-4]);
  if (pt == 0xffaaaffa) { 
    // подпись найдена
    signsize=*((uint32_t*)&ptable[i].pimage[ptable[i].hd.psize-12]);
    // выделяем хеш открытого ключа
    for(j=0;j<32;j++) {
     sprintf(signver_hash+2*j,"%02X",ptable[i].pimage[ptable[i].hd.psize-signsize+6+j]);
    }
    return signsize;
  }
}
// не найдена
return -1;
}