#include "pad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_opus_dec.h"
#include "nvs.h"
#include <cstring>
#include <algorithm>
#include <cassert>

static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
struct Guard { Guard(){portENTER_CRITICAL(&mux);} ~Guard(){portEXIT_CRITICAL(&mux);} };
static pad_status_t state{};
static uint16_t buttons=0,touch=0;
static bool armed=false,blocked=false,pairing=false;
static constexpr uint16_t button_bits[PAD_BUTTON_COUNT]={0x1000,0x400,0x8,0x100,0x4,0x2,0x1,0x20};
static constexpr uint32_t mapping_magic=0x53335032;
static pad_map_entry_t mappings[PAD_BUTTON_COUNT];
static uint32_t epoch=0,last_audio=0;
static int16_t pcm[4800];
static size_t head=0,used=0;
static bool buffered=false;
static uint8_t hid[64][8],last_report[8];
static unsigned hidhead=0,hidused=0;
struct Packet {uint32_t epoch;uint16_t seq;uint8_t len,data[94];};
static QueueHandle_t packets;
uint32_t pad_millis(){return esp_timer_get_time()/1000;}
// All helpers ending in _locked require mux. No blocking calls under mux.
static void report_locked(){
 uint8_t r[8]={};
 if(state.usb){
  if(state.source==MIC_BOARD)r[0]=0x40;
  else if(state.source==MIC_REMOTE)r[0]=mappings[PAD_VOICE].right_option?0x40:0x04;
  unsigned n=2;uint16_t b=(armed&&!blocked?buttons:0);
  auto add=[&](uint8_t key){if(!key||n>=8)return;for(unsigned i=2;i<n;i++)if(r[i]==key)return;r[n++]=key;};
  for(unsigned i=0;i<PAD_BUTTON_COUNT&&n<8;i++){
   const auto &m=mappings[i];
   if((b&button_bits[i])&&m.type==PAD_MAP_KEY&&!m.modifiers)add(m.key);
  }
  if(touch&0x8)add(0x28);
  if(touch&0x100)add(0x2c);
  if(touch&0x1000)add(0x50);
  if(touch&0x400)add(0x4f);
 }
 if(!memcmp(r,last_report,8))return;
 if(hidused==64){ // Fail closed if the host stops consuming reports.
  hidhead=hidused=0;buttons=touch=0;armed=false;blocked=true;
  state.source=MIC_OFF;state.voice=false;epoch++;used=0;buffered=false;memset(r,0,8);
 }
 memcpy(hid[(hidhead+hidused)%64],r,8);hidused++;memcpy(last_report,r,8);
}
static void source_locked(mic_source_t source){
 if(state.source==source)return;
 state.source=source;state.voice=source!=MIC_OFF;epoch++;
 used=head=0;buffered=false;state.last_peak=0;last_audio=pad_millis();report_locked();
}
// One complete shortcut pulse per press. Never mix it with held voice/keys.
static void pulse_locked(const pad_map_entry_t &entry,pad_action_t action){
 if(!state.usb || entry.type!=PAD_MAP_KEY || !entry.key)return;
 // Reserve space for releasing old input, the chord and its release.
 if(hidused>60)return;
 buttons=touch=0;armed=false;blocked=true;
 source_locked(MIC_OFF);report_locked();
 uint8_t r[8]={};
 r[0]=entry.modifiers;
 r[2]=entry.key;
 memcpy(hid[(hidhead+hidused)%64],r,8);hidused++;
 memset(hid[(hidhead+hidused)%64],0,8);hidused++;
 memset(last_report,0,8);
 if(action==PAD_ACTION_CHATGPT||action==PAD_ACTION_CLAUDE)state.last_app=action;
 state.last_input_ms=pad_millis();
}
void pad_mapped_action(pad_button_t button){
 if(button<0||button>=PAD_BUTTON_COUNT)return;
 Guard g; pulse_locked(mappings[button],button==PAD_LEFT?PAD_ACTION_CHATGPT:button==PAD_RIGHT?PAD_ACTION_CLAUDE:PAD_ACTION_NONE);
}
void pad_send_chord(uint8_t modifiers,uint8_t key){
 if(!key||modifiers&~0x0f)return;
 Guard g;pad_map_entry_t entry={PAD_MAP_KEY,key,modifiers,0};pulse_locked(entry,PAD_ACTION_NONE);
}
void pad_shortcut(pad_action_t action){
 if(action<PAD_ACTION_CHATGPT||action>PAD_ACTION_INPUT)return;
 pad_mapped_action(action==PAD_ACTION_CHATGPT?PAD_LEFT:action==PAD_ACTION_CLAUDE?PAD_RIGHT:PAD_TV);
}
static void mapping_defaults(){
 memset(mappings,0,sizeof(mappings));
 mappings[PAD_LEFT]={PAD_MAP_KEY,0x0a,0x0d,0};
 mappings[PAD_RIGHT]={PAD_MAP_KEY,0x06,0x0d,0};
 mappings[PAD_SELECT]={PAD_MAP_KEY,0x28,0,0};
 mappings[PAD_PLAY]={PAD_MAP_KEY,0x2c,0,0};
 mappings[PAD_MINUS]={PAD_MAP_KEY,0x50,0,0};
 mappings[PAD_PLUS]={PAD_MAP_KEY,0x4f,0,0};
 mappings[PAD_TV]={PAD_MAP_KEY,0x2c,0x08,0};
 mappings[PAD_VOICE]={PAD_MAP_VOICE,0,0,1};
}
static bool mapping_valid(pad_button_t button,const pad_map_entry_t &m){
 if(m.type==PAD_MAP_NONE)return true;
 if(m.type==PAD_MAP_VOICE)return button==PAD_VOICE&&m.right_option<=1;
 return m.type==PAD_MAP_KEY&&m.key>=0x04&&m.key<=0x52&&!(m.modifiers&~0x0f);
}
static bool mapping_save(){
 nvs_handle_t h;if(nvs_open("siri_pad",NVS_READWRITE,&h)!=ESP_OK)return false;
 struct {uint32_t magic;pad_map_entry_t entries[PAD_BUTTON_COUNT];} saved;
 saved.magic=mapping_magic;
 {Guard g;memcpy(saved.entries,mappings,sizeof(mappings));}
 esp_err_t err=nvs_set_blob(h,"map_s3",&saved,sizeof(saved));
 if(err==ESP_OK)err=nvs_commit(h);
 nvs_close(h);return err==ESP_OK;
}
void pad_mapping_get(pad_button_t button,pad_map_entry_t *out){
 if(!out||button<0||button>=PAD_BUTTON_COUNT)return;
 Guard g;*out=mappings[button];
}
bool pad_mapping_set(pad_button_t button,const pad_map_entry_t *entry){
 if(!entry||button<0||button>=PAD_BUTTON_COUNT||!mapping_valid(button,*entry))return false;
 {Guard g;mappings[button]=*entry;
  if(button==PAD_VOICE&&state.source==MIC_REMOTE)source_locked(MIC_OFF);
  report_locked();}
 return mapping_save();
}
void pad_mapping_reset(){
 {Guard g;mapping_defaults();source_locked(MIC_OFF);report_locked();}
 mapping_save();
}
static void append_locked(const int16_t *data,size_t n){
 unsigned peak=0;
 for(size_t i=0;i<n;i++)peak=std::max(peak,unsigned(data[i]<0?-int(data[i]):data[i]));
 state.last_peak=peak;last_audio=pad_millis();state.audio_frames++;
 if(n>4800){data+=n-4800;n=4800;}
 if(used+n>4800){size_t excess=used+n-4800;head=(head+excess)%4800;used-=excess;state.fifo_drops++;}
 for(size_t i=0;i<n;i++){pcm[(head+used+i)%4800]=data[i];}
 used+=n;
 if(used>=960)buffered=true;
}
void pad_remote_connecting(bool active){Guard g;state.connecting=active;}
void pad_remote_connected(bool ready){Guard g;state.ble=ready;state.connecting=false;buttons=0;armed=false;blocked=false;state.last_input_ms=pad_millis();if(state.source==MIC_REMOTE)source_locked(MIC_OFF);report_locked();}
void pad_remote_buttons(uint16_t mask){
 Guard g;if(!state.ble)return;
 if(mask!=buttons)state.last_input_ms=pad_millis();
 if(!mask){armed=true;blocked=false;}
 uint16_t prev=buttons;buttons=mask;
 if(armed&&state.usb&&!blocked){
  int held_shortcuts=0,pulse_index=-1;
  for(unsigned i=0;i<PAD_BUTTON_COUNT;i++){
   if(!(mask&button_bits[i])||mappings[i].type!=PAD_MAP_KEY||!mappings[i].modifiers)continue;
   held_shortcuts++;
   if(mask&~prev&button_bits[i])pulse_index=i;
  }
  if(held_shortcuts==1&&pulse_index>=0){
   pulse_locked(mappings[pulse_index],pulse_index==PAD_LEFT?PAD_ACTION_CHATGPT:pulse_index==PAD_RIGHT?PAD_ACTION_CLAUDE:PAD_ACTION_NONE);return;
  }
 }
 if(armed && state.usb && !blocked && (mask&0x20) && !(prev&0x20)&&mappings[PAD_VOICE].type==PAD_MAP_VOICE)source_locked(MIC_REMOTE);
 if(!(mask&0x20) && state.source==MIC_REMOTE)source_locked(MIC_OFF);
 report_locked();
}
void pad_remote_audio(const uint8_t *data,size_t len){
 if(len<5 || data[4]>94 || size_t(data[4])+5>len)return;
 Packet p{};
 {Guard g;if(!state.usb||state.source!=MIC_REMOTE)return;
  if(!data[4]){blocked=true;source_locked(MIC_OFF);return;}
  p.epoch=epoch;p.seq=data[2]|(data[3]<<8);p.len=data[4];}
 memcpy(p.data,data+5,p.len);
 if(xQueueSend(packets,&p,0)!=pdTRUE){Guard g;state.fifo_drops++;}
}
void pad_usb_connected(bool ready){
 Guard g;state.usb=ready;state.last_app=PAD_ACTION_NONE;buttons=touch=0;armed=false;blocked=false;source_locked(MIC_OFF);
 hidhead=hidused=0;memset(last_report,0,8);memset(hid[0],0,8);hidused=ready?1:0;
}
void pad_touch_key(uint8_t key,bool down){
 uint16_t bit=key==0x28?8:key==0x50?0x1000:key==0x4f?0x400:key==0x2c?0x100:0;
 Guard g;if(!state.usb||!bit)return;state.last_input_ms=pad_millis();if(down)touch|=bit;else touch&=~bit;report_locked();
}
void pad_toggle_board_mic(){Guard g;if(!state.usb||!state.board_ready||state.source==MIC_REMOTE)return;state.last_input_ms=pad_millis();source_locked(state.source==MIC_BOARD?MIC_OFF:MIC_BOARD);}
void pad_cancel_voice(){Guard g;blocked=true;source_locked(MIC_OFF);}
void pad_request_pairing(){Guard g;pairing=true;}
bool pad_take_pairing_request(){Guard g;bool p=pairing;pairing=false;return p;}
void pad_set_board_ready(bool ready){Guard g;state.board_ready=ready;if(!ready&&state.source==MIC_BOARD)source_locked(MIC_OFF);}
bool pad_board_active(){Guard g;return state.source==MIC_BOARD;}
uint32_t pad_board_token(){Guard g;return state.source==MIC_BOARD?epoch:0;}
void pad_board_pcm(const int16_t *data,size_t n,uint32_t token){Guard g;if(state.usb&&state.source==MIC_BOARD&&epoch==token)append_locked(data,n);}
void pad_read_pcm(int16_t *data,size_t n){
 Guard g;
 for(size_t i=0;i<n;i++){data[i]=0;if(state.usb&&state.source!=MIC_OFF&&buffered&&used){data[i]=pcm[head];head=(head+1)%4800;used--;}}
 if(!used)buffered=false;
}
void pad_status(pad_status_t *out){Guard g;*out=state;}
bool pad_next_hid(uint8_t r[8]){Guard g;if(!hidused)return false;memcpy(r,hid[hidhead],8);hidhead=(hidhead+1)%64;hidused--;return true;}
void pad_service(){Guard g;if(state.source!=MIC_OFF&&pad_millis()-last_audio>1000){blocked=true;source_locked(MIC_OFF);}}
static void decode_task(void*){
 void *decoder=nullptr;uint32_t current=~0u;uint16_t seq=0;bool have_seq=false;Packet p;int16_t out[960];
 for(;;){
  if(xQueueReceive(packets,&p,pdMS_TO_TICKS(100))!=pdTRUE)continue;
  {Guard g;if(p.epoch!=epoch||state.source!=MIC_REMOTE)continue;}
  if(current!=p.epoch){
   if(decoder){esp_opus_dec_close(decoder);}
   decoder=nullptr;
   esp_opus_dec_cfg_t cfg={48000,1,ESP_OPUS_DEC_FRAME_DURATION_20_MS,false};
   auto err=esp_opus_dec_open(&cfg,sizeof(cfg),&decoder);current=p.epoch;have_seq=false;
   if(err!=ESP_AUDIO_ERR_OK){Guard g;state.audio_errors++;source_locked(MIC_OFF);continue;}
  }
  if(have_seq){uint16_t delta=p.seq-seq;if(delta==0||delta>32768)continue;if(delta>1){Guard g;state.lost_packets+=delta-1;}}
  seq=p.seq;have_seq=true;
  esp_audio_dec_in_raw_t raw{};raw.buffer=p.data;raw.len=p.len;
  esp_audio_dec_out_frame_t frame{};frame.buffer=(uint8_t*)out;frame.len=sizeof(out);esp_audio_dec_info_t info{};
  auto err=esp_opus_dec_decode(decoder,&raw,&frame,&info);
  Guard g;if(p.epoch!=epoch||state.source!=MIC_REMOTE)continue;
  if(err!=ESP_AUDIO_ERR_OK||frame.decoded_size!=1920||info.sample_rate!=48000||info.channel!=1){state.audio_errors++;continue;}
  append_locked(out,960);
 }
}
void pad_init(){
 mapping_defaults();
 nvs_handle_t h;
 if(nvs_open("siri_pad",NVS_READONLY,&h)==ESP_OK){
  struct {uint32_t magic;pad_map_entry_t entries[PAD_BUTTON_COUNT];} saved;
  size_t size=sizeof(saved);
  if(nvs_get_blob(h,"map_s3",&saved,&size)==ESP_OK&&size==sizeof(saved)&&saved.magic==mapping_magic){
   bool valid=true;for(int i=0;i<PAD_BUTTON_COUNT;i++)if(!mapping_valid((pad_button_t)i,saved.entries[i]))valid=false;
   if(valid)memcpy(mappings,saved.entries,sizeof(mappings));
  }
  nvs_close(h);
 }
 state.last_input_ms=pad_millis();
 packets=xQueueCreate(32,sizeof(Packet));assert(packets);xTaskCreatePinnedToCore(decode_task,"opus",24576,nullptr,6,nullptr,1);
}
