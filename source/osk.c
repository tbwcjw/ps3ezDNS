//based on https://github.com/lmirel/fm_psx/blob/master/source/osk_input.c
//utils from https://github.com/lmirel/fm_psx/blob/master/source/util.c

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>

#include <sysutil/osk.h>
#include <sysutil/sysutil.h>
#include <sys/memory.h>
#include <ppu-lv2.h>

#include <tiny3d.h>
#include "libfont2.h"

#define OSKDIALOG_FINISHED          0x503
#define OSKDIALOG_UNLOADED          0x504
#define OSKDIALOG_INPUT_ENTERED     0x505
#define OSKDIALOG_INPUT_CANCELED    0x506

#define SUCCESS 	1
#define FAILED	 	0

volatile int osk_event = 0;
volatile int osk_unloaded = 0;
int osk_action = SUCCESS;

static sys_mem_container_t container_mem;

static oskCallbackReturnParam output_returned;
static oskParam dialog_osk;
static oskInputFieldInfo input_field;

static void osk_event_handler(u64 status, u64 param, void * userdata) {

    switch((u32) status) {

	case OSKDIALOG_INPUT_CANCELED:
		osk_event= OSKDIALOG_INPUT_CANCELED;
		break;

    case OSKDIALOG_UNLOADED:
		osk_unloaded= 1;
		break;

    case OSKDIALOG_INPUT_ENTERED:
		osk_event=OSKDIALOG_INPUT_ENTERED;
		break;

	case OSKDIALOG_FINISHED:
		osk_event=OSKDIALOG_FINISHED;
		break;

    default:
        break;
    }
}

static int osk_level = 0;

void utf16_to_8(u16 *stw, u8 *stb)
{
    while(stw[0])
    {
        if((stw[0] & 0xFF80) == 0)
        {
            *(stb++) = stw[0] & 0xFF;   // utf16 00000000 0xxxxxxx utf8 0xxxxxxx
        }
        else if((stw[0] & 0xF800) == 0)
        {
            // utf16 00000yyy yyxxxxxx utf8 110yyyyy 10xxxxxx
            *(stb++) = ((stw[0]>>6) & 0xFF) | 0xC0; *(stb++) = (stw[0] & 0x3F) | 0x80;
        }
        else if((stw[0] & 0xFC00) == 0xD800 && (stw[1] & 0xFC00) == 0xDC00)
        {
            // utf16 110110ww wwzzzzyy 110111yy yyxxxxxx (wwww = uuuuu - 1)
            // utf8  1111000uu 10uuzzzz 10yyyyyy 10xxxxxx
            *(stb++)= (((stw[0] + 64)>>8) & 0x3) | 0xF0; *(stb++)= (((stw[0]>>2) + 16) & 0x3F) | 0x80;
            *(stb++)= ((stw[0]>>4) & 0x30) | 0x80 | ((stw[1]<<2) & 0xF); *(stb++)= (stw[1] & 0x3F) | 0x80;
            stw++;
        }
        else
        {
            // utf16 zzzzyyyy yyxxxxxx utf8 1110zzzz 10yyyyyy 10xxxxxx
            *(stb++)= ((stw[0]>>12) & 0xF) | 0xE0; *(stb++)= ((stw[0]>>6) & 0x3F) | 0x80; *(stb++)= (stw[0] & 0x3F) | 0x80;
        }

        stw++;
    }

    *stb = 0;
}

void utf8_to_16(u8 *stb, u16 *stw)
{
   int n, m;
   u32 UTF32;
   while(*stb)
   {
       if(*stb & 128)
       {
            m = 1;

            if((*stb & 0xf8) == 0xf0)
            {
                // 4 bytes
                UTF32 = (u32) (*(stb++) & 3);
                m = 3;
            }
            else if((*stb & 0xE0) == 0xE0)
            {
                // 3 bytes
                UTF32 = (u32) (*(stb++) & 0xf);
                m = 2;
            }
            else if((*stb & 0xE0) == 0xC0)
            {
                // 2 bytes
                UTF32 = (u32) (*(stb++) & 0x1f);
                m = 1;
            }
            else {stb++; continue;} // Error!

            for(n = 0; n < m; n++)
            {
                if(!*stb) break; // Error!

                if((*stb & 0xc0) != 0x80) break; // Error!
                UTF32 = (UTF32 <<6) |((u32) (*(stb++) & 63));
            }

            if((n != m) && !*stb) break;

        } else UTF32 = (u32) *(stb++);

        if(UTF32<65536)
            *stw++= (u16) UTF32;
        else
        {
            //110110ww wwzzzzyy 110111yy yyxxxxxx
            *stw++= (((u16) (UTF32>>10)) & 0x3ff) | 0xD800;
            *stw++= (((u16) (UTF32)) & 0x3ff) | 0xDC00;
        }
   }

   *stw++ = 0;
}

