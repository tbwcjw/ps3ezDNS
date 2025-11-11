//std
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

//net
#include <arpa/inet.h>
#include <net/net.h>

//psl1ght
#include <io/pad.h>
#include <sysutil/osk.h>
#include <sys/process.h>
#include <tiny3d.h>
#include <libfont2.h>

//local
#include "xreg.h"
#include "debug.h"
#include "font.h"
#include "csv.h"
#include "osk.h"

#define SUCCESS 1
#define FAILURE 0

#define DNS_FLAG_AUTOMATIC  0
#define DNS_FLAG_MANUAL     1
#define DNS_FLAG_STRING(flag) ((flag) == DNS_FLAG_MANUAL ? "Manual" : "Automatic")

#define XREG_PATH           "/dev_flash2/etc/xRegistry.sys"
#define PROFILE_PATH        "/dev_hdd0/tmp/ezDNS.csv"

#define DNS_FLAG_KEY        "/setting/net/dnsFlag"
#define DNS_PRIMARY_KEY     "/setting/net/primaryDns"
#define DNS_SECONDARY_KEY   "/setting/net/secondaryDns"

#define WHITE               0xFFFFFFFF
#define BLACK               0x00000000
#define LIGHT_GREY          0x878787FF
#define DARK_GREY           0x5A5A5AFF
#define TRIANGLE            0x40E2A0FF 
#define CIRCLE              0xFF6666FF
#define CROSS               0x7CB2E8FF
#define SQUARE              0xFF69F8FF

//break main while loop
static volatile int exit_requested = 0;

//error dialog
#define ERR_RECOVERABLE     1
#define ERR_UNRECOVERABLE   0

static volatile int error_recoverable = ERR_RECOVERABLE;
static volatile int error_dialog_buzzer = 0; //has buzzer rung this error?
char *el1, *el2, *el3; //error line 1 2 3 

//restart_countdown
static time_t last_update = 0;
static int restart_countdown = 3;

//pad input controls
typedef struct {
    int BTN_CROSS;
    int BTN_CIRCLE;
    int BTN_SQUARE;
    int BTN_TRIANGLE;
    int BTN_UP;
    int BTN_DOWN;
    int BTN_LEFT;
    int BTN_RIGHT;
    int BTN_SELECT;
    int BTN_START;
} PadButtons;

//single press/release mode
static PadButtons lastPad = {0};
#define PRESSED_NOW(btn)  (paddata.btn && !lastPad.btn) 
#define RELEASED_NOW(btn) (!paddata.btn && lastPad.btn) 

//data structures for profiles
typedef struct {
    char* name;
    int dnsFlag;
    char* primaryDns;
    char* secondaryDns;
} Values;

static Values currentValues = {0}; //the values set by the system
static Values modifiedValues = {0}; //values changed in the app

static Values *savedValueList = NULL; //list of values from file
static int savedValueCount = 0; //index ptr

//cursor position in table
static volatile int cur_pos = 0;
static volatile Values curPosValues = {0};

//window state
typedef enum {
    STATE_NO_DIALOG,
    STATE_RESTART_DIALOG,
    STATE_SAVE_DIALOG,
    STATE_CONFIRMATION_DIALOG,
    STATE_DELETION_CONFIRMATION_DIALOG,
    STATE_NEW_PROFILE_DIALOG,
    STATE_FIRST_RUN_DIALOG,
    STATE_ERROR_DIALOG
} State;
static State currentState = STATE_NO_DIALOG; 

//form validation
typedef enum {
    VALID,
    TOO_MANY_ROWS,
    NAME_LENGTH,
    NAME_COMMA,
    NAME_UNIQUENESS,
    PRIMARY_DNS_LENGTH,
    SECONDARY_DNS_LENGTH,
    PRIMARY_DNS_ADDR_INVALID,
    SECONDARY_DNS_ADDR_INVALID,
    VALIDATION_STATE_COUNT 
} ValidationState;

//form validation helper strings
static const char *validation_state_strings[VALIDATION_STATE_COUNT] = {
    [VALID]                      = "Valid",
    [TOO_MANY_ROWS]              = "Stored profiles full, remove one first",
    [NAME_LENGTH]                = "Profile name must be between 1-20 chars",
    [NAME_COMMA]                 = "Profile name must not contain commas",
    [NAME_UNIQUENESS]            = "Profile name must be unique",
    [PRIMARY_DNS_LENGTH]         = "Primary DNS address has incorrect length",
    [SECONDARY_DNS_LENGTH]       = "Secondary DNS address has incorrect length",
    [PRIMARY_DNS_ADDR_INVALID]   = "Primary DNS address is invalid",
    [SECONDARY_DNS_ADDR_INVALID] = "Secondary DNS address is invalid"
};

//on screen keyboard buffer for new profile form
char osk_name_buf[20];
char osk_primary_buf[15];
char osk_secondary_buf[15];

int sys_soft_reboot() {
    unlink("/dev_hdd0/tmp/turnoff"); //delete turnoff file to avoid bad reboot
    lv2syscall3(379, 0x0200, 0, 0);

    return_to_user_prog(int);
}

int sys_ring_buzzer(int beeps) {
    if (beeps < 1) beeps = 1;
    if (beeps > 3) beeps = 3;

    static const uint64_t args[3][3] = {
        {0x1004, 0x4, 0x6},     //single beep
        {0x1004, 0x7, 0x36},    //two beeps
        {0x1004, 0xa, 0x1b6}    //three beeps
    };

    lv2syscall3(392, args[beeps - 1][0], args[beeps - 1][1], args[beeps - 1][2]);
    return 0;
}

int is_valid_addr(const char *ip) {
    struct sockaddr_in sa;
    int result = inet_pton(AF_INET, ip, &(sa.sin_addr));
    if (result == 1) {
        return SUCCESS;  // valid address
    }
    netDebug("IP address invalid");
    return FAILURE;     // invalid address
}

int get_value_int(xreg_registry_t *reg, char *key_name, int *out) {
    const xreg_key_t *key = xreg_find_key(reg, key_name);
    if(!key) return FAILURE;

    xreg_value_t *val = xreg_find_value_by_key(reg, key);
    if(!val) return FAILURE;
    if(val->value_type != 1) return FAILURE; // 1=int
    if(val->value_length != 4) return FAILURE; //too long

    *out = (val->value_data[0] << 24) |
           (val->value_data[1] << 16) |
           (val->value_data[2] << 8)  |
           (val->value_data[3]);

    return SUCCESS;
}

