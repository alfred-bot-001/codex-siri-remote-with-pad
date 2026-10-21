#include "pad.h"
#include "app_icons.h"
#include <initializer_list>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_3inch5_lcd_port.h"
#include "esp_io_expander_tca9554.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "esp_system.h"
#include "esp_lcd_panel_ops.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "nvs.h"
LV_FONT_DECLARE(font_port17);
static constexpr uint32_t BG=0xefeee9,WHITE=0xf8f7f3,ORANGE=0xdc531f,DARK=0x272824,MUTED=0x64665f,PALE=0xe4e4dc;
static lv_obj_t *root,*home,*settings,*pages[3],*tabs[3],*notice,*status_bt,*status_usb,*mic,*mic_status,*mic_body,*mic_stem,*mic_base,*bt_phase,*bt_identity,*candidates[4],*map_list,*timeout_label,*timeout_buttons[6],*footer_timeout,*editor,*editor_type,*editor_key,*editor_option,*editor_mods[4],*editor_preview;
static lv_obj_t *apps[2],*quota_labels[2],*quota_bars[2],*sleep_overlay;
static std::atomic<int> weekly_left[2]{{-1},{-1}};
static std::atomic<uint32_t> quota_received_ms{0};
static esp_lcd_panel_handle_t panel;
static esp_io_expander_handle_t expander;
static i2c_master_dev_handle_t pmic;
static std::atomic<uint32_t> ui_ticks{0},ui_heartbeat_ms{0};
static std::atomic<int> ui_tick_stage{0};
static uint32_t lv_free_after_create=0;
static RTC_NOINIT_ATTR uint32_t ui_stall_marker;
static uint16_t timeout_minutes=30;
static uint32_t last_activity_ms=0,last_remote_input=0;
static uint32_t notice_until=0;
static bool screen_sleeping=false;
static std::atomic<bool> ui_constructing{false};
static int edit_index=-1;
static pad_map_entry_t draft{};
static const uint16_t timeout_values[]={1,5,15,30,60,0};
static const char *timeout_names[]={"1分钟","5分钟","15分钟","30分钟","60分钟","永不"};
static const char *button_names[]={"圆环左","圆环右","中央确认","播放暂停","减号","加号","小电视","语音键"};
static const uint8_t key_codes[]={0x28,0x2c,0x50,0x4f,0x52,0x51,0x2a,0x29,0x2b,
 0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,
 0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d};
static const char *key_names[]={"回车","空格","左","右","上","下","Backspace","Esc","Tab",
 "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"};
extern "C" void pad_quota_update(int codex_weekly_left,int claude_weekly_left){
 if(codex_weekly_left< -1||codex_weekly_left>100||claude_weekly_left< -1||claude_weekly_left>100)return;
 weekly_left[0]=codex_weekly_left;weekly_left[1]=claude_weekly_left;
 quota_received_ms=pad_millis();
}
static void ui_watchdog(void*){
 for(;;){vTaskDelay(pdMS_TO_TICKS(1000));uint32_t age=pad_millis()-ui_heartbeat_ms.load();
  if(age>10000){ESP_LOGE("display","UI stalled %lu ms",(unsigned long)age);
   ui_stall_marker=0x55495354;pad_cancel_voice();vTaskDelay(pdMS_TO_TICKS(100));esp_restart();}
 }
}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color,int radius){
 lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
 lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
 lv_obj_set_style_radius(o,radius,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE);
 if(ui_constructing)vTaskDelay(1);
 return o;
}
static lv_obj_t *label(lv_obj_t *parent,const char *value,int x,int y,int w,uint32_t color,const lv_font_t *font=&font_port17){
 lv_obj_t *o=lv_label_create(parent);lv_label_set_text(o,value);lv_obj_set_pos(o,x,y);
 lv_obj_set_width(o,w);lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
 lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);
 if(ui_constructing)vTaskDelay(1);
 return o;
}
static lv_obj_t *button(lv_obj_t *parent,const char *value,int x,int y,int w,int h,uint32_t fill,uint32_t ink,lv_event_cb_t cb=nullptr,void *arg=nullptr){
 lv_obj_t *o=box(parent,x,y,w,h,fill,10);lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0xc8c9c2),0);
 lv_obj_set_style_shadow_width(o,7,0);lv_obj_set_style_shadow_ofs_y(o,3,0);
 lv_obj_set_style_shadow_opa(o,LV_OPA_20,0);lv_obj_set_style_shadow_color(o,lv_color_hex(DARK),0);
 lv_obj_set_style_bg_color(o,lv_color_hex(fill==ORANGE?0xa83d1b:0xd2d2ca),LV_STATE_PRESSED);
 const lv_font_t *font=(strcmp(value,LV_SYMBOL_LEFT)==0||strcmp(value,LV_SYMBOL_RIGHT)==0)
  ?&lv_font_montserrat_28:&font_port17;
 lv_obj_t *t=label(o,value,4,0,w-8,ink,font);lv_obj_align(t,LV_ALIGN_CENTER,0,0);
 if(cb)lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,arg);
 return o;
}
static void activity(){last_activity_ms=pad_millis();if(screen_sleeping){
 esp_3inch5_brightness_port_set(80);screen_sleeping=false;lv_obj_add_flag(sleep_overlay,LV_OBJ_FLAG_HIDDEN);}}
