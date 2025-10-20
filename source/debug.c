#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>

#include <sysmodule/sysmodule.h>
#include "debug.h"
#include <net/net.h>
#include <netinet/in.h>

static int SocketFD = -1;
void netDebugInit() {
	#ifdef DEBUG 
	struct sockaddr_in stSockAddr;
	SocketFD = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	memset(&stSockAddr, 0, sizeof stSockAddr);

	stSockAddr.sin_family = AF_INET;
	stSockAddr.sin_port = htons(atoi(DEBUG_PORT));
	inet_pton(AF_INET, DEBUG_ADDR, &stSockAddr.sin_addr);
	netConnect(SocketFD, (struct sockaddr*)&stSockAddr, sizeof stSockAddr);
	#endif
}

void netDebug(const char* fmt, ...) {
	if(SocketFD < 0) return;
	char buffer[0x800];
  	va_list arg;
  	va_start (arg, fmt);
  	vsnprintf (buffer, sizeof (buffer), fmt, arg);
	va_end (arg);
	netSend(SocketFD, buffer, strlen(buffer), 0);
}