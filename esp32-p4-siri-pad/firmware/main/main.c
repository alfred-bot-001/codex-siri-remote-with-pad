#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "remote.h"
#include "usb_keyboard.h"

LV_FONT_DECLARE(font_cn28);
static const char *TAG = "siri-pad-p4";
static const uint32_t BG=0xf7f8fa, WHITE=0xffffff, BLUE=0x245bff, DARK=0x142235, MUTED=0x7e8c9d, PALE=0xeaf0ff;

enum { REM_LEFT, REM_RIGHT, REM_SELECT, REM_PLAY, REM_MINUS, REM_PLUS, REM_TV, REM_VOICE,
       REM_UP, REM_DOWN, REM_BACK, REM_MUTE, REM_POWER, REM_COUNT };
enum { MAP_NONE, MAP_KEY, MAP_VOICE };
enum { MOD_CTRL=1, MOD_OPT=2, MOD_CMD=4, MOD_SHIFT=8 };
typedef struct { uint8_t type,key,mods,option; } map_entry_t;
typedef struct { uint32_t magic; map_entry_t entry[REM_COUNT]; } mapping_t;
static mapping_t mapping;
static bool nvs_ready;
static const char *button_names[REM_COUNT] = {
    "圆环左键", "圆环右键", "中央确认", "播放暂停", "减号", "加号", "小电视", "语音键",
    "圆环上键", "圆环下键", "返回键", "静音键", "电源键"
};
static const uint8_t keys[]={0x2c,0x28,0x50,0x4f,0x52,0x51,0x2a,0x29,0x2b,
    0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,
    0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d};