static void show_notice(const char *s){lv_label_set_text(lv_obj_get_child(notice,0),s);lv_obj_clear_flag(notice,LV_OBJ_FLAG_HIDDEN);lv_obj_move_foreground(notice);notice_until=pad_millis()+2500;}
static void key_event(lv_event_t *e){
 uint8_t key=(uintptr_t)lv_event_get_user_data(e);lv_event_code_t c=lv_event_get_code(e);
 if(c==LV_EVENT_PRESSED){activity();pad_touch_key(key,true);}
 if(c==LV_EVENT_RELEASED||c==LV_EVENT_PRESS_LOST)pad_touch_key(key,false);
}
static void app_event(lv_event_t *e){activity();pad_shortcut((pad_action_t)(uintptr_t)lv_event_get_user_data(e));}
static void chrome_event(lv_event_t *e){(void)e;activity();pad_send_chord(0x0d,0x05);}
static void mic_event(lv_event_t *e){(void)e;activity();pad_toggle_board_mic();}
static void home_event(lv_event_t *e){(void)e;activity();lv_obj_add_flag(settings,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(home,LV_OBJ_FLAG_HIDDEN);}
static void page_select(int page){for(int i=0;i<3;i++){
 if(i==page)lv_obj_clear_flag(pages[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(pages[i],LV_OBJ_FLAG_HIDDEN);
 lv_obj_set_style_bg_color(tabs[i],lv_color_hex(i==page?ORANGE:PALE),0);
 lv_obj_set_style_text_color(lv_obj_get_child(tabs[i],0),lv_color_hex(i==page?WHITE:DARK),0);
}}
static void settings_event(lv_event_t *e){
 activity();int page=(intptr_t)lv_event_get_user_data(e);
 lv_obj_add_flag(home,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(settings,LV_OBJ_FLAG_HIDDEN);page_select(page);
}
static void ble_event(lv_event_t *e){activity();int cmd=(intptr_t)lv_event_get_user_data(e);
 if(cmd==0)pad_ble_scan();else if(cmd==1)pad_ble_reconnect();else if(cmd==2)pad_ble_disconnect();else pad_ble_forget();}
static void candidate_event(lv_event_t *e){activity();pad_ble_connect_candidate((uintptr_t)lv_event_get_user_data(e));}
static void timeout_refresh(){
 for(int i=0;i<6;i++)lv_obj_set_style_bg_color(timeout_buttons[i],lv_color_hex(timeout_minutes==timeout_values[i]?ORANGE:WHITE),0);
 char s[32];if(timeout_minutes)snprintf(s,sizeof(s),"%u分钟",(unsigned)timeout_minutes);else snprintf(s,sizeof(s),"永不");
 lv_label_set_text(timeout_label,s);
 if(timeout_minutes)snprintf(s,sizeof(s),"%u分钟自动息屏",(unsigned)timeout_minutes);
 lv_label_set_text(footer_timeout,s);
}
static void timeout_event(lv_event_t *e){
 activity();timeout_minutes=(uintptr_t)lv_event_get_user_data(e);timeout_refresh();
 nvs_handle_t h;if(nvs_open("siri_pad",NVS_READWRITE,&h)==ESP_OK){nvs_set_u16(h,"screen_min",timeout_minutes);nvs_commit(h);nvs_close(h);}
}
static void action_text(const pad_map_entry_t &m,char *out,size_t size){
 if(m.type==PAD_MAP_NONE){snprintf(out,size,"无");return;}
 if(m.type==PAD_MAP_VOICE){snprintf(out,size,"%s Option",m.right_option?"右":"左");return;}
 const char *key="?";for(unsigned i=0;i<sizeof(key_codes);i++)if(key_codes[i]==m.key){key=key_names[i];break;}
 snprintf(out,size,"%s%s%s%s%s",(m.modifiers&1)?"Ctrl+":"",(m.modifiers&4)?"Opt+":"",
  (m.modifiers&8)?"Cmd+":"",(m.modifiers&2)?"Shift+":"",key);
}
static void editor_refresh();
static void map_render(){
 lv_obj_clean(map_list);
 for(int i=0;i<PAD_BUTTON_COUNT;i++){
  pad_map_entry_t m;pad_mapping_get((pad_button_t)i,&m);char action[80];action_text(m,action,sizeof(action));
  lv_obj_t *row=button(map_list,"",(i%2)*232,(i/2)*43,224,39,WHITE,DARK,nullptr,nullptr);
  label(row,button_names[i],4,9,76,DARK);
  label(row,action,82,9,134,ORANGE);
  lv_obj_add_event_cb(row,[](lv_event_t *e){
   activity();edit_index=(intptr_t)lv_event_get_user_data(e);
   pad_mapping_get((pad_button_t)edit_index,&draft);editor_refresh();lv_obj_clear_flag(editor,LV_OBJ_FLAG_HIDDEN);
  },LV_EVENT_CLICKED,(void*)(intptr_t)i);
 }
}
static void editor_refresh(){
 if(edit_index<0)return;
 lv_label_set_text(lv_obj_get_child(editor_type,0),draft.type==PAD_MAP_NONE?"无":draft.type==PAD_MAP_VOICE?"语音":"按键");
 const char *key="?";for(unsigned i=0;i<sizeof(key_codes);i++)if(key_codes[i]==draft.key){key=key_names[i];break;}
 lv_label_set_text(lv_obj_get_child(editor_key,0),key);
 lv_label_set_text(lv_obj_get_child(editor_option,0),draft.right_option?"右 Option":"左 Option");
 for(int i=0;i<4;i++)lv_obj_set_style_bg_color(editor_mods[i],lv_color_hex((draft.modifiers&(1<<i))?ORANGE:PALE),0);
 char text[96];action_text(draft,text,sizeof(text));lv_label_set_text(editor_preview,text);
 lv_obj_set_style_opa(editor_key,draft.type==PAD_MAP_KEY?LV_OPA_COVER:LV_OPA_50,0);
 lv_obj_set_style_opa(editor_option,draft.type==PAD_MAP_VOICE?LV_OPA_COVER:LV_OPA_50,0);
}
static void editor_type_event(lv_event_t *e){(void)e;activity();draft.type=(draft.type+1)%(edit_index==PAD_VOICE?3:2);
 if(draft.type==PAD_MAP_KEY&&!draft.key)draft.key=0x2c;
 editor_refresh();}
static void editor_key_event(lv_event_t *e){
 activity();if(draft.type!=PAD_MAP_KEY)return;int step=(intptr_t)lv_event_get_user_data(e);unsigned index=0;
 for(unsigned i=0;i<sizeof(key_codes);i++)if(key_codes[i]==draft.key){index=i;break;}
 draft.key=key_codes[(index+sizeof(key_codes)+step)%sizeof(key_codes)];editor_refresh();
}
static void editor_mod_event(lv_event_t *e){activity();draft.modifiers^=(uintptr_t)lv_event_get_user_data(e);editor_refresh();}
static void editor_option_event(lv_event_t *e){(void)e;activity();draft.right_option=!draft.right_option;editor_refresh();}
static void editor_close_event(lv_event_t *e){(void)e;activity();lv_obj_add_flag(editor,LV_OBJ_FLAG_HIDDEN);edit_index=-1;}
static void editor_save_event(lv_event_t *e){(void)e;activity();
 if(edit_index>=0){bool saved=pad_mapping_set((pad_button_t)edit_index,&draft);map_render();show_notice(saved?"已保存":"保存失败");}
 editor_close_event(nullptr);
}
static void reset_event(lv_event_t *e){(void)e;activity();pad_mapping_reset();map_render();show_notice("默认按键已恢复");}
static void screen_wake_event(lv_event_t *e){(void)e;activity();}
static void tick(lv_timer_t*){
 ui_ticks++;ui_heartbeat_ms=pad_millis();
 ui_tick_stage=1;
 if(notice_until&&(int32_t)(pad_millis()-notice_until)>=0){notice_until=0;lv_obj_add_flag(notice,LV_OBJ_FLAG_HIDDEN);}
 pad_status_t s;pad_status(&s);pad_ble_status_t b;pad_ble_status(&b);
 ui_tick_stage=2;
 if(s.last_input_ms!=last_remote_input){last_remote_input=s.last_input_ms;activity();}
 static int last_phase=-1,last_ble=-1,last_usb=-1,last_voice=-1,last_source=-1;
 if(last_phase!=b.phase||last_ble!=s.ble){last_phase=b.phase;last_ble=s.ble;activity();
  const char *states[]={"初始化","未连接","搜索中","连接中","已连接","连接失败"};
  const char *header_states[]={"蓝牙初始化","遥控器未连接","遥控器搜索中","遥控器连接中","遥控器已连接","遥控器连接失败"};
  lv_label_set_text(status_bt,header_states[b.phase]);lv_label_set_text(bt_phase,states[b.phase]);
 }
 ui_tick_stage=3;
 if(last_usb!=s.usb){last_usb=s.usb;activity();lv_label_set_text(status_usb,s.usb?"USB 已连接":"USB 未连接");}
 if(last_voice!=s.voice||last_source!=s.source){last_voice=s.voice;last_source=s.source;
  lv_obj_set_style_bg_color(mic,lv_color_hex(s.voice?0x30322d:ORANGE),0);
  lv_label_set_text(mic_status,s.source==MIC_REMOTE?"遥控器输入":s.source==MIC_BOARD?"板载输入":"语音待机");
 }
 static int last_quota[2]={-2,-2};
 uint32_t received=quota_received_ms.load();
 bool fresh=received&&(uint32_t)(pad_millis()-received)<15*60000U;
 for(int i=0;i<2;i++){
  int value=fresh?weekly_left[i].load():-1;
  if(value==last_quota[i])continue;
  last_quota[i]=value;
  char percent[16];if(value<0)snprintf(percent,sizeof(percent),"--");
  else snprintf(percent,sizeof(percent),"%d%%",value);
  lv_label_set_text(quota_labels[i],percent);
  lv_obj_set_width(quota_bars[i],value<0?0:205*value/100);
 }
 ui_tick_stage=4;
 static char last_identity[18]{};static uint8_t last_count=255;
 if(strcmp(last_identity,b.identity)||last_count!=b.count){
  snprintf(last_identity,sizeof(last_identity),"%s",b.identity);last_count=b.count;
  lv_label_set_text(bt_identity,b.identity[0]?b.identity:"未配对");
  for(int i=0;i<4;i++){
   if(i<b.count){char text[48];snprintf(text,sizeof(text),"%s  %d dBm",b.candidates[i].identity,b.candidates[i].rssi);
    lv_label_set_text(lv_obj_get_child(candidates[i],0),text);lv_obj_clear_flag(candidates[i],LV_OBJ_FLAG_HIDDEN);}
   else lv_obj_add_flag(candidates[i],LV_OBJ_FLAG_HIDDEN);
  }
 }
 ui_tick_stage=5;
 if(!screen_sleeping&&timeout_minutes&&s.source==MIC_OFF&&(uint32_t)(pad_millis()-last_activity_ms)>=uint32_t(timeout_minutes)*60000U){
  esp_3inch5_brightness_port_set(0);screen_sleeping=true;lv_obj_clear_flag(sleep_overlay,LV_OBJ_FLAG_HIDDEN);
 }
 ui_tick_stage=6;
}
static void create_ui(lv_obj_t *screen){
 ui_constructing=true;
 root=screen;lv_obj_set_style_bg_color(root,lv_color_hex(BG),0);lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);
 box(root,0,0,480,43,WHITE,0);
 label(root,"SIRI VOICE PAD",9,12,149,DARK,&lv_font_montserrat_14);
 status_bt=label(root,"遥控器未连接",158,11,131,MUTED);
 status_usb=label(root,"USB 未连接",291,11,117,MUTED);
 button(root,"设置",413,4,58,34,PALE,DARK,settings_event,(void*)0);
 home=box(root,0,43,480,277,BG,0);settings=box(root,0,43,480,277,BG,0);
 const char *app_names[]={"ChatGPT","Claude","Chrome"};
 for(int i=0;i<3;i++){
  int y=2+i*68;
  lv_obj_t *card=button(home,"",9,y,283,61,WHITE,DARK,
   i==2?chrome_event:app_event,i==2?nullptr:(void*)(uintptr_t)(i==0?PAD_ACTION_CHATGPT:PAD_ACTION_CLAUDE));
  if(i<2)apps[i]=card;
  lv_obj_t *icon=lv_img_create(card);lv_img_set_src(icon,i==0?&icon_openai:i==1?&icon_claude:&icon_googlechrome);
  lv_obj_set_pos(icon,11,16);
  label(card,app_names[i],48,6,129,DARK);
  if(i<2){
   label(card,i==0?"Codex 每周剩余":"Claude 每周剩余",48,28,155,MUTED);
   quota_labels[i]=label(card,"--",209,5,64,i==0?0x087c62:0xa55438);
   box(card,48,53,205,3,0xe0dfd7,2);
   quota_bars[i]=box(card,48,53,0,3,i==0?0x12a27d:0xca7852,2);
  }else label(card,"打开浏览器",48,31,150,MUTED);
 }
 lv_obj_t *voice_card=box(home,301,2,170,196,WHITE,12);
 lv_obj_set_style_border_width(voice_card,1,0);lv_obj_set_style_border_color(voice_card,lv_color_hex(0xd9d7cf),0);
 label(voice_card,"麦克风",12,11,146,MUTED);
 mic=box(voice_card,31,39,108,108,ORANGE,54);lv_obj_add_flag(mic,LV_OBJ_FLAG_CLICKABLE);
 lv_obj_add_event_cb(mic,mic_event,LV_EVENT_CLICKED,nullptr);
 mic_body=box(mic,46,24,16,37,WHITE,8);
 box(mic,32,55,4,17,WHITE,2);box(mic,72,55,4,17,WHITE,2);box(mic,35,70,38,4,WHITE,2);
 mic_stem=box(mic,52,74,4,13,WHITE,2);mic_base=box(mic,42,87,24,4,WHITE,2);
 mic_status=label(voice_card,"语音待机",5,154,160,DARK);
 lv_obj_t *left=button(home,LV_SYMBOL_LEFT,9,205,89,50,WHITE,DARK,nullptr,nullptr);
 lv_obj_t *space=button(home,"空格",105,205,92,50,WHITE,DARK,nullptr,nullptr);
 lv_obj_t *enter=button(home,"回车",204,205,163,50,0x30322d,WHITE,nullptr,nullptr);
 lv_obj_t *right=button(home,LV_SYMBOL_RIGHT,375,205,96,50,WHITE,DARK,nullptr,nullptr);
 lv_obj_t *arr[]={left,space,enter,right};uint8_t codes[]={0x50,0x2c,0x28,0x4f};
 for(int i=0;i<4;i++)lv_obj_add_event_cb(arr[i],key_event,LV_EVENT_ALL,(void*)(uintptr_t)codes[i]);
 box(home,0,258,480,1,0xcfd0c9,0);
 label(home,"SIRI VOICE PAD",12,261,190,MUTED,&lv_font_montserrat_14);
 footer_timeout=label(home,"30分钟自动息屏",286,259,184,MUTED);
 tabs[0]=button(settings,"蓝牙",9,2,103,35,ORANGE,WHITE,settings_event,(void*)0);
 tabs[1]=button(settings,"按键",118,2,103,35,PALE,DARK,settings_event,(void*)1);
 tabs[2]=button(settings,"息屏",227,2,103,35,PALE,DARK,settings_event,(void*)2);
 button(settings,"返回主页",348,2,122,35,WHITE,DARK,home_event,nullptr);
 for(int i=0;i<3;i++)pages[i]=box(settings,0,44,480,222,BG,0);
 label(pages[0],"遥控器蓝牙",12,5,195,DARK);
 bt_phase=label(pages[0],"初始化",260,5,205,ORANGE);
 bt_identity=label(pages[0],"",12,29,456,MUTED,&lv_font_montserrat_14);
 button(pages[0],"搜索",10,53,110,37,WHITE,DARK,ble_event,(void*)0);
 button(pages[0],"重连",128,53,110,37,WHITE,DARK,ble_event,(void*)1);
 button(pages[0],"断开",246,53,110,37,WHITE,DARK,ble_event,(void*)2);
 button(pages[0],"移除",364,53,110,37,WHITE,DARK,ble_event,(void*)3);
 for(int i=0;i<4;i++){
  candidates[i]=button(pages[0],"",12,98+i*30,456,27,WHITE,DARK,candidate_event,(void*)(uintptr_t)i);
  lv_obj_add_flag(candidates[i],LV_OBJ_FLAG_HIDDEN);
 }
 button(pages[1],"默认",402,2,69,32,WHITE,DARK,reset_event,nullptr);
 map_list=box(pages[1],8,39,464,177,BG,0);map_render();
 label(pages[2],"无交互后关闭背光",12,9,220,DARK);
 timeout_label=label(pages[2],"30分钟",263,9,205,ORANGE);
 for(int i=0;i<6;i++)
  timeout_buttons[i]=button(pages[2],timeout_names[i],12+(i%3)*156,51+(i/3)*76,144,60,WHITE,DARK,timeout_event,(void*)(uintptr_t)timeout_values[i]);
 timeout_refresh();lv_obj_add_flag(settings,LV_OBJ_FLAG_HIDDEN);page_select(0);
 notice=box(root,96,270,288,39,0x30322d,8);label(notice,"",3,7,282,WHITE);lv_obj_add_flag(notice,LV_OBJ_FLAG_HIDDEN);
 editor=box(root,0,0,480,320,BG,0);lv_obj_add_flag(editor,LV_OBJ_FLAG_HIDDEN);
 label(editor,"编辑按键",12,9,456,DARK);
 editor_type=button(editor,"按键",12,42,100,42,WHITE,DARK,editor_type_event,nullptr);
 button(editor,"<",122,42,50,42,WHITE,DARK,editor_key_event,(void*)-1);
 editor_key=button(editor,"空格",179,42,120,42,WHITE,DARK,nullptr,nullptr);
 button(editor,">",306,42,50,42,WHITE,DARK,editor_key_event,(void*)1);
 editor_option=button(editor,"右 Option",365,42,104,42,WHITE,DARK,editor_option_event,nullptr);
 label(editor,"修饰键",12,96,456,MUTED);
 const char *mods[]={"Ctrl","Shift","Opt","Cmd"};
 for(int i=0;i<4;i++)editor_mods[i]=button(editor,mods[i],12+i*116,122,106,44,PALE,DARK,editor_mod_event,(void*)(uintptr_t)(1<<i));
 editor_preview=label(editor,"",12,185,456,ORANGE);
 button(editor,"关闭",12,248,218,55,WHITE,DARK,editor_close_event,nullptr);
 button(editor,"保存",250,248,218,55,ORANGE,WHITE,editor_save_event,nullptr);
 sleep_overlay=box(root,0,0,480,320,BG,0);lv_obj_set_style_bg_opa(sleep_overlay,LV_OPA_TRANSP,0);
 lv_obj_add_flag(sleep_overlay,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(sleep_overlay,screen_wake_event,LV_EVENT_PRESSED,nullptr);
 lv_obj_add_flag(sleep_overlay,LV_OBJ_FLAG_HIDDEN);
 lv_timer_create(tick,100,nullptr);last_activity_ms=pad_millis();ui_heartbeat_ms=last_activity_ms;
 ui_constructing=false;
}

void pad_ui_init(){
 if(esp_reset_reason()==ESP_RST_SW && ui_stall_marker==0x55495354)
  ESP_LOGW("display","Recovered from a stalled UI task");
 ui_stall_marker=0;
 nvs_handle_t settings_nvs;
 if(nvs_open("siri_pad",NVS_READONLY,&settings_nvs)==ESP_OK){
  uint16_t saved=30;
  if(nvs_get_u16(settings_nvs,"screen_min",&saved)==ESP_OK)
   for(uint16_t allowed:timeout_values)if(saved==allowed){timeout_minutes=saved;break;}
  nvs_close(settings_nvs);
 }
 i2c_master_bus_config_t bc{};bc.i2c_port=I2C_NUM_0;bc.sda_io_num=GPIO_NUM_8;bc.scl_io_num=GPIO_NUM_7;
 bc.clk_source=I2C_CLK_SRC_DEFAULT;bc.glitch_ignore_cnt=7;bc.flags.enable_internal_pullup=true;
 i2c_master_bus_handle_t bus;ESP_ERROR_CHECK(i2c_new_master_bus(&bc,&bus));
 // Only enable the board's 3.3 V ALDO1 rail. Preserve charger and other rails.
 i2c_device_config_t pc{};pc.dev_addr_length=I2C_ADDR_BIT_LEN_7;pc.device_address=0x34;pc.scl_speed_hz=400000;
 ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&pc,&pmic));
 uint8_t reg=0x92,v=0; // AXP2101 ALDO1 voltage: 500 mV + N * 100 mV.
 if(i2c_master_transmit_receive(pmic,&reg,1,&v,1,100)==ESP_OK){uint8_t w[]={reg,uint8_t((v&0xe0)|28)};ESP_ERROR_CHECK(i2c_master_transmit(pmic,w,2,100));}
 reg=0x90;
 if(i2c_master_transmit_receive(pmic,&reg,1,&v,1,100)==ESP_OK){uint8_t w[]={reg,uint8_t(v|1)};ESP_ERROR_CHECK(i2c_master_transmit(pmic,w,2,100));}
 ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(bus,ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,&expander));
 ESP_ERROR_CHECK(esp_io_expander_set_dir(expander,IO_EXPANDER_PIN_NUM_1,IO_EXPANDER_OUTPUT));
 ESP_ERROR_CHECK(esp_io_expander_set_level(expander,IO_EXPANDER_PIN_NUM_1,0));vTaskDelay(pdMS_TO_TICKS(100));
 ESP_ERROR_CHECK(esp_io_expander_set_level(expander,IO_EXPANDER_PIN_NUM_1,1));vTaskDelay(pdMS_TO_TICKS(150));
 esp_lcd_panel_io_handle_t io;esp_lcd_touch_handle_t touch;
 esp_3inch5_display_port_init(&io,&panel,320*40*2);esp_3inch5_touch_port_init(&touch,bus,320,480,0);
 esp_3inch5_brightness_port_init();esp_3inch5_brightness_port_set(50);
 lvgl_port_cfg_t port=ESP_LVGL_PORT_INIT_CONFIG();port.task_affinity=1;ESP_ERROR_CHECK(lvgl_port_init(&port));
 lvgl_port_display_cfg_t dc{};dc.io_handle=io;dc.panel_handle=panel;dc.buffer_size=320*40;dc.double_buffer=true;dc.hres=320;dc.vres=480;dc.rotation.mirror_x=true;dc.flags.buff_spiram=true;dc.flags.sw_rotate=true;
 auto *display=lvgl_port_add_disp(&dc);assert(display);
 lvgl_port_touch_cfg_t tc{};tc.disp=display;tc.handle=touch;assert(lvgl_port_add_touch(&tc));
 lvgl_port_lock(0);
 // The panel is native 320x480. LVGL maps it to 480x320 and rotates raw
 // FT6336 touch coordinates together with the framebuffer.
 lv_disp_set_rotation(display,LV_DISP_ROT_270);
 auto *screen=lv_scr_act();create_ui(screen);
 lv_mem_monitor_t mem{};lv_mem_monitor(&mem);lv_free_after_create=mem.free_size;
 lvgl_port_unlock();pad_onboard_init(bus);
 // Keep the validated reset/redraw sequence for this panel.
 pad_display_recover();
 ui_heartbeat_ms=pad_millis();
 assert(xTaskCreatePinnedToCore(ui_watchdog,"ui_watchdog",3072,nullptr,3,nullptr,0)==pdPASS);
}

uint8_t *pad_ui_snapshot(size_t *size){
 pad_status_t s;pad_status(&s);if(s.voice)return nullptr;
 if(!lvgl_port_lock(100))return nullptr;
 *size=lv_snapshot_buf_size_needed(lv_scr_act(),LV_IMG_CF_TRUE_COLOR);
 uint8_t *copy=(uint8_t*)malloc(*size);lv_img_dsc_t img{};
 if(copy&&lv_snapshot_take_to_buf(lv_scr_act(),LV_IMG_CF_TRUE_COLOR,&img,copy,*size)!=LV_RES_OK){free(copy);copy=nullptr;}
 lvgl_port_unlock();return copy;
}

void pad_display_diagnostics(){
 ESP_LOGI("display","heartbeat_age=%lu tick_stage=%d lv_free=%lu heap_free=%lu internal_free=%lu",
 (unsigned long)(pad_millis()-ui_heartbeat_ms.load()),
 ui_tick_stage.load(),(unsigned long)lv_free_after_create,
 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
 uint32_t reset=0;uint8_t reg=0x90,rails=0;
 auto a=esp_io_expander_get_level(expander,IO_EXPANDER_PIN_NUM_1,&reset);
 auto b=i2c_master_transmit_receive(pmic,&reg,1,&rails,1,100);
 ESP_LOGI("display","uptime=%lu reset_reason=%d ticks=%lu backlight_duty=%lu freq=%lu gpio6_mux=%lx lcd_reset=%lu/%d rails=%02x/%d",
 (unsigned long)pad_millis(),esp_reset_reason(),(unsigned long)ui_ticks.load(),
 (unsigned long)ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0),(unsigned long)ledc_get_freq(LEDC_LOW_SPEED_MODE,LEDC_TIMER_1),
 (unsigned long)REG_READ(GPIO_FUNC6_OUT_SEL_CFG_REG),(unsigned long)reset,a,rails,b);
}
void pad_display_backlight(){esp_3inch5_brightness_port_set(80);pad_display_diagnostics();}
void pad_display_recover(){
 pad_cancel_voice();
 if(!lvgl_port_lock(1000)){ESP_LOGE("display","UI lock timeout");return;}
 ESP_ERROR_CHECK(esp_io_expander_set_level(expander,IO_EXPANDER_PIN_NUM_1,0));vTaskDelay(pdMS_TO_TICKS(20));
 ESP_ERROR_CHECK(esp_io_expander_set_level(expander,IO_EXPANDER_PIN_NUM_1,1));vTaskDelay(pdMS_TO_TICKS(150));
 ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
 ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel,true));ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
 lv_disp_set_rotation(nullptr,LV_DISP_ROT_270);
 lv_obj_invalidate(lv_scr_act());lv_refr_now(nullptr);lvgl_port_unlock();pad_display_backlight();
}