int get_value_string(xreg_registry_t *reg, 
                    char *key_name, 
                    char **out) {
    const xreg_key_t *key = xreg_find_key(reg, key_name);
    if(!key) return FAILURE;

    xreg_value_t *val = xreg_find_value_by_key(reg, key);
    if(!val) return FAILURE;
    if(val->value_type != 2) return FAILURE; //2=string

    int actual_len = 0;
    while (actual_len < val->value_length && val->value_data[actual_len] != '\0') {
        actual_len++;
    }

    *out = malloc(actual_len + 1);
    if (!*out) return FAILURE;

    for (int i = 0; i < actual_len; ++i) {
        char c = val->value_data[i];
        (*out)[i] = (c >= 32 && c < 127) ? c : '.';
    }
    (*out)[actual_len] = '\0';
    return SUCCESS;
}

int set_value_int(xreg_registry_t *reg, 
                    const char *key_name, 
                    int value) {
    if (!reg || !key_name) return FAILURE;

    int be_value = htonl(value); //convert to big endian
    if (!xreg_update_value(reg, key_name, 1, &be_value, sizeof(be_value))) {
        return FAILURE;
    }

    if (!xreg_save(reg, XREG_PATH)) {
        return FAILURE;
    }
    return SUCCESS;
}

int set_value_string(xreg_registry_t *reg, 
                    const char *key_name, 
                    const char *str) {
    if (!reg || !key_name || !str) return FAILURE;

    size_t len = strlen(str);
    if (!xreg_update_value(reg, key_name, 2, str, len)) {
        return FAILURE;
    }

    if (!xreg_save(reg, XREG_PATH)) {
        return FAILURE; 
    }

    return SUCCESS;
}

int save_modified_values(xreg_registry_t *reg) {
    if(set_value_int(reg, DNS_FLAG_KEY, modifiedValues.dnsFlag) != SUCCESS) {
        netDebug("Failed to set dns flag");
        return FAILURE;
    }
    if(set_value_string(reg, DNS_PRIMARY_KEY, modifiedValues.primaryDns) != SUCCESS) {
        netDebug("Failed to set primary dns");
        return FAILURE;
    }
    if(set_value_string(reg, DNS_SECONDARY_KEY, modifiedValues.secondaryDns) != SUCCESS) {
        netDebug("Failed to set secondary dns");
        return FAILURE;
    }
    return SUCCESS;
}

void load_texture() {
    u32 * texture_mem = tiny3d_AllocTexture(170*1024*1024); 
    u32 * texture_pointer;
    if(!texture_mem) return; //whomp
    texture_pointer = texture_mem;
    ResetFont();
    texture_pointer = (u32 *) AddFontFromBitmapArray((u8 *) font  , (u8 *) texture_pointer, 32, 255, 16, 32, 2, BIT0_FIRST_PIXEL);
}

void draw_horizontal_line(float y_pos, float thickness) {
    tiny3d_SetPolygon(TINY3D_QUADS);

    float z = 65535.0f;

    tiny3d_VertexPos(0.0f, y_pos, z);
    tiny3d_VertexColor(0xFFFFFFFF);

    tiny3d_VertexPos(847.0f, y_pos, z);
    tiny3d_VertexColor(0xFFFFFFFF);

    tiny3d_VertexPos(847.0f, y_pos + thickness, z);
    tiny3d_VertexColor(0xFFFFFFFF);

    tiny3d_VertexPos(0.0f, y_pos + thickness, z);
    tiny3d_VertexColor(0xFFFFFFFF);

    tiny3d_End();

}

void draw_rect(float x, float y, float w, 
                float h, u32 color, float z) {
    tiny3d_SetPolygon(TINY3D_QUADS);

    tiny3d_VertexPos(x, y, z);
    tiny3d_VertexColor(color);

    tiny3d_VertexPos(x + w, y, z);
    tiny3d_VertexColor(color);

    tiny3d_VertexPos(x + w, y + h, z);
    tiny3d_VertexColor(color);

    tiny3d_VertexPos(x, y + h, z);
    tiny3d_VertexColor(color);

    tiny3d_End();
}

void draw_error_dialog() {
    if(!error_recoverable) { //unrecoverable, reset values so we don't trigger the save dialog
        modifiedValues.dnsFlag = currentValues.dnsFlag;
        modifiedValues.primaryDns = currentValues.primaryDns;
        modifiedValues.secondaryDns = currentValues.secondaryDns;
    }
    error_dialog_buzzer = 1; //buzzer fired.
    float z = 65535.0f;

    float dialog_w = 325.0f;
    float dialog_h = 90.0f;
    float dialog_x = (848.0f - dialog_w) / 2.0f;
    float dialog_y = (512.0f - dialog_h) / 2.0f;
    SetFontAutoCenter(1);

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, CIRCLE, z); 
    draw_rect(dialog_x+2.0f, dialog_y+2.0f, dialog_w-4.0f, dialog_h-4.0f, BLACK, z); 

    DrawString(dialog_x+16.0f, dialog_y+4.0f, "An error occurred.");
    draw_rect(dialog_x, dialog_y+18.0f, dialog_w, 1.0f, CIRCLE, z); 

    if(el1 != NULL) DrawString(dialog_x+40.0f, dialog_y+23.0f, el1);
    if(el2 != NULL) DrawString(dialog_x+40.0f, dialog_y+37.0f, el2);
    if(el2 != NULL) DrawString(dialog_x+40.0f, dialog_y+51.0f, el3);
    draw_rect(dialog_x, dialog_y+68.0f, dialog_w, 1.0f, CIRCLE, z); 
    if(error_recoverable) { 
        SetFontColor(SQUARE, BLACK);
        DrawString(dialog_x+40.0f, dialog_y+72.0f, "Press Square to resume...");
        SetFontColor(WHITE, BLACK);
    } else {
        SetFontColor(LIGHT_GREY, BLACK);
        DrawString(dialog_x+40.0f, dialog_y+72.0f, "Unrecoverable. Press SELECT to exit...");
        SetFontColor(WHITE, BLACK);
    }
    SetFontAutoCenter(0);
}