static void OSK_exit(void)
{
    if(osk_level == 2) {
        oskAbort();
        oskUnloadAsync(&output_returned);
        
        osk_event = 0;
        osk_action=FAILED;
    }

    if(osk_level >= 1) {
        sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
        sysMemContainerDestroy(container_mem);
    }

}

int get_osk_string(char *caption, 
                    char *str, 
                    int len) {
    int ret=SUCCESS;

    u16 * message = NULL;
    u16 * out = NULL;
    u16 * in = NULL;

    if(len > 256) len = 256; //will never be >256 but to be safe

    osk_level = 0;
    atexit(OSK_exit);

    if(sysMemContainerCreate(&container_mem, 8*1024*1024) < 0) return FAILED;

    osk_level = 1;

    message = malloc(strlen(caption)*2+32);
    if(!message) {ret=FAILED; goto end;}

    out = malloc(0x420*2);
    if(!out) {ret=FAILED; goto end;}

    in = malloc(0x420*2);
    if(!in) {ret=FAILED; goto end;}

    //memset(message, 0, 64);
    utf8_to_16((u8 *) caption, (u16 *) message);
    utf8_to_16((u8 *) str, (u16 *) in);

    input_field.message =  (u16 *) message;
    input_field.startText = (u16 *) in;
    input_field.maxLength = len;

    output_returned.res = OSK_NO_TEXT; //OSK_OK;
    output_returned.len = len;

    output_returned.str = (u16 *) out;

    memset(out, 0, 1024);

    if(oskSetKeyLayoutOption (OSK_10KEY_PANEL | OSK_FULLKEY_PANEL)<0) {ret=FAILED; goto end;}

    dialog_osk.firstViewPanel = OSK_PANEL_TYPE_ALPHABET_FULL_WIDTH;
    dialog_osk.allowedPanels = (OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL);

    if(oskAddSupportLanguage ( OSK_PANEL_TYPE_ALPHABET )<0) {ret=FAILED; goto end;}

    if(oskSetLayoutMode( OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER )<0) {ret=FAILED; goto end;}

    oskPoint pos = {0.0, 0.0};

    dialog_osk.controlPoint = pos;
    dialog_osk.prohibitFlags = OSK_PROHIBIT_RETURN;
    if(oskSetInitialInputDevice(OSK_DEVICE_PAD)<0) {ret=FAILED; goto end;}

    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, osk_event_handler, NULL);

    osk_action = SUCCESS;
    osk_unloaded = false;

    if(oskLoadAsync(container_mem, (const void *) &dialog_osk, (const void *)  &input_field)<0) {ret=FAILED; goto end;}

    osk_level = 2;

    while(!osk_unloaded)
    {
		tiny3d_Flip();

        switch(osk_event)
        {
            case OSKDIALOG_INPUT_ENTERED:
                oskGetInputText(&output_returned);
                osk_event = 0;
                break;

            case OSKDIALOG_INPUT_CANCELED:
                oskAbort();
                oskUnloadAsync(&output_returned);
                osk_event  = 0;
                osk_action = FAILED;
                break;

            case OSKDIALOG_FINISHED:
                oskUnloadAsync(&output_returned);
                osk_event = 0;
                break;

            default:
                break;
        }

    }

    //usleep(150000); 

    if(output_returned.res == OSK_OK && osk_action == SUCCESS)
		utf16_to_8((u16 *) out, (u8 *) str);
    else ret=FAILED;

end:

    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    sysMemContainerDestroy(container_mem);

    osk_level = 0;
    if(message) free(message);
    if(out) free(out);
    if(in) free(in);

    return ret;
}