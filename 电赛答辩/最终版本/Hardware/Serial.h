#ifndef __SERIAL_H
#define __SERIAL_H

#include <stdio.h>

extern char Serial_RxPacket[];
extern uint8_t Serial_RxFlag;

void Serial_Init(void);
void Serial_Printf(char *format, ...);
uint8_t Serial_GetRxFlag(void);
void ParseCmd(char *s);

#endif
