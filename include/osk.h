#ifndef OSK_H
#define OSK_H

#include <sysutil/osk.h>

void utf16_to_8(u16 *stw, u8 *stb);
void utf8_to_16(u8 *stb, u16 *stw);
int get_osk_string(char *caption, char *str, int len);

#endif //OSK_H