void draw_first_run_dialog() {
    float z = 65535.0f;

    float dialog_w = 325.0f;
    float dialog_h = 90.0f;
    float dialog_x = (848.0f - dialog_w) / 2.0f;
    float dialog_y = (512.0f - dialog_h) / 2.0f;
    SetFontAutoCenter(1);

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, CIRCLE, z); 
    draw_rect(dialog_x+2.0f, dialog_y+2.0f, dialog_w-4.0f, dialog_h-4.0f, BLACK, z); 

    DrawString(dialog_x+16.0f, dialog_y+4.0f, "Welcome to ezDNS");
    draw_rect(dialog_x, dialog_y+18.0f, dialog_w, 1.0f, CIRCLE, z); 

    DrawString(dialog_x+40.0f, dialog_y+23.0f, "On the right are the basic controls.");
    DrawString(dialog_x+40.0f, dialog_y+37.0f, "Support & Issues: github:tbwcjw/ps3ezDNS");
    DrawString(dialog_x+40.0f, dialog_y+51.0f, "Thank you for using ezDNS.");

    draw_rect(dialog_x, dialog_y+68.0f, dialog_w, 1.0f, CIRCLE, z); 

    SetFontColor(SQUARE, BLACK);
    DrawString(dialog_x+40.0f, dialog_y+72.0f, "Press Square to close...");
    SetFontColor(WHITE, BLACK);

    SetFontAutoCenter(0);
}

void draw_reboot_warning() {
    float z = 65535.0f;

    float dialog_w = 300.0f;
    float dialog_h = 60.0f;
    float dialog_x = (848.0f - dialog_w) / 2.0f;
    float dialog_y = (512.0f - dialog_h) / 2.0f;

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, WHITE, z); 
    draw_rect(dialog_x+2.0f, dialog_y+2.0f, dialog_w-4.0f, dialog_h-4.0f, BLACK, z); 

    SetFontAutoCenter(1);                          //restart to take effect
    DrawString(dialog_x+16.0f, dialog_y+4.0f, "Save successful!");
    draw_rect(dialog_x, dialog_y+18.0f, dialog_w, 1.0f, WHITE, z); 

    DrawFormatString(dialog_x+80.0f, dialog_y+32.0f, "Restarting in %i...", restart_countdown);
    SetFontAutoCenter(0);

}

void draw_save_dialog() {
    float z = 65535.0f;

    float dialog_w = 300.0f;
    float dialog_h = 70.0f;
    float dialog_x = (848.0f - dialog_w) / 2.0f;
    float dialog_y = (512.0f - dialog_h) / 2.0f;

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, WHITE, z); // dialog container
    draw_rect(dialog_x + 2.0f, dialog_y + 2.0f, dialog_w - 4.0f, dialog_h - 4.0f, BLACK, z); // dialog content 
    SetFontAutoCenter(1);
    DrawString(dialog_x + 6.0f, dialog_y + 4.0f, "Save changes?"); // header
    SetFontAutoCenter(0);
    draw_rect(dialog_x, dialog_y + 18.0f, dialog_w, 1.0f, WHITE, z);  // horizontal line

    // Each line spaced 14.0f apart
    SetFontColor(SQUARE, BLACK);
    DrawString(dialog_x + 6.0f, dialog_y + 22.0f, "Square:  Discard & Exit");
    SetFontColor(CROSS, BLACK);
    DrawString(dialog_x + 6.0f, dialog_y + 36.0f, "Cross:   Save & Restart");
    SetFontColor(CIRCLE, BLACK);
    DrawString(dialog_x + 6.0f, dialog_y + 50.0f, "Circle:  Cancel & Resume");
    SetFontColor(WHITE, BLACK);
}

void draw_deletion_confirmation_dialog() {
    float z = 65535.0f;

    float dialog_w = 200.0f;
    float dialog_h = 106.0f;
    float dialog_x = (848.0f - dialog_w) / 2.0f;
    float dialog_y = (512.0f - dialog_h) / 2.0f;

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, WHITE, z); // dialog container
    draw_rect(dialog_x + 2.0f, dialog_y + 2.0f, dialog_w - 4.0f, dialog_h - 4.0f, BLACK, z); // dialog content 

    SetFontAutoCenter(1);
    DrawString(dialog_x + 6.0f, dialog_y + 4.0f, "Delete this profile?"); // header
    SetFontAutoCenter(0);
    draw_rect(dialog_x, dialog_y + 18.0f, dialog_w, 1.0f, WHITE, z);  // horizontal line

    float y = dialog_y + 22.0f; // starting Y position

    DrawFormatString(dialog_x + 6.0f, y, "Name:   %s", curPosValues.name);
    y += 14.0f;
    if(strlen(curPosValues.primaryDns) < 1) {
        DrawString(dialog_x + 6.0f, y, "DNS 1:  <auto>");
    } else {
        DrawFormatString(dialog_x + 6.0f, y, "DNS 1:  %s", curPosValues.primaryDns);
    }
    y += 14.0f;
    if(strlen(curPosValues.secondaryDns) < 1) {
        DrawString(dialog_x + 6.0f, y, "DNS 2:  <auto>");
    } else {
        DrawFormatString(dialog_x + 6.0f, y, "DNS 2:  %s", curPosValues.secondaryDns);
    }
    

    draw_rect(dialog_x, y + 18.0f, dialog_w, 1.0f, WHITE, z);  // horizontal line
    y += 24.0f; //skip a line
    SetFontColor(CROSS, BLACK);
    DrawString(dialog_x + 6.0f, y,       "Cross:  Delete");
    y += 14.0f;
    SetFontColor(CIRCLE, BLACK);
    DrawString(dialog_x + 6.0f, y,       "Circle: Cancel");
    y += 14.0f;
    SetFontColor(WHITE, BLACK);
    }