static const char *key_names[]={"空格","回车","左箭头","右箭头","上箭头","下箭头","退格","Esc","Tab",
    "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
#define KEY_COUNT (sizeof(keys)/sizeof(keys[0]))
static lv_obj_t *root,*home,*settings,*bt_page,*map_page,*mapping_list,*notice,*home_left_note,*home_right_note,*home_tv_note,*usb_state;
static lv_obj_t *tab_bt,*tab_map,*reset_button,*editor,*editor_type,*editor_key,*editor_option,*editor_mods[4],*editor_preview,*keyboard_group;
static int editing_index=-1;
static map_entry_t draft;
static void inform(const char *message);
static bool reset_armed;
static lv_obj_t *bt_header,*bt_state,*bt_peer,*bt_detail,*bt_scan_note,*bt_candidates[REMOTE_CANDIDATES],*bt_forget;
static uint32_t forget_until;
static uint16_t previous_buttons;
static uint8_t hid_modifiers(uint8_t m){return ((m&MOD_CTRL)?0x01:0)|((m&MOD_OPT)?0x04:0)|((m&MOD_CMD)?0x08:0)|((m&MOD_SHIFT)?0x02:0);}
static void send_entry(const map_entry_t *entry){
    if(entry->type==MAP_KEY)usb_keyboard_send(hid_modifiers(entry->mods),entry->key);
}
// Called from the BLE host task. Keep it nonblocking; USB reports are queued for the USB task.
void pad_remote_buttons(uint16_t mask){
    static const uint16_t bits[REM_COUNT]={0x1000,0x0400,0x0008,0x0100,0x0004,0x0002,0x0001,0x0020};
    uint16_t newly_pressed=mask&~previous_buttons;
    previous_buttons=mask;
    if(!usb_keyboard_connected())return;
    for(int i=0;i<8;i++){
        if(!(newly_pressed&bits[i]))continue;
        if(i==REM_LEFT||i==REM_RIGHT||i==REM_TV)send_entry(&mapping.entry[i]);
    }
    uint8_t modifier=0,key=0;
    for(int i=REM_SELECT;i<=REM_VOICE;i++){
        if(!(mask&bits[i]))continue;
        const map_entry_t *entry=&mapping.entry[i];
        if(entry->type==MAP_VOICE)modifier|=entry->option?0x40:0x04;
        else if(entry->type==MAP_KEY){modifier|=hid_modifiers(entry->mods);if(!key)key=entry->key;}
    }
    usb_keyboard_set(modifier,key);
}
static void bluetooth_action(lv_event_t *e){
    remote_command_t cmd=(remote_command_t)(intptr_t)lv_event_get_user_data(e);
    remote_status_t s;remote_get_status(&s);
    if(cmd==REMOTE_SCAN&&(s.phase==REMOTE_READY||s.phase==REMOTE_PAIRING||s.phase==REMOTE_DISCOVERING||s.phase==REMOTE_CONNECTING)){
        inform("请先断开当前遥控器，再搜索设备");return;
    }
    if(cmd==REMOTE_FORGET){
        uint32_t now=lv_tick_get();
        if(!forget_until||(int32_t)(forget_until-now)<=0){forget_until=now+5000;lv_label_set_text(lv_obj_get_child(bt_forget,0),"确认移除");return;}
        forget_until=0;lv_label_set_text(lv_obj_get_child(bt_forget,0),"移除配对");
    }
    if(!remote_command(cmd,-1))inform("蓝牙正在初始化，请稍后再试");
}
static void bluetooth_connect(lv_event_t *e){
    if(!remote_command(REMOTE_CONNECT,(int)(intptr_t)lv_event_get_user_data(e)))inform("设备列表已变化，请重新搜索");
}
static void bluetooth_refresh(lv_timer_t *timer){
    (void)timer;remote_status_t s;remote_get_status(&s);
    static bool last_usb;
    bool connected=usb_keyboard_connected();
    if(connected!=last_usb){lv_label_set_text(usb_state,connected?"电脑已连接":"电脑未接入");last_usb=connected;}
    const char *states[]={"正在初始化","未连接","正在搜索","正在连接","正在配对","正在读取按键","已连接","连接失败"};
    lv_label_set_text(bt_state,states[s.phase]);
    lv_label_set_text(lv_obj_get_child(bt_header,0),s.phase==REMOTE_READY?"蓝牙已连接":states[s.phase]);
    lv_label_set_text(bt_peer,s.peer[0]?s.peer:"尚未保存遥控器");
    char detail[160];snprintf(detail,sizeof(detail),"C6 蓝牙：%s\n按键：%04X   已收：%lu\n音频包：%lu",s.driver_ready?"就绪":"初始化中",s.buttons,(unsigned long)s.button_reports,(unsigned long)s.audio_packets);
    lv_label_set_text(bt_detail,detail);
    lv_label_set_text(bt_scan_note,s.count?"点选设备连接并配对":s.phase==REMOTE_SCANNING?"搜索中，请让遥控器进入配对模式":"按住遥控器返回键与音量加键配对");
    for(int i=0;i<REMOTE_CANDIDATES;i++){
        if(i<s.count){char t[96];snprintf(t,sizeof(t),"%s  %d dBm",s.candidates[i].address,s.candidates[i].rssi);
            lv_label_set_text(lv_obj_get_child(bt_candidates[i],0),t);lv_obj_remove_flag(bt_candidates[i],LV_OBJ_FLAG_HIDDEN);}
        else lv_obj_add_flag(bt_candidates[i],LV_OBJ_FLAG_HIDDEN);
    }
    static int last_error;
    static remote_phase_t last_phase=REMOTE_STARTING;
    if(s.error&&s.error!=last_error){snprintf(detail,sizeof(detail),"蓝牙状态码：%d；可重新连接或搜索配对",s.error);inform(detail);}
    else if(!s.error&&s.phase!=last_phase){
        if(s.phase==REMOTE_READY)inform(connected?"遥控器与电脑键盘已连接；麦克风待接入":"遥控器已连接；请检查电脑 USB OTG 连接");
        else if(s.phase==REMOTE_CONNECTING)inform("正在连接遥控器");
        else if(s.phase==REMOTE_SCANNING)inform("正在搜索，请让遥控器进入配对模式");
        else if(s.phase==REMOTE_IDLE)inform("遥控器未连接，可搜索或重新连接");
    }
    last_error=s.error;last_phase=s.phase;
    if(forget_until&&(int32_t)(forget_until-lv_tick_get())<=0){forget_until=0;lv_label_set_text(lv_obj_get_child(bt_forget,0),"移除配对");}
}

static bool valid_entry(const map_entry_t *e) {
    if(e->type==MAP_NONE)return true;
    if(e->type==MAP_VOICE)return e->option<=1;
    if(e->type!=MAP_KEY || (e->mods&~15))return false;
    for(size_t i=0;i<KEY_COUNT;i++)if(keys[i]==e->key)return true;
    return false;
}
static void mapping_defaults(void) {
    memset(&mapping,0,sizeof(mapping));mapping.magic=0x53355031;
    mapping.entry[REM_LEFT]=(map_entry_t){MAP_KEY,0x0a,MOD_CTRL|MOD_OPT|MOD_CMD,0};
    mapping.entry[REM_RIGHT]=(map_entry_t){MAP_KEY,0x06,MOD_CTRL|MOD_OPT|MOD_CMD,0};
    mapping.entry[REM_SELECT]=(map_entry_t){MAP_KEY,0x28,0,0};
    mapping.entry[REM_PLAY]=(map_entry_t){MAP_KEY,0x2c,0,0};
    mapping.entry[REM_MINUS]=(map_entry_t){MAP_KEY,0x50,0,0};
    mapping.entry[REM_PLUS]=(map_entry_t){MAP_KEY,0x4f,0,0};
    mapping.entry[REM_TV]=(map_entry_t){MAP_KEY,0x2c,MOD_CMD,0};
    mapping.entry[REM_VOICE]=(map_entry_t){MAP_VOICE,0,0,1};
}
static void mapping_load(void) {
    mapping_defaults();
    esp_err_t error=nvs_flash_init();
    if(error!=ESP_OK){ESP_LOGW(TAG,"NVS unavailable: %s",esp_err_to_name(error));return;}
    nvs_ready=true;nvs_handle_t h;
    if(nvs_open("siri_pad",NVS_READONLY,&h)!=ESP_OK)return;
    mapping_t saved;size_t length=sizeof(saved);
    if(nvs_get_blob(h,"map_v1",&saved,&length)==ESP_OK&&length==sizeof(saved)&&saved.magic==mapping.magic){
        bool valid=true;for(int i=0;i<REM_COUNT;i++)if(!valid_entry(&saved.entry[i]))valid=false;
        if(valid)mapping=saved;
    }
    nvs_close(h);
}
static bool mapping_save(void) {
    if(!nvs_ready)return false;
    nvs_handle_t h;if(nvs_open("siri_pad",NVS_READWRITE,&h)!=ESP_OK)return false;
    esp_err_t e=nvs_set_blob(h,"map_v1",&mapping,sizeof(mapping));
    if(e==ESP_OK)e=nvs_commit(h);
    nvs_close(h);
    return e==ESP_OK;
}
static void format_action(const map_entry_t *e,char *out,size_t size) {
    if(e->type==MAP_NONE){snprintf(out,size,"未设置");return;}
    if(e->type==MAP_VOICE){snprintf(out,size,"%sOption + 遥控器麦克风",e->option?"右":"左");return;}
    const char *key="?";for(size_t i=0;i<KEY_COUNT;i++)if(keys[i]==e->key)key=key_names[i];
    snprintf(out,size,"%s%s%s%s%s",(e->mods&MOD_CTRL)?"Ctrl+":"",(e->mods&MOD_OPT)?"Opt+":"",
        (e->mods&MOD_CMD)?"Cmd+":"",(e->mods&MOD_SHIFT)?"Shift+":"",key);
}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius) {
    lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,radius,0);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;
}
static lv_obj_t *label(lv_obj_t *parent,const char *value,int x,int y,int w,uint32_t color) {
    lv_obj_t *o=lv_label_create(parent);lv_label_set_text(o,value);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,&font_cn28,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent,const char *value,int x,int y,int w,int h,uint32_t color,uint32_t text_color,lv_event_cb_t cb,void *arg) {
    lv_obj_t *o=box(parent,x,y,w,h,color,15);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o,lv_color_hex(color==BLUE?0x1945c9:0xdce6ff),LV_STATE_PRESSED);
    lv_obj_t *text=label(o,value,0,0,w,text_color);lv_obj_align(text,LV_ALIGN_CENTER,0,0);
    lv_obj_set_style_text_align(text,LV_TEXT_ALIGN_CENTER,0);
    if(cb)lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,arg);
    return o;
}
static void inform(const char *message){lv_label_set_text(notice,message);ESP_LOGI(TAG,"UI: %s",message);}
static void update_home_notes(void){char s[96];format_action(&mapping.entry[REM_LEFT],s,sizeof(s));lv_label_set_text(home_left_note,s);
    format_action(&mapping.entry[REM_RIGHT],s,sizeof(s));lv_label_set_text(home_right_note,s);
    format_action(&mapping.entry[REM_TV],s,sizeof(s));lv_label_set_text(home_tv_note,s);}
