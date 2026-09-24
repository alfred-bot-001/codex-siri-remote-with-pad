#include "pad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_opus_dec.h"
#include <cstring>
#include <algorithm>
#include <cassert>

static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
struct Guard { Guard(){portENTER_CRITICAL(&mux);} ~Guard(){portEXIT_CRITICAL(&mux);} };
static pad_status_t state{};
static uint16_t buttons=0,touch=0;
static bool armed=false,blocked=false,pairing=false;
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
  if(state.source!=MIC_OFF)r[0]=0x40; // HID Right Alt / right Option
  unsigned n=2;uint16_t b=(armed?buttons:0)|touch;
  if(b&0x8)r[n++]=0x28;
  if(b&0x100)r[n++]=0x2c; // Remote Play/Pause -> keyboard Space
  if((b&0x4)||(touch&0x1000))r[n++]=0x50; // Volume Down / screen Left
  if((b&0x2)||(touch&0x400))r[n++]=0x4f; // Volume Up / screen Right
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
static void shortcut_locked(pad_action_t action){
 if(!state.usb || action<PAD_ACTION_CHATGPT || action>PAD_ACTION_INPUT)return;
 // Reserve space for releasing old input, the chord and its release.
 if(hidused>60)return;
 buttons=touch=0;armed=false;blocked=true;
 source_locked(MIC_OFF);report_locked();
 uint8_t r[8]={};
 r[0]=action==PAD_ACTION_INPUT?0x08:0x0d; // Left GUI, or Left Ctrl+Alt+GUI
 r[2]=action==PAD_ACTION_INPUT?0x2c:action==PAD_ACTION_CHATGPT?0x0a:0x06; // Space, G, C
 memcpy(hid[(hidhead+hidused)%64],r,8);hidused++;
 memset(hid[(hidhead+hidused)%64],0,8);hidused++;
 memset(last_report,0,8);
 if(action!=PAD_ACTION_INPUT)state.last_app=action;
}
void pad_shortcut(pad_action_t action){Guard g;shortcut_locked(action);}
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
void pad_remote_connected(bool ready){Guard g;state.ble=ready;state.connecting=false;buttons=0;armed=false;blocked=false;if(state.source==MIC_REMOTE)source_locked(MIC_OFF);report_locked();}
void pad_remote_buttons(uint16_t mask){
 Guard g;if(!state.ble)return;
 if(!mask){armed=true;blocked=false;}
 uint16_t prev=buttons;buttons=mask;
 uint16_t hotkeys=mask&0x1401; // Left, Right, TV
 if(armed && state.usb && hotkeys && !(hotkeys&(hotkeys-1)) && (hotkeys&~prev)){
  shortcut_locked(hotkeys==0x1000?PAD_ACTION_CHATGPT:hotkeys==0x400?PAD_ACTION_CLAUDE:PAD_ACTION_INPUT);
  return;
 }
 if(armed && state.usb && !blocked && (mask&0x20) && !(prev&0x20))source_locked(MIC_REMOTE);
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
 uint16_t bit=key==0x28?8:key==0x50?0x1000:key==0x4f?0x400:0;
 Guard g;if(!state.usb)return;if(down)touch|=bit;else touch&=~bit;report_locked();
}
void pad_toggle_board_mic(){Guard g;if(!state.usb||!state.board_ready||state.source==MIC_REMOTE)return;source_locked(state.source==MIC_BOARD?MIC_OFF:MIC_BOARD);}
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
void pad_init(){packets=xQueueCreate(32,sizeof(Packet));assert(packets);xTaskCreatePinnedToCore(decode_task,"opus",24576,nullptr,6,nullptr,1);}