void draw_confirmation_dialog() {
    float z = 65535.0f;

    float dialog_w = 200.0f;
    float dialog_h = 106.0f;
    float dialog_x = (848.0f - dialog_w) / 2.0f;
    float dialog_y = (512.0f - dialog_h) / 2.0f;

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, WHITE, z); // dialog container
    draw_rect(dialog_x + 2.0f, dialog_y + 2.0f, dialog_w - 4.0f, dialog_h - 4.0f, BLACK, z); // dialog content 

    SetFontAutoCenter(1);
    DrawString(dialog_x + 6.0f, dialog_y + 4.0f, "Activate this profile?"); // header
    SetFontAutoCenter(0);
    draw_rect(dialog_x, dialog_y + 18.0f, dialog_w, 1.0f, WHITE, z);  // horizontal line

    float y = dialog_y + 22.0f; // starting Y position

    DrawFormatString(dialog_x + 6.0f, y, "Name:   %s", curPosValues.name);
    y += 14.0f;
    if(strlen(curPosValues.primaryDns) < 1) {
        DrawString(dialog_x + 6.0f, y, "DNS 1:  <auto>");
    } else {
        DrawFormatString(dialog_x + 6.0f, y, "DNS 1:  %s", curPosValues.primaryDns);
    }
    y += 14.0f;
    if(strlen(curPosValues.secondaryDns) < 1) {
        DrawString(dialog_x + 6.0f, y, "DNS 2:  <auto>");
    } else {
        DrawFormatString(dialog_x + 6.0f, y, "DNS 2:  %s", curPosValues.secondaryDns);
    }
    

    draw_rect(dialog_x, y + 18.0f, dialog_w, 1.0f, WHITE, z);  // horizontal line
    y += 24.0f; //skip a line
    SetFontColor(CROSS, BLACK);
    DrawString(dialog_x + 6.0f, y,       "Cross:  Save & Restart");
    y += 14.0f;
    SetFontColor(CIRCLE, BLACK);
    DrawString(dialog_x + 6.0f, y,       "Circle: Cancel & Resume");
    y += 14.0f;
    SetFontColor(WHITE, BLACK);
    }

void draw_profile_table() {
    float z = 65535.0f;

    float dialog_w = 586.0f;
    float dialog_h = 458.0f;
    float dialog_x = 0.0f;
    float dialog_y = 24.0f;

    float row_height = 20.0f;

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, WHITE, z); 
    draw_rect(dialog_x+1.0f, dialog_y+1.0f, dialog_w-2.0f, dialog_h-2.0f, BLACK, z); 

    //headers
    DrawString(dialog_x+12.0f, dialog_y+4.0f,  "Name");
    draw_rect(184.0f, dialog_y, 0.5f, dialog_h, WHITE, z);  //col divider
    DrawString(dialog_x+196.0f, dialog_y+4.0f,  "Primary (DNS 1)");
    draw_rect(383.0f, dialog_y, 0.5f, dialog_h, WHITE, z);  //col divider
    DrawString(dialog_x+399.0f, dialog_y+4.0f,  "Secondary (DNS 2)");
    draw_rect(dialog_x, dialog_y+20.0f, dialog_w, 1.0f, WHITE, z);  //row divider

    //items
    for(int i = 0; i < savedValueCount; i++) {
        if(i == 0 && cur_pos != 0) SetFontColor(LIGHT_GREY, BLACK); //visually "disable" current profile entry
        if(i == cur_pos) SetFontColor(CROSS, BLACK);
        float row_y = dialog_y + row_height + i * row_height - 1.0f;
        DrawString(dialog_x+12.0f, row_y+4.0f, savedValueList[i].name);
        if(strcmp(savedValueList[i].primaryDns, currentValues.primaryDns) == 0) {
            DrawString(dialog_x+188.0f, row_y+4.0f, "+");
        }
        if(strlen(savedValueList[i].primaryDns) < 1) {
            DrawString(dialog_x+200.0f, row_y+4.0f, "<auto>");
        } else {
            DrawString(dialog_x+200.0f, row_y+4.0f, savedValueList[i].primaryDns);
        }
        
        if(strcmp(savedValueList[i].secondaryDns, currentValues.secondaryDns) == 0) {
            DrawString(dialog_x+387.0f, row_y+4.0f, "+");
        }
        if(strlen(savedValueList[i].secondaryDns) < 1) {
            DrawString(dialog_x+399.0f, row_y+4.0f, "<auto>");
        } else {
            DrawString(dialog_x+399.0f, row_y+4.0f, savedValueList[i].secondaryDns);
        }
        SetFontColor(WHITE, BLACK);
        draw_rect(dialog_x, row_y+row_height, dialog_w, 1.0f, WHITE, z);  //row divider
    }
}

static volatile int cur_pos_new_profile_dialog = 0;