static void show_home(lv_event_t *e){(void)e;lv_obj_remove_flag(home,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(settings,LV_OBJ_FLAG_HIDDEN);}
static void select_tab(bool bluetooth){
    if(bluetooth){lv_obj_remove_flag(bt_page,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(map_page,LV_OBJ_FLAG_HIDDEN);}
    else{lv_obj_add_flag(bt_page,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(map_page,LV_OBJ_FLAG_HIDDEN);}
    lv_obj_set_style_bg_color(tab_bt,lv_color_hex(bluetooth?PALE:WHITE),0);
    lv_obj_set_style_bg_color(tab_map,lv_color_hex(bluetooth?WHITE:PALE),0);
}
static void show_bt(lv_event_t *e){(void)e;lv_obj_add_flag(home,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(settings,LV_OBJ_FLAG_HIDDEN);select_tab(true);}
static void show_mapping(lv_event_t *e){(void)e;lv_obj_add_flag(home,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(settings,LV_OBJ_FLAG_HIDDEN);select_tab(false);}
static void usb_unavailable(lv_event_t *e){(void)e;inform("USB 麦克风尚未接入");}
static void touch_key(lv_event_t *e){
    uint8_t key=(uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if(!usb_keyboard_send(0,key))inform("电脑键盘未连接，请使用 USB OTG 口");
}
static void touch_mapping(lv_event_t *e){
    int index=(int)(intptr_t)lv_event_get_user_data(e);
    if(!usb_keyboard_connected()){inform("电脑键盘未连接，请使用 USB OTG 口");return;}
    send_entry(&mapping.entry[index]);
}
static void editor_refresh(void);
static void editor_close(lv_event_t *e){(void)e;if(editor){lv_obj_delete_async(editor);editor=NULL;}editing_index=-1;}
static void editor_type_cycle(lv_event_t *e){(void)e;draft.type=(draft.type+1)%3;if(draft.type==MAP_KEY&&!draft.key)draft.key=0x2c;editor_refresh();}
static void editor_mod_toggle(lv_event_t *e){int bit=(int)(intptr_t)lv_event_get_user_data(e);draft.mods^=bit;editor_refresh();}
static void editor_key_cycle(lv_event_t *e){int dir=(int)(intptr_t)lv_event_get_user_data(e);size_t index=0;
    for(size_t i=0;i<KEY_COUNT;i++)if(keys[i]==draft.key){index=i;break;}
    index=(index+KEY_COUNT+dir)%KEY_COUNT;draft.key=keys[index];editor_refresh();}
static void editor_option_toggle(lv_event_t *e){(void)e;draft.option=!draft.option;editor_refresh();}
static void mapping_render(void);
static void editor_save(lv_event_t *e){(void)e;if(editing_index<0||!valid_entry(&draft))return;
    mapping.entry[editing_index]=draft;
    bool saved=mapping_save();mapping_render();update_home_notes();editor_close(NULL);
    inform(saved?"按键映射已保存到设备":"映射已更改，但保存到设备失败");}
static void editor_refresh(void){
    if(!editor)return;
    lv_obj_t *text=lv_obj_get_child(editor_type,0);
    lv_label_set_text(text,draft.type==MAP_KEY?"键盘按键 / 快捷键":draft.type==MAP_VOICE?"遥控器语音":"不执行动作");
    if(draft.type==MAP_KEY)lv_obj_remove_flag(keyboard_group,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(keyboard_group,LV_OBJ_FLAG_HIDDEN);
    if(draft.type==MAP_VOICE)lv_obj_remove_flag(editor_option,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(editor_option,LV_OBJ_FLAG_HIDDEN);
    const char *mods[]={"Ctrl","Option","Command","Shift"};
    for(int i=0;i<4;i++){
        lv_obj_set_style_bg_color(editor_mods[i],lv_color_hex((draft.mods&(1<<i))?PALE:0xf1f3f6),0);
        lv_obj_set_style_border_width(editor_mods[i],(draft.mods&(1<<i))?2:0,0);
        lv_obj_set_style_border_color(editor_mods[i],lv_color_hex(BLUE),0);
        lv_label_set_text(lv_obj_get_child(editor_mods[i],0),mods[i]);
    }
    char action[96];format_action(&draft,action,sizeof(action));lv_label_set_text(editor_preview,action);
    lv_label_set_text(lv_obj_get_child(editor_key,0),action);
    lv_label_set_text(lv_obj_get_child(editor_option,0),draft.option?"右 Option":"左 Option");
}
static void editor_open(lv_event_t *e){
    editing_index=(int)(intptr_t)lv_event_get_user_data(e);draft=mapping.entry[editing_index];
    editor=box(root,0,0,1280,720,0x4d5a6b,0);lv_obj_add_flag(editor,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(editor,LV_OPA_70,0);
    lv_obj_t *modal=box(editor,305,70,670,580,WHITE,25);
    char title[70];snprintf(title,sizeof(title),"编辑 %s",button_names[editing_index]);label(modal,title,31,25,560,DARK);
    button(modal,"关闭",555,20,88,54,0xf1f3f6,DARK,editor_close,NULL);
    label(modal,"按下后执行",31,100,550,MUTED);
    editor_type=button(modal,"",31,145,608,60,0xf1f4f8,DARK,editor_type_cycle,NULL);
    keyboard_group=box(modal,0,213,670,230,WHITE,0);
    label(keyboard_group,"修饰键（可多选）",31,0,590,MUTED);
    const char *mod_text[]={"Ctrl","Option","Command","Shift"};
    for(int i=0;i<4;i++)editor_mods[i]=button(keyboard_group,mod_text[i],31+i*155,46,145,55,0xf1f3f6,BLUE,editor_mod_toggle,(void*)(intptr_t)(1<<i));
    label(keyboard_group,"主键",31,120,500,MUTED);
    button(keyboard_group,"<",31,162,70,56,PALE,BLUE,editor_key_cycle,(void*)(intptr_t)-1);
    editor_key=button(keyboard_group,"",110,162,450,56,0xf1f4f8,DARK,NULL,NULL);
    button(keyboard_group,">",569,162,70,56,PALE,BLUE,editor_key_cycle,(void*)(intptr_t)1);
    editor_option=button(modal,"",31,260,608,65,PALE,BLUE,editor_option_toggle,NULL);
    label(modal,"当前设置",31,455,160,MUTED);editor_preview=label(modal,"",170,455,468,BLUE);
    button(modal,"取消",350,505,135,60,PALE,BLUE,editor_close,NULL);
    button(modal,"保存映射",495,505,145,60,BLUE,WHITE,editor_save,NULL);
    editor_refresh();
}
static void mapping_render(void){
    lv_obj_clean(mapping_list);
    for(int i=0;i<REM_COUNT;i++){
        int y=i*72;lv_obj_t *row=box(mapping_list,0,y,855,68,(i%2)?0xf8f9fb:WHITE,0);
        label(row,button_names[i],15,17,190,DARK);
        char action[96];format_action(&mapping.entry[i],action,sizeof(action));label(row,action,220,17,465,BLUE);
        button(row,"编辑",715,9,126,50,PALE,BLUE,editor_open,(void*)(intptr_t)i);
    }
}
static void reset_mapping(lv_event_t *e){(void)e;if(!reset_armed){reset_armed=true;lv_label_set_text(lv_obj_get_child(reset_button,0),"再次点按确认");return;}
    reset_armed=false;lv_label_set_text(lv_obj_get_child(reset_button,0),"恢复默认");
    mapping_defaults();bool saved=mapping_save();mapping_render();update_home_notes();inform(saved?"已恢复默认映射":"恢复默认，但写入设备失败");}
static void create_ui(void){
    root=lv_screen_active();lv_obj_set_style_bg_color(root,lv_color_hex(BG),0);lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    box(root,0,0,1280,86,WHITE,0);label(root,"语音输入",37,27,250,DARK);
    box(root,706,16,185,54,0xf1f3f6,14);usb_state=label(root,"电脑未接入",727,29,165,MUTED);
    bt_header=button(root,"蓝牙初始化",904,16,205,54,0xf1f3f6,MUTED,show_bt,NULL);
    button(root,"设置",1122,16,124,54,PALE,BLUE,show_bt,NULL);
    home=box(root,0,86,1280,572,BG,0);settings=box(root,0,86,1280,572,BG,0);
    // Home: application shortcuts, microphone, and large keyboard keys.
    lv_obj_t *apps=box(home,33,20,278,532,WHITE,23);label(apps,"应用",24,22,220,MUTED);
    button(apps,"ChatGPT",20,80,238,103,0xecf7f3,DARK,touch_mapping,(void*)(intptr_t)REM_LEFT);
    home_left_note=label(apps,"",22,193,235,MUTED);
    button(apps,"Claude",20,247,238,103,0xf8efe9,DARK,touch_mapping,(void*)(intptr_t)REM_RIGHT);
    home_right_note=label(apps,"",22,358,235,MUTED);
    button(apps,"输入法切换",20,431,238,75,PALE,BLUE,touch_mapping,(void*)(intptr_t)REM_TV);
    home_tv_note=label(apps,"",22,506,235,MUTED);
    lv_obj_t *mic=box(home,330,20,522,532,WHITE,23);label(mic,"麦克风",26,22,250,MUTED);
    button(mic,"麦克风",150,126,220,220,0xe6edff,BLUE,usb_unavailable,NULL);
    label(mic,"等待音频功能",147,369,300,DARK);label(mic,"遥控器与板载麦克风待移植",64,417,430,MUTED);
    lv_obj_t *control=box(home,870,20,377,532,WHITE,23);label(control,"键盘",23,22,300,MUTED);
    button(control,"<",20,78,163,110,0xf1f4f8,DARK,touch_key,(void*)(uintptr_t)0x50);
    button(control,">",194,78,163,110,0xf1f4f8,DARK,touch_key,(void*)(uintptr_t)0x4f);
    button(control,"回车",20,204,337,130,BLUE,WHITE,touch_key,(void*)(uintptr_t)0x28);
    button(control,"空格",20,350,337,78,0xf1f4f8,DARK,touch_key,(void*)(uintptr_t)0x2c);
    label(control,"USB 键盘按键",20,454,350,MUTED);
    // Settings: independent Bluetooth and mapping pages.
    lv_obj_t *nav=box(settings,33,20,246,532,WHITE,23);label(nav,"设置",25,27,190,DARK);
    tab_bt=button(nav,"蓝牙连接",20,105,206,70,PALE,BLUE,show_bt,NULL);
    tab_map=button(nav,"按键映射",20,189,206,70,WHITE,DARK,show_mapping,NULL);
    button(nav,"返回主页",20,325,206,65,0xf1f3f6,DARK,show_home,NULL);
    lv_obj_t *content=box(settings,298,20,949,532,WHITE,23);
    bt_page=box(content,0,0,949,532,WHITE,23);label(bt_page,"蓝牙连接",30,25,650,DARK);
    label(bt_page,"管理 Apple TV 遥控器配对与重连",30,71,800,MUTED);
    lv_obj_t *device=box(bt_page,30,125,420,333,0xf8f9fb,18);
    label(device,"当前遥控器",22,16,360,MUTED);bt_state=label(device,"正在初始化",22,59,350,DARK);
    bt_peer=label(device,"",22,104,385,MUTED);bt_detail=label(device,"",22,148,390,MUTED);
    button(device,"重连",15,269,118,50,PALE,BLUE,bluetooth_action,(void*)(intptr_t)REMOTE_RECONNECT);
    button(device,"断开",143,269,118,50,PALE,BLUE,bluetooth_action,(void*)(intptr_t)REMOTE_DISCONNECT);
    bt_forget=button(device,"移除配对",271,269,134,50,0xffeeec,0xbb5b48,bluetooth_action,(void*)(intptr_t)REMOTE_FORGET);
    lv_obj_t *discover=box(bt_page,470,125,450,333,0xf8f9fb,18);
    label(discover,"附近的遥控器",22,16,235,DARK);
    button(discover,"搜索",316,10,114,50,PALE,BLUE,bluetooth_action,(void*)(intptr_t)REMOTE_SCAN);
    bt_scan_note=label(discover,"",22,72,410,MUTED);
    for(int i=0;i<REMOTE_CANDIDATES;i++)bt_candidates[i]=button(discover,"",15,125+i*50,420,45,PALE,BLUE,bluetooth_connect,(void*)(intptr_t)i);
    label(bt_page,"遥控器经蓝牙连接；电脑键盘经 USB OTG 连接。",30,478,890,MUTED);
    map_page=box(content,0,0,949,532,WHITE,23);label(map_page,"按键映射",30,25,500,DARK);
    label(map_page,"点按编辑，配置快捷键；保存到设备 NVS",30,71,850,MUTED);
    reset_button=button(map_page,"恢复默认",700,22,216,62,PALE,BLUE,reset_mapping,NULL);
    mapping_list=box(map_page,28,125,892,380,WHITE,15);lv_obj_add_flag(mapping_list,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(mapping_list,LV_DIR_VER);mapping_render();
    lv_obj_add_flag(settings,LV_OBJ_FLAG_HIDDEN);select_tab(true);update_home_notes();
    box(root,0,658,1280,62,WHITE,0);notice=label(root,"电脑请连接 USB OTG；USB 麦克风待接入",38,675,1150,MUTED);
}
void app_main(void){
    mapping_load();
    usb_keyboard_start();
    bsp_display_cfg_t cfg={
        .lv_adapter_cfg=ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation=ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode=ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags={.swap_xy=1,.mirror_x=1,.mirror_y=0},
    };
    lv_display_t *display=bsp_display_start_with_config(&cfg);assert(display);
    bsp_display_backlight_on();
    bsp_display_lock(-1);
    int width=lv_display_get_horizontal_resolution(display),height=lv_display_get_vertical_resolution(display);
    ESP_LOGI(TAG,"Display resolution %d x %d; NVS %s",width,height,nvs_ready?"ready":"unavailable");
    if(width==1280&&height==720)create_ui();
    else{lv_obj_t *error=lv_label_create(lv_screen_active());lv_label_set_text(error,"Display rotation error");lv_obj_center(error);}
    if(width==1280&&height==720)lv_timer_create(bluetooth_refresh,250,NULL);
    bsp_display_unlock();
    remote_start();
}
