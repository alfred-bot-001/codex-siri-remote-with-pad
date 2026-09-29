#include "usb_keyboard.h"
#include <string.h>
#include "tusb.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG="pad-usb";
static const uint8_t report_descriptor[]={TUD_HID_REPORT_DESC_KEYBOARD()};
static const tusb_desc_device_t device_descriptor={
    .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,
    .bcdUSB=0x0200,.bDeviceClass=TUSB_CLASS_UNSPECIFIED,
    .bMaxPacketSize0=64,.idVendor=0x303a,.idProduct=0x4015,
    .bcdDevice=0x0100,.iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1
};
#define CONFIG_LENGTH (TUD_CONFIG_DESC_LEN+TUD_HID_DESC_LEN)
static const uint8_t configuration_descriptor[]={
    TUD_CONFIG_DESCRIPTOR(1,1,0,CONFIG_LENGTH,0,100),
    TUD_HID_DESCRIPTOR(0,4,HID_ITF_PROTOCOL_KEYBOARD,sizeof(report_descriptor),0x81,8,2)
};
_Static_assert(sizeof(configuration_descriptor)==CONFIG_LENGTH,"USB descriptor length");

uint8_t const *tud_descriptor_device_cb(void){return (const uint8_t *)&device_descriptor;}
uint8_t const *tud_descriptor_configuration_cb(uint8_t index){(void)index;return configuration_descriptor;}
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance){(void)instance;return report_descriptor;}
uint16_t const *tud_descriptor_string_cb(uint8_t index,uint16_t langid){
    (void)langid;
    static const char *strings[]={"","Siri Voice Pad","Siri Voice Pad P4 Keyboard","80F1B2D5C87F","Keyboard"};
    static uint16_t result[64];
    if(index==0){result[0]=0x0304;result[1]=0x0409;return result;}
    if(index>=sizeof(strings)/sizeof(strings[0]))return NULL;
    size_t n=strlen(strings[index]);if(n>63)n=63;
    result[0]=(TUSB_DESC_STRING<<8)|(2*n+2);
    for(size_t i=0;i<n;i++)result[i+1]=(uint8_t)strings[index][i];
    return result;
}

typedef struct {uint8_t modifier,key;} key_report_t;
static QueueHandle_t reports;
static volatile bool mounted;
static key_report_t last_report;
static usb_phy_handle_t phy;

uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,uint8_t *buffer,uint16_t length){
    (void)instance;(void)id;
    if(type!=HID_REPORT_TYPE_INPUT||length<8)return 0;
    memset(buffer,0,8);buffer[0]=last_report.modifier;buffer[2]=last_report.key;return 8;
}
void tud_hid_set_report_cb(uint8_t instance,uint8_t id,hid_report_type_t type,const uint8_t *buffer,uint16_t size){
    (void)instance;(void)id;(void)type;(void)buffer;(void)size;
}
void tud_mount_cb(void){mounted=true;ESP_LOGI(TAG,"computer USB keyboard mounted");}
void tud_umount_cb(void){mounted=false;xQueueReset(reports);last_report=(key_report_t){0};ESP_LOGI(TAG,"computer USB keyboard unmounted");}
void tud_suspend_cb(bool wake){(void)wake;mounted=false;xQueueReset(reports);last_report=(key_report_t){0};}
void tud_resume_cb(void){mounted=tud_mounted();}

bool usb_keyboard_connected(void){return mounted&&!tud_suspended();}
static bool enqueue(key_report_t report){
    if(!usb_keyboard_connected()||!reports)return false;
    if(xQueueSend(reports,&report,0)==pdTRUE)return true;
    // If the host stalls, discard stale keys and ensure a release is sent first.
    xQueueReset(reports);key_report_t release={0};xQueueSend(reports,&release,0);
    return false;
}
bool usb_keyboard_send(uint8_t modifiers,uint8_t key){
    key_report_t press={modifiers,key},release={0};
    if(!enqueue(press))return false;
    return enqueue(release);
}
bool usb_keyboard_set(uint8_t modifiers,uint8_t key){return enqueue((key_report_t){modifiers,key});}
void usb_keyboard_release(void){enqueue((key_report_t){0});}

static void task(void *arg){
    (void)arg;key_report_t pending={0};bool have_pending=false;
    while(1){
        tud_task();
        if(!usb_keyboard_connected()){have_pending=false;vTaskDelay(pdMS_TO_TICKS(1));continue;}
        if(!have_pending)have_pending=xQueueReceive(reports,&pending,0)==pdTRUE;
        if(have_pending&&tud_hid_ready()){
            uint8_t keys[6]={pending.key};
            if(tud_hid_keyboard_report(0,pending.modifier,keys)){
                last_report=pending;have_pending=false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
void usb_keyboard_start(void){
    reports=xQueueCreate(64,sizeof(key_report_t));
    if(!reports){ESP_LOGE(TAG,"keyboard queue allocation failed");return;}
    usb_phy_config_t cfg={.controller=USB_PHY_CTRL_OTG,.target=USB_PHY_TARGET_INT,.otg_mode=USB_OTG_MODE_DEVICE};
    esp_err_t err=usb_new_phy(&cfg,&phy);
    if(err!=ESP_OK){ESP_LOGE(TAG,"USB PHY: %s",esp_err_to_name(err));return;}
    if(!tusb_init()){ESP_LOGE(TAG,"TinyUSB initialization failed");return;}
    if(xTaskCreate(task,"usb-keyboard",6144,NULL,8,NULL)!=pdPASS)ESP_LOGE(TAG,"USB task failed");
}