void draw_new_profile_dialog() {
    float z = 65535.0f;

    float dialog_w = 300.0f;
    float dialog_h = 120.0f;
    float dialog_x = (848.0f - dialog_w) / 2.0f;
    float dialog_y = (512.0f - dialog_h) / 2.0f;

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, WHITE, z); // dialog container
    draw_rect(dialog_x + 2.0f, dialog_y + 2.0f, dialog_w - 4.0f, dialog_h - 4.0f, BLACK, z); // dialog content 

    SetFontAutoCenter(1);
    DrawString(dialog_x + 6.0f, dialog_y + 4.0f, "Create a profile"); // header
    SetFontAutoCenter(0);
    draw_rect(dialog_x, dialog_y + 18.0f, dialog_w, 1.0f, WHITE, z);  // horizontal line

    float y = dialog_y + 22.0f; // starting Y position

    if(cur_pos_new_profile_dialog == 0)  {
        SetFontColor(CROSS, BLACK);
    } else {
        SetFontColor(WHITE, BLACK);
    }
    DrawFormatString(dialog_x + 6.0f, y, "Name: %s", osk_name_buf);
    y += 14.0f;

    if(cur_pos_new_profile_dialog == 1)  {
        SetFontColor(CROSS, BLACK);
    } else {
        SetFontColor(WHITE, BLACK);
    }
    DrawFormatString(dialog_x + 6.0f, y, "DNS 1: %s", osk_primary_buf);

    if(cur_pos_new_profile_dialog == 2)  {
        SetFontColor(CROSS, BLACK);
    } else {
        SetFontColor(WHITE, BLACK);
    }

    y += 14.0f;
    DrawFormatString(dialog_x + 6.0f, y, "DNS 2: %s", osk_secondary_buf);

    draw_rect(dialog_x, y + 18.0f, dialog_w, 1.0f, WHITE, z);  // horizontal line
    y += 24.0f; //skip a line
    SetFontColor(CROSS, BLACK);
    DrawString(dialog_x + 6.0f, y,       "Cross:   Edit value");
    y += 14.0f;
    SetFontColor(SQUARE, BLACK);
    DrawString(dialog_x + 6.0f, y,       "Square:  Save");
    y += 14.0f;
    SetFontColor(CIRCLE, BLACK);
    DrawString(dialog_x + 6.0f, y,       "Circle:  Discard & Cancel");
    y += 14.0f;
    SetFontColor(WHITE, BLACK);
}
void draw_controls_box() {
    float z = 65535.0f;

    float dialog_w = 250.0f;
    float dialog_h = 458.0f;
    float dialog_x = 597.0f;
    float dialog_y = 24.0f;

    draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, WHITE, z); 
    draw_rect(dialog_x+1.0f, dialog_y+1.0f, dialog_w-2.0f, dialog_h-2.0f, BLACK, z); 
    DrawString(dialog_x+6.0f, dialog_y+4.0f,  "Controls:");
    SetFontColor(LIGHT_GREY, BLACK);
    DrawString(dialog_x+6.0f, dialog_y+18.0f,  "Start:     Create Profile");
    DrawString(dialog_x+6.0f, dialog_y+30.0f, "Select:    Exit ezDNS");
    SetFontColor(DARK_GREY, BLACK);
    DrawString(dialog_x+6.0f, dialog_y+42.0f, "Up/Down:   Move Cursor");
    SetFontColor(CROSS, BLACK);
    DrawString(dialog_x+6.0f, dialog_y+54.0f, "Cross:     Select Profile");
    SetFontColor(CIRCLE, BLACK);
    DrawString(dialog_x+6.0f, dialog_y+66.0f, "Circle:    Delete Profile");
    SetFontColor(TRIANGLE, BLACK);
    DrawString(dialog_x+6.0f, dialog_y+78.0f, "Triangle:  Switch DNS Mode");
    SetFontColor(WHITE,BLACK);
    DrawString(dialog_x+6.0f, dialog_y+90.0f, ""); //lb
    DrawString(dialog_x+6.0f, dialog_y+102.0f, "Legend:"); //lb
    DrawString(dialog_x+6.0f, dialog_y+114.0f,  "* = unsaved value");
    DrawString(dialog_x+6.0f, dialog_y+126.0f, "+ =  active value");
    DrawString(dialog_x+6.0f, dialog_y+138.0f, ""); //lb
    DrawString(dialog_x+6.0f, dialog_y+150.0f, "DNS Modes:");
    DrawString(dialog_x+6.0f, dialog_y+162.0f, "Automatic: System gets DNS");
    DrawString(dialog_x+6.0f, dialog_y+174.0f, "values from DHCP. Our custom");
    DrawString(dialog_x+6.0f, dialog_y+186.0f, "values will be ignored...");
    DrawString(dialog_x+6.0f, dialog_y+198.0f, ""); //lb
    DrawString(dialog_x+6.0f, dialog_y+198.0f, "Manual: System will use DNS");
    DrawString(dialog_x+6.0f, dialog_y+210.0f, "info that we provide. Use");
    DrawString(dialog_x+6.0f, dialog_y+222.0f, "this mode to set custom DNS");
    DrawString(dialog_x+6.0f, dialog_y+234.0f, "values...");
    DrawString(dialog_x+6.0f, dialog_y+248.0f, ""); //lb
    DrawString(dialog_x+6.0f, dialog_y+258.0f, "Beeps:");
    DrawString(dialog_x+6.0f, dialog_y+270.0f, "1: An error occurred. Spawned");
    DrawString(dialog_x+6.0f, dialog_y+282.0f, "An error dialog.");
    DrawString(dialog_x+6.0f, dialog_y+294.0f, "2: Console restarting after");
    DrawString(dialog_x+6.0f, dialog_y+308.0f, "saving values to registry.");
    DrawString(dialog_x+6.0f, dialog_y+320.0f, ""); //lb
    DrawString(dialog_x+6.0f, dialog_y+332.0f, "xRegistry path:");
    DrawString(dialog_x+6.0f, dialog_y+344.0f, XREG_PATH);
    DrawString(dialog_x+6.0f, dialog_y+356.0f, ""); //lb
    DrawString(dialog_x+6.0f, dialog_y+368.0f, "ezDNS data path:");
    DrawString(dialog_x+6.0f, dialog_y+380.0f, PROFILE_PATH);

    draw_rect(dialog_x, dialog_y + 428.0f, dialog_w, 1.0f, WHITE, z); 
    DrawFormatString(dialog_x+6.0f, dialog_y+436.0f, "Stored profiles: %i/%i (max)", savedValueCount-2, ROW_CAPACITY);
}

void draw_header() {
    DrawFormatString(0,0, "v%s", VERSION);
    SetFontAutoCenter(1);
    DrawString(350,0, "ezDNS");
    SetFontAutoCenter(0);
    DrawString(670,0, "github:tbwcjw/ps3ezDNS");
    draw_horizontal_line(14.0f, 1.0f);
}

void draw_footer() { 
    draw_horizontal_line(492.0f, 1.0f);

    
    if(modifiedValues.dnsFlag != currentValues.dnsFlag) { //asterisk to indicated modified, unsaved values
        DrawFormatString(10,500, "*Mode: %s", DNS_FLAG_STRING(modifiedValues.dnsFlag));
    } else {
        DrawFormatString(10,500, "Mode: %s", DNS_FLAG_STRING(modifiedValues.dnsFlag));
    }

    SetFontAutoCenter(1);
    if(strcmp(modifiedValues.primaryDns, currentValues.primaryDns) != 0) {                     //if modified
        if(strlen(modifiedValues.primaryDns) < 1) {                                 // if the dns is "auto"
            DrawString(640,500, "*Primary: <auto>");                          // write <auto> w/ asterish
        } else {
            DrawFormatString(640,500, "*Primary: %s", modifiedValues.primaryDns);   // else the value w/asterisk
        }
    } else {
        if(strlen(currentValues.primaryDns) < 1) {
            DrawString(640,500, "Primary: <auto>");
        } else {
            DrawFormatString(640,500, "Primary: %s", currentValues.primaryDns);
        }
    } 
    SetFontAutoCenter(0);
    
    
    if(strcmp(modifiedValues.secondaryDns, currentValues.secondaryDns) != 0) {
        if(strlen(modifiedValues.secondaryDns) < 1) {
            DrawString(640,500, "*Secondary: <auto>");
        } else {
            DrawFormatString(640,500, "*Secondary: %s", modifiedValues.secondaryDns);
        }
        
    } else {
        if(strlen(currentValues.secondaryDns) < 1) {
            DrawString(640,500, "Secondary: <auto>");
        } else {
            DrawFormatString(640,500, "Secondary: %s", currentValues.secondaryDns);
        }
    }
}

