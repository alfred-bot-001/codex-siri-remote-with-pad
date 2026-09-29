#include "audio_stream.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_opus_dec.h"
#include "bsp/esp-bsp.h"
#include "usb_keyboard.h"

static const char *TAG="pad-audio";
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static pad_audio_status_t state;
static int16_t fifo[9600];
static size_t head,used;
static uint32_t generation;
static QueueHandle_t packets;
typedef struct {uint32_t generation;uint16_t seq;uint8_t size,data[94];} audio_packet_t;

static void source_set(pad_mic_source_t source){
    portENTER_CRITICAL(&lock);
    if(state.source!=source){state.source=source;generation++;head=used=0;state.peak=0;}
    portEXIT_CRITICAL(&lock);
}
void audio_stream_stop(void){source_set(PAD_MIC_OFF);usb_keyboard_release();}
bool audio_stream_toggle_board(void){
    pad_mic_source_t next;
    portENTER_CRITICAL(&lock);
    next=state.board_ready?(state.source==PAD_MIC_BOARD?PAD_MIC_OFF:PAD_MIC_BOARD):PAD_MIC_OFF;
    portEXIT_CRITICAL(&lock);
    source_set(next);
    return next==PAD_MIC_BOARD;
}
void audio_stream_remote_button(bool held){
    pad_mic_source_t current;
    portENTER_CRITICAL(&lock);current=state.source;portEXIT_CRITICAL(&lock);
    if(held)source_set(PAD_MIC_REMOTE);
    else if(current==PAD_MIC_REMOTE)source_set(PAD_MIC_OFF);
}
void audio_stream_status(pad_audio_status_t *out){portENTER_CRITICAL(&lock);*out=state;portEXIT_CRITICAL(&lock);}
static void append_pcm(const int16_t *pcm,size_t count,uint32_t expected_generation,pad_mic_source_t source){
    portENTER_CRITICAL(&lock);
    if(expected_generation==generation&&state.source==source){
        unsigned peak=0;
        for(size_t i=0;i<count;i++){
            unsigned amplitude=pcm[i]<0?(unsigned)(-(int32_t)pcm[i]):(unsigned)pcm[i];
            if(amplitude>peak)peak=amplitude;
        }
        state.peak=peak;state.frames++;
        if(count>9600){pcm+=count-9600;count=9600;}
        if(used+count>9600){size_t lost=used+count-9600;head=(head+lost)%9600;used-=lost;}
        for(size_t i=0;i<count;i++)fifo[(head+used+i)%9600]=pcm[i];
        used+=count;
    }
    portEXIT_CRITICAL(&lock);
}
void audio_stream_read(int16_t *out,size_t samples){
    memset(out,0,samples*sizeof(int16_t));
    portENTER_CRITICAL(&lock);
    size_t count=used<samples?used:samples;
    for(size_t i=0;i<count;i++)out[i]=fifo[(head+i)%9600];
    head=(head+count)%9600;used-=count;
    portEXIT_CRITICAL(&lock);
}
void audio_stream_remote_packet(const uint8_t *data,size_t length){
    if(length<5||data[4]>94||length<(size_t)data[4]+5||!packets)return;
    audio_packet_t packet={0};
    portENTER_CRITICAL(&lock);
    if(state.source!=PAD_MIC_REMOTE){portEXIT_CRITICAL(&lock);return;}
    packet.generation=generation;
    portEXIT_CRITICAL(&lock);
    packet.seq=data[2]|(data[3]<<8);packet.size=data[4];
    if(!packet.size){audio_stream_remote_button(false);return;}
    memcpy(packet.data,data+5,packet.size);
    if(xQueueSend(packets,&packet,0)!=pdTRUE){portENTER_CRITICAL(&lock);state.errors++;portEXIT_CRITICAL(&lock);}
}
static void decoder_task(void *arg){
    (void)arg;void *decoder=NULL;uint32_t current=UINT32_MAX;uint16_t previous=0;bool have_previous=false;
    audio_packet_t packet;int16_t output[960];
    for(;;){
        if(xQueueReceive(packets,&packet,pdMS_TO_TICKS(100))!=pdTRUE)continue;
        portENTER_CRITICAL(&lock);bool active=state.source==PAD_MIC_REMOTE&&generation==packet.generation;portEXIT_CRITICAL(&lock);
        if(!active)continue;
        if(current!=packet.generation){
            if(decoder)esp_opus_dec_close(decoder);
            decoder=NULL;esp_opus_dec_cfg_t config={48000,1,ESP_OPUS_DEC_FRAME_DURATION_20_MS,false};
            esp_audio_err_t result=esp_opus_dec_open(&config,sizeof(config),&decoder);
            current=packet.generation;have_previous=false;
            if(result!=ESP_AUDIO_ERR_OK){portENTER_CRITICAL(&lock);state.errors++;portEXIT_CRITICAL(&lock);continue;}
        }
        if(have_previous){
            uint16_t delta=packet.seq-previous;
            if(delta==0||delta>32768)continue;
            if(delta>1){portENTER_CRITICAL(&lock);state.lost_packets+=delta-1;portEXIT_CRITICAL(&lock);}
        }
        previous=packet.seq;have_previous=true;
        esp_audio_dec_in_raw_t raw={.buffer=packet.data,.len=packet.size};
        esp_audio_dec_out_frame_t frame={.buffer=(uint8_t *)output,.len=sizeof(output)};
        esp_audio_dec_info_t info={0};
        esp_audio_err_t result=esp_opus_dec_decode(decoder,&raw,&frame,&info);
        if(result!=ESP_AUDIO_ERR_OK||frame.decoded_size!=sizeof(output)||info.sample_rate!=48000||info.channel!=1){
            portENTER_CRITICAL(&lock);state.errors++;portEXIT_CRITICAL(&lock);continue;
        }
        portENTER_CRITICAL(&lock);state.decoded_packets++;portEXIT_CRITICAL(&lock);
        append_pcm(output,960,packet.generation,PAD_MIC_REMOTE);
    }
}
static void board_task(void *arg){
    (void)arg;
    esp_codec_dev_handle_t microphone=bsp_audio_codec_microphone_init();
    if(!microphone){ESP_LOGE(TAG,"ES7210 microphone initialization failed");vTaskDelete(NULL);return;}
    esp_codec_dev_sample_info_t sample={.sample_rate=48000,.channel=2,.bits_per_sample=16};
    esp_err_t error=esp_codec_dev_open(microphone,&sample);
    if(error!=ESP_OK){ESP_LOGE(TAG,"ES7210 microphone open failed: %s",esp_err_to_name(error));vTaskDelete(NULL);return;}
    error=esp_codec_dev_set_in_gain(microphone,24);
    if(error!=ESP_OK)ESP_LOGW(TAG,"ES7210 gain setup: %s",esp_err_to_name(error));
    portENTER_CRITICAL(&lock);state.board_ready=true;portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG,"ES7210 microphone ready at 48 kHz stereo input");
    int16_t stereo[960],mono[480];
    for(;;){
        portENTER_CRITICAL(&lock);bool active=state.source==PAD_MIC_BOARD;uint32_t current=generation;portEXIT_CRITICAL(&lock);
        if(!active){vTaskDelay(pdMS_TO_TICKS(20));continue;}
        error=esp_codec_dev_read(microphone,stereo,sizeof(stereo));
        if(error!=ESP_OK){portENTER_CRITICAL(&lock);state.errors++;portEXIT_CRITICAL(&lock);vTaskDelay(pdMS_TO_TICKS(10));continue;}
        for(size_t i=0;i<480;i++)mono[i]=stereo[i*2];
        append_pcm(mono,480,current,PAD_MIC_BOARD);
    }
}
void audio_stream_start(void){
    packets=xQueueCreate(32,sizeof(audio_packet_t));
    if(!packets){ESP_LOGE(TAG,"audio packet queue allocation failed");return;}
    if(xTaskCreate(decoder_task,"remote-opus",24576,NULL,6,NULL)!=pdPASS)ESP_LOGE(TAG,"remote decoder task failed");
    if(xTaskCreate(board_task,"board-mic",8192,NULL,5,NULL)!=pdPASS)ESP_LOGE(TAG,"board mic task failed");
}