void throw_error(int recoverable, 
                    const char* l1, 
                    const char* l2, 
                    const char* l3) {
    error_recoverable = recoverable;
    el1 = strdup(l1);
    el2 = strdup(l2);
    el3 = strdup(l3);
    currentState = STATE_ERROR_DIALOG;
    netDebug("error (%s): %s: %s, %s", recoverable ? "recoverable" : "unrecoverable", el1, el2, el3);
}

int profiles_csv_exists() {
    FILE *file = fopen(PROFILE_PATH, "r");
    if (file) {
        fclose(file);
        return SUCCESS;
    }
    return FAILURE;
}

int load_profiles_csv() {
    if (!profiles_csv_exists()) {
        char *header[] = {"name","primary","secondary"};
        if (csv_create(PROFILE_PATH, header, 3, ',') != 1)
            return FAILURE;
        return SUCCESS;
    }

    CSVTable table = csv_read_all(PROFILE_PATH, ',');
    if (table.count == 0) {
        char *header[] = {"name","primary","secondary"};
        csv_create(PROFILE_PATH, header, 3, ',');
        return SUCCESS;
    }

    size_t oldCount = savedValueCount;
    savedValueList = realloc(savedValueList,
                             (savedValueCount + table.count - 1) * sizeof(Values));

    for (size_t i = 1; i < table.count; i++) {
        CSVRow *row = &table.rows[i];
        if (row->count < 3) continue;
        savedValueList[oldCount].name = strdup(row->fields[0]);
        savedValueList[oldCount].dnsFlag = DNS_FLAG_MANUAL;
        savedValueList[oldCount].primaryDns = strdup(row->fields[1]);
        savedValueList[oldCount].secondaryDns = strdup(row->fields[2]);
        oldCount++;
    }

    savedValueCount = oldCount;
    csv_free_table(&table);
    return SUCCESS;
}


void strtolower(const char *str, 
                char *out, 
                size_t out_size) {
    size_t i;
    for (i = 0; i < out_size - 1 && str[i]; i++) {
        out[i] = tolower((unsigned char)str[i]);
    }
    out[i] = '\0';
}

const char *validation_state_to_string(ValidationState state) {
    if (state < 0 || state >= VALIDATION_STATE_COUNT)
        return "Unknown validation state";
    return validation_state_strings[state];
}

ValidationState validate_new_profile_form() {
    if(savedValueCount-2 >= ROW_CAPACITY) return TOO_MANY_ROWS; //ignore 2; we have "Current" and "System default"
    if(strlen(osk_name_buf) < 1) return NAME_LENGTH; // maxlen is handled by osk buffers

    //unique check
    for(int i = 0; i < savedValueCount; i++) {
        char lower_osk_name_buf[sizeof(osk_name_buf)];
        char lower_name[sizeof(osk_name_buf)];

        strtolower(osk_name_buf, lower_osk_name_buf, sizeof(lower_osk_name_buf));
        strtolower(savedValueList[i].name, lower_name, sizeof(lower_name));
        
        if(strcmp(lower_osk_name_buf, lower_name) == 0) return NAME_UNIQUENESS;
    }

    for(int i = 0; osk_name_buf[i] != '\0'; i++) {
        if(osk_name_buf[i] == ',') return NAME_COMMA;
    }

    if(strlen(osk_primary_buf) < 7) return PRIMARY_DNS_LENGTH;
    if(is_valid_addr(osk_primary_buf) == FAILURE) return PRIMARY_DNS_ADDR_INVALID;

    if(strlen(osk_secondary_buf) < 7) return SECONDARY_DNS_LENGTH;
    if(is_valid_addr(osk_secondary_buf) == FAILURE) return SECONDARY_DNS_ADDR_INVALID;

    return VALID;
}

void add_saved_value(Values **list, int *count, Values newVal) {
    *list = realloc(*list, (*count + 1) * sizeof(Values));
    (*list)[*count].name = strdup(newVal.name);
    (*list)[*count].dnsFlag = newVal.dnsFlag;
    (*list)[*count].primaryDns = strdup(newVal.primaryDns);
    (*list)[*count].secondaryDns = strdup(newVal.secondaryDns);
    (*count)++;
}

int delete_profile() {
    netDebug("%s", curPosValues.name);

    //remove row from csv
    if(csv_remove_row(PROFILE_PATH, 0, curPosValues.name, ',') != 1) {
        return FAILURE;
    }
    netDebug("Removed row from csv");

    //search for and remove in savedValueList
    for(int i=0; i < savedValueCount; i++) {
        if(savedValueList[i].name && strcmp(savedValueList[i].name, curPosValues.name) == 0) { //found
            //shift left
            for(int j = i; j < savedValueCount; j++) {
                savedValueList[j] = savedValueList[j+1];
            }
            savedValueCount--;
            netDebug("Removed row from savedValueList");
            return SUCCESS;
        }
    }
    return FAILURE;
}

void reset_new_profile_form() {
    memset(osk_name_buf, 0, sizeof(osk_name_buf));
    memset(osk_primary_buf, 0, sizeof(osk_primary_buf));
    memset(osk_secondary_buf, 0, sizeof(osk_secondary_buf));
}
int main(int argc, char **argv) {
    //initialize tiny3d
    tiny3d_Init(1024*1024);
    tiny3d_Project2D();
    load_texture();

    //set up font
    SetCurrentFont(0);
    SetFontSize(8,13);
    SetFontColor(WHITE, BLACK);
    SetFontAutoCenter(0);
    SetFontZ(65535.0f);

    //initialize pad
    ioPadInit(7);
    padInfo padinfo;
	padData paddata;
    memset(&paddata, 0, sizeof(paddata));
    
    //initialize network/debugging
    netInitialize();
    netDebugInit();
    netDebug("Hello!");

    //load registry
    xreg_registry_t *reg = xreg_load(XREG_PATH);
    if (!reg) {
        netDebug("Failed to load xRegistry file");
        throw_error(ERR_UNRECOVERABLE, "Failed to load the xRegistry file.", "Ensure it exists at:", XREG_PATH);
    }

    if(get_value_int(reg, DNS_FLAG_KEY, &currentValues.dnsFlag) != SUCCESS) {
        netDebug("Failed to read DNS flag");
        throw_error(ERR_UNRECOVERABLE, "Failed to read DNS flag", "This is most probably a bug.", "Report it on Github.");
    } else {
        netDebug("%d", currentValues.dnsFlag);
    }

    if (get_value_string(reg, DNS_PRIMARY_KEY, &currentValues.primaryDns) != SUCCESS) {
        throw_error(ERR_RECOVERABLE, "Failed to read primary DNS", "This is most probably a bug.", "Report it on Github.");
    } else {
        netDebug(currentValues.primaryDns);
    }

    if (get_value_string(reg, DNS_SECONDARY_KEY, &currentValues.secondaryDns) != SUCCESS) {
        throw_error(ERR_RECOVERABLE, "Failed to read secondary DNS", "This is most probably a bug.", "Report it on Github.");
    } else {
        netDebug(currentValues.secondaryDns);
    }

    //set default from system
    modifiedValues.dnsFlag = currentValues.dnsFlag;
    modifiedValues.primaryDns = currentValues.primaryDns;
    modifiedValues.secondaryDns = currentValues.secondaryDns;

    Values current = {"Current", currentValues.dnsFlag, currentValues.primaryDns, currentValues.secondaryDns};
    add_saved_value(&savedValueList, &savedValueCount, current);

    Values sys_default = {"System Default", DNS_FLAG_AUTOMATIC, "", ""};
    add_saved_value(&savedValueList, &savedValueCount, sys_default);

    if(!profiles_csv_exists()) { //csv file doesnt exist assume first run
        currentState = STATE_FIRST_RUN_DIALOG;
    }
    //create+load/load profiles csv
    if(load_profiles_csv() != SUCCESS) {
        throw_error(ERR_UNRECOVERABLE, "Failed to load data.", "The file may not exist or has ", "malformed data; check for empty lines.");
    }

    while(!exit_requested) {
        //clear screen
        tiny3d_Clear(0xff000000, TINY3D_CLEAR_ALL);
        
        //handle pad input
        ioPadGetInfo(&padinfo);
        if (padinfo.status[0] && ioPadGetData(0, &paddata) == 0) {
            //save dialog entry: exit requested, check for changes, run save dialog if required.
            if (PRESSED_NOW(BTN_SELECT) && currentState != STATE_SAVE_DIALOG) {
                if(currentValues.dnsFlag != modifiedValues.dnsFlag) { //changes have been made, confirm exit. all other changes are made at time user selects from table
                    currentState = STATE_SAVE_DIALOG;
                } else {
                    exit_requested = 1;     //no changes have been made, exit.
                }
            }
            //11-10-25: fixed a bug where circle on save_dialog would open the deletion_confirmation dialog
            //save dialog: X button saves and restarts
            else if (PRESSED_NOW(BTN_CROSS) && currentState == STATE_SAVE_DIALOG) {
                currentState = STATE_RESTART_DIALOG;
                restart_countdown = 3;
                time(&last_update);
            }
            //save dialog: Square exits without saving.
            else if (PRESSED_NOW(BTN_SQUARE) && currentState == STATE_SAVE_DIALOG) {
                exit_requested = 1;
            }

            //exit dialog: circle close dialog (cancel)
            else if (PRESSED_NOW(BTN_CIRCLE) && currentState == STATE_SAVE_DIALOG) {
                currentState = STATE_NO_DIALOG;
            }

            //confirmation dialog: open confirmation dialog from table
            else if (PRESSED_NOW(BTN_CROSS) && currentState == STATE_NO_DIALOG) {
                
                //11-10-25: disallow setting of "current" profile.
                if(cur_pos != 0) {
                    currentState = STATE_CONFIRMATION_DIALOG;
                    modifiedValues.dnsFlag = curPosValues.dnsFlag;
                    modifiedValues.primaryDns = curPosValues.primaryDns;
                    modifiedValues.secondaryDns = curPosValues.secondaryDns;
                }
            } else if (PRESSED_NOW(BTN_CROSS) && currentState == STATE_CONFIRMATION_DIALOG) { //save confirmed, restart
                currentState = STATE_RESTART_DIALOG;
                restart_countdown = 3;
                time(&last_update);
            }else if (PRESSED_NOW(BTN_CIRCLE) && currentState == STATE_CONFIRMATION_DIALOG) { //discard changes and resume
                currentState = STATE_NO_DIALOG;

                modifiedValues.dnsFlag = currentValues.dnsFlag;
                modifiedValues.primaryDns = currentValues.primaryDns;
                modifiedValues.secondaryDns = currentValues.secondaryDns;
            }

            //deletion confirumation dialog: open from table, ignore first element
            else if(PRESSED_NOW(BTN_CIRCLE) && currentState == STATE_NO_DIALOG) { 
                if(cur_pos > 1) { // skip two
                    currentState = STATE_DELETION_CONFIRMATION_DIALOG; //open dialog
                }                
            } else if(PRESSED_NOW(BTN_CIRCLE) && currentState == STATE_DELETION_CONFIRMATION_DIALOG) { //cancel
                currentState = STATE_NO_DIALOG;
            } else if(PRESSED_NOW(BTN_CROSS) && currentState == STATE_DELETION_CONFIRMATION_DIALOG) { //confirm
                if(delete_profile() != SUCCESS) {
                    throw_error(ERR_RECOVERABLE, "Failed to delete profile", "Perhaps the file is locked?", "Try again later");
                }
                netDebug("cur pos %i", cur_pos);
                //11-10-25: instead of moving cursor up one, reset cursor to 0.
                cur_pos = 0;
                currentValues = savedValueList[0];
                currentState = STATE_NO_DIALOG;
            }

            //new profile dialog: open with start
            if(PRESSED_NOW(BTN_START) && currentState == STATE_NO_DIALOG) {
                currentState = STATE_NEW_PROFILE_DIALOG;
            } else if(PRESSED_NOW(BTN_CIRCLE) && currentState == STATE_NEW_PROFILE_DIALOG) { //close dialog
                //discard changes
                reset_new_profile_form();
                //close dialog
                cur_pos_new_profile_dialog = 0;
                currentState = STATE_NO_DIALOG;
            } else if(PRESSED_NOW(BTN_CROSS) && currentState == STATE_NEW_PROFILE_DIALOG) { //edit cur pos item value
                if(cur_pos_new_profile_dialog == 0) { //name
                    get_osk_string("Name", osk_name_buf, sizeof(osk_name_buf));
                }
                if(cur_pos_new_profile_dialog == 1) { //primary dns
                    get_osk_string("Primary DNS", osk_primary_buf, sizeof(osk_primary_buf));
                }
                if(cur_pos_new_profile_dialog == 2) { //secondary dns
                    get_osk_string("Secondary DNS", osk_secondary_buf, sizeof(osk_secondary_buf));
                }
                
            } else if(PRESSED_NOW(BTN_SQUARE) && currentState == STATE_NEW_PROFILE_DIALOG) { //save values
                //validate form
                ValidationState valid = validate_new_profile_form();
                if (valid == VALID) {
                    //save fields in savedValueList & to file.
                    Values newProfile = {osk_name_buf, DNS_FLAG_MANUAL, osk_primary_buf, osk_secondary_buf};
                    add_saved_value(&savedValueList, &savedValueCount, newProfile);
                    char *fields[] = {osk_name_buf, osk_primary_buf, osk_secondary_buf};
                    if(csv_append_row(PROFILE_PATH, fields, 3, ',') == 0) {
                        throw_error(ERR_RECOVERABLE, "Failed to save to file.", "This is most probably a bug.", "Report it on Github.");
                    }
                    //reset form and exit
                    reset_new_profile_form();
                    currentState = STATE_NO_DIALOG;
                } else {
                    throw_error(ERR_RECOVERABLE, "Invalid values in form.", validation_state_to_string(valid), "Please try again");
                }
            //up/down movement in form
            } else if(PRESSED_NOW(BTN_DOWN) && currentState == STATE_NEW_PROFILE_DIALOG) { //move cur pos down
                cur_pos_new_profile_dialog++;
                if(cur_pos_new_profile_dialog > 2 || cur_pos_new_profile_dialog < 0) cur_pos_new_profile_dialog = 0;
            } else if(PRESSED_NOW(BTN_UP) && currentState == STATE_NEW_PROFILE_DIALOG) { //move cur pos up
                cur_pos_new_profile_dialog--;
                if(cur_pos_new_profile_dialog > 2 || cur_pos_new_profile_dialog < 0) cur_pos_new_profile_dialog = 2;
            }

            //error dialog: recoverable
            else if (PRESSED_NOW(BTN_SQUARE) && currentState == STATE_ERROR_DIALOG && error_recoverable == 1) {
                error_dialog_buzzer = 0; //reset buzzer flag for next error
                currentState = STATE_NO_DIALOG;
            } else if (PRESSED_NOW(BTN_SELECT) && currentState == STATE_ERROR_DIALOG && error_recoverable != 1) { //not recoverable
                error_dialog_buzzer = 0;
                currentState = STATE_NO_DIALOG; //these won't do anything, but for continuity sake we'll put them here.
                break;
            }

            //move cursor in table: move up
            else if (PRESSED_NOW(BTN_DOWN) && currentState == STATE_NO_DIALOG) {
                cur_pos++;
                
                if (cur_pos >= savedValueCount) cur_pos = 0;
                netDebug("Current pos: %i", cur_pos);
                curPosValues = savedValueList[cur_pos]; //set active item by cursor
            }

            //move cursor in table: move down
            else if (PRESSED_NOW(BTN_UP) && currentState == STATE_NO_DIALOG) {
                cur_pos--;
                
                if (cur_pos < 0) cur_pos = savedValueCount - 1;
                netDebug("Current pos: %i", cur_pos);
                curPosValues = savedValueList[cur_pos]; //set active item by cursor
            }

            //change dns mode
            else if (PRESSED_NOW(BTN_TRIANGLE) && currentState == STATE_NO_DIALOG) {
                modifiedValues.dnsFlag = !modifiedValues.dnsFlag;
            }

            //firsty run dialog: square to close
            else if (PRESSED_NOW(BTN_SQUARE) && currentState == STATE_FIRST_RUN_DIALOG) {
                currentState = STATE_NO_DIALOG;
            }
            
            //reset pad events
            lastPad = (PadButtons){
                .BTN_CROSS   = paddata.BTN_CROSS,
                .BTN_CIRCLE  = paddata.BTN_CIRCLE,
                .BTN_SQUARE  = paddata.BTN_SQUARE,
                .BTN_TRIANGLE= paddata.BTN_TRIANGLE,
                .BTN_UP      = paddata.BTN_UP,
                .BTN_DOWN    = paddata.BTN_DOWN,
                .BTN_LEFT    = paddata.BTN_LEFT,
                .BTN_RIGHT   = paddata.BTN_RIGHT,
                .BTN_SELECT  = paddata.BTN_SELECT,
                .BTN_START   = paddata.BTN_START
            };
        }

        //draw always visible elements
        draw_header();
        draw_profile_table();
        draw_controls_box();
        draw_footer();

        // save to registry and restart system.
        if (currentState == STATE_RESTART_DIALOG) {
            time_t current_time;
            time(&current_time);
            
            if(save_modified_values(reg) != SUCCESS) { //save to xregistry
                netDebug("Failed to save modified values");
                throw_error(0, "Failed to save modified values.", "The registry has not been changed.", "Check /dev_flash2/etc/xRegistry.sys exists");
            }
            if (difftime(current_time, last_update) >= 1.0) {
                restart_countdown--;
                last_update = current_time;

                if (restart_countdown <= 0) {
                    sys_ring_buzzer(2);
                    xreg_free(reg);
                    sys_soft_reboot();
                }
            }

            draw_reboot_warning();
        }

        if (currentState == STATE_FIRST_RUN_DIALOG) {
            draw_first_run_dialog();
        }

        if (currentState == STATE_ERROR_DIALOG) {
            if(!error_dialog_buzzer) sys_ring_buzzer(1);
            draw_error_dialog();
        }

        if(currentState == STATE_NEW_PROFILE_DIALOG) {
            draw_new_profile_dialog();
        }
        
        if (currentState == STATE_SAVE_DIALOG) {
            draw_save_dialog();
        }

        if(currentState == STATE_DELETION_CONFIRMATION_DIALOG) {
            draw_deletion_confirmation_dialog();
        }
        if (currentState == STATE_CONFIRMATION_DIALOG) {
           draw_confirmation_dialog();
        }

        tiny3d_Flip();
    }

    xreg_free(reg);
    #ifdef PS3LOADX
    sysProcessExitSpawn2("/dev_hdd0/game/PSL145310/RELOAD.SELF", NULL, NULL, NULL, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
    #endif
    return 0;
}