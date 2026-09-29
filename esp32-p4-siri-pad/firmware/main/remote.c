#include "remote.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG="pad-remote";
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static remote_status_t status;
static ble_addr_t candidates[REMOTE_CANDIDATES], saved_peer;
static uint8_t own_type;
static uint16_t connection=BLE_HS_CONN_HANDLE_NONE, service_start, service_end;
static bool have_peer, auto_reconnect, link_error, user_stop;
static struct ble_npl_event command_event;
static struct ble_npl_callout retry_timer;
static QueueHandle_t commands;
typedef struct { remote_command_t kind; ble_addr_t address; } command_t;
typedef struct { uint16_t def,value,end,ref,cccd; uint8_t id,type,props; } report_t;
static report_t reports[24];
static int report_count,report_index,button_index,audio_index,enable_index;
void ble_store_config_init(void);
static int gap_event(struct ble_gap_event *event,void *arg);
static void start_connection(const ble_addr_t *address);

static void address_text(const ble_addr_t *a,char out[18]){
    snprintf(out,18,"%02x:%02x:%02x:%02x:%02x:%02x",a->val[5],a->val[4],a->val[3],a->val[2],a->val[1],a->val[0]);
}
static void phase(remote_phase_t p,int error){
    portENTER_CRITICAL(&lock);status.phase=p;status.error=error;status.revision++;portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG,"state=%d error=%d",p,error);
}
void remote_get_status(remote_status_t *out){portENTER_CRITICAL(&lock);*out=status;portEXIT_CRITICAL(&lock);}
static void retry_later(void){if(auto_reconnect&&have_peer)ble_npl_callout_reset(&retry_timer,ble_npl_time_ms_to_ticks32(10000));}
static void failed(int error){
    link_error=true;phase(REMOTE_ERROR,error);
    if(connection!=BLE_HS_CONN_HANDLE_NONE)ble_gap_terminate(connection,BLE_ERR_REM_USER_CONN_TERM);
    else retry_later();
}
static bool save_peer(const ble_addr_t *peer){
    nvs_handle_t h;if(nvs_open("siri_pad",NVS_READWRITE,&h)!=ESP_OK)return false;
    esp_err_t e=nvs_set_blob(h,"ble_peer",peer,sizeof(*peer));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e==ESP_OK;
}
static void ready(void){
    struct ble_gap_conn_desc desc;
    if(ble_gap_conn_find(connection,&desc)!=0||!desc.sec_state.encrypted){failed(BLE_HS_ENOTCONN);return;}
    saved_peer=desc.peer_id_addr;have_peer=true;auto_reconnect=true;
    bool stored=save_peer(&saved_peer);
    portENTER_CRITICAL(&lock);status.paired=stored&&desc.sec_state.bonded;address_text(&saved_peer,status.peer);portEXIT_CRITICAL(&lock);
    phase(REMOTE_READY,stored?0:ESP_FAIL);
    ESP_LOGI(TAG,"HID ready: peer=%s interval=%u encrypted=%u bonded=%u persisted=%u",status.peer,desc.conn_itvl,desc.sec_state.encrypted,desc.sec_state.bonded,stored);
}
static int enable_done(uint16_t handle,const struct ble_gatt_error *error,struct ble_gatt_attr *attr,void *arg){
    (void)attr;(void)arg;if(handle!=connection)return 0;
    if(error->status)failed(error->status);else ready();return 0;
}
static int subscribe_done(uint16_t handle,const struct ble_gatt_error *error,struct ble_gatt_attr *attr,void *arg){
    (void)attr;if(handle!=connection)return 0;
    if(error->status){failed(error->status);return 0;}
    int step=(int)(intptr_t)arg;int rc;
    if(step==0){uint8_t cccd[]={1,0};rc=ble_gattc_write_flat(connection,reports[audio_index].cccd,cccd,2,subscribe_done,(void*)1);}
    else{
        uint8_t enable=0xaf;report_t *r=&reports[enable_index];
        if(r->props&BLE_GATT_CHR_PROP_WRITE)rc=ble_gattc_write_flat(connection,r->value,&enable,1,enable_done,NULL);
        else{rc=ble_gattc_write_no_rsp_flat(connection,r->value,&enable,1);if(!rc)ready();}
    }
    if(rc)failed(rc);
    return 0;
}
static void subscribe_reports(void){
    button_index=audio_index=enable_index=-1;
    for(int i=0;i<report_count;i++){
        report_t *r=&reports[i];
        if(r->id==0xfb&&r->type==1&&r->cccd)button_index=i;
        if(r->id==0xfa&&r->type==1&&r->cccd)audio_index=i;
        if(r->id==0xf0&&(r->type==2||r->type==3))enable_index=i;
    }
    if(button_index<0||audio_index<0||enable_index<0){failed(BLE_HS_ENOENT);return;}
    uint8_t cccd[]={1,0};int rc=ble_gattc_write_flat(connection,reports[button_index].cccd,cccd,2,subscribe_done,NULL);if(rc)failed(rc);
}
static void next_report(void);
static int reference_done(uint16_t handle,const struct ble_gatt_error *error,struct ble_gatt_attr *attr,void *arg){
    (void)arg;if(handle!=connection)return 0;
    if(error->status){failed(error->status);return 0;}
    uint8_t value[2];if(!attr||OS_MBUF_PKTLEN(attr->om)<2||os_mbuf_copydata(attr->om,0,2,value)){failed(BLE_HS_EBADDATA);return 0;}
    reports[report_index].id=value[0];reports[report_index].type=value[1];
    ESP_LOGI(TAG,"report id=0x%02x type=%u value=%u",value[0],value[1],reports[report_index].value);
    report_index++;next_report();return 0;
}
static int descriptor_found(uint16_t handle,const struct ble_gatt_error *error,uint16_t chr,const struct ble_gatt_dsc *dsc,void *arg){
    (void)chr;(void)arg;if(handle!=connection)return 0;
    report_t *r=&reports[report_index];
    if(!error->status){uint16_t uuid=ble_uuid_u16(&dsc->uuid.u);if(uuid==0x2908)r->ref=dsc->handle;if(uuid==0x2902)r->cccd=dsc->handle;return 0;}
    if(error->status!=BLE_HS_EDONE){failed(error->status);return 0;}
    if(r->ref){int rc=ble_gattc_read(connection,r->ref,reference_done,NULL);if(rc)failed(rc);}
    else{report_index++;next_report();}return 0;
}
static void next_report(void){
    while(report_index<report_count&&reports[report_index].value>=reports[report_index].end)report_index++;
    if(report_index>=report_count){subscribe_reports();return;}
    report_t *r=&reports[report_index];int rc=ble_gattc_disc_all_dscs(connection,r->value,r->end,descriptor_found,NULL);if(rc)failed(rc);
}
static int characteristic_found(uint16_t handle,const struct ble_gatt_error *error,const struct ble_gatt_chr *chr,void *arg){
    (void)arg;if(handle!=connection)return 0;
    if(!error->status){
        if(report_count&&reports[report_count-1].end==service_end)reports[report_count-1].end=chr->def_handle-1;
        if(ble_uuid_u16(&chr->uuid.u)==0x2a4d){
            if(report_count>=24){failed(BLE_HS_ENOMEM);return 0;}
            reports[report_count++]=(report_t){.def=chr->def_handle,.value=chr->val_handle,.end=service_end,.props=chr->properties};
        }return 0;
    }
    if(error->status!=BLE_HS_EDONE){failed(error->status);return 0;}
    report_index=0;next_report();return 0;
}
static int service_found(uint16_t handle,const struct ble_gatt_error *error,const struct ble_gatt_svc *svc,void *arg){
    (void)arg;if(handle!=connection)return 0;
    if(!error->status){service_start=svc->start_handle;service_end=svc->end_handle;return 0;}
    if(error->status!=BLE_HS_EDONE||!service_start){failed(error->status==BLE_HS_EDONE?BLE_HS_ENOENT:error->status);return 0;}
    int rc=ble_gattc_disc_all_chrs(connection,service_start,service_end,characteristic_found,NULL);if(rc)failed(rc);
    return 0;
}
static void discover(void){
    phase(REMOTE_DISCOVERING,0);service_start=service_end=0;report_count=0;memset(reports,0,sizeof(reports));
    int rc=ble_gattc_disc_svc_by_uuid(connection,BLE_UUID16_DECLARE(0x1812),service_found,NULL);if(rc)failed(rc);
}
static int mtu_done(uint16_t handle,const struct ble_gatt_error *error,uint16_t mtu,void *arg){
    (void)arg;if(handle!=connection)return 0;
    ESP_LOGI(TAG,"MTU=%u status=%d",mtu,error->status);
    if(error->status)failed(error->status);else discover();
    return 0;
}
static void scan_result(const struct ble_gap_disc_desc *disc){
    struct ble_hs_adv_fields fields;
    if(ble_hs_adv_parse_fields(&fields,disc->data,disc->length_data))return;
    bool hid=false;for(int i=0;i<fields.num_uuids16;i++)if(ble_uuid_u16(&fields.uuids16[i].u)==0x1812)hid=true;
    const uint8_t *m=fields.mfg_data;
    if(!hid||fields.mfg_data_len<14||m[0]!=0x4c||m[1]!=0||m[2]!=7||m[3]!=13)return;
    portENTER_CRITICAL(&lock);
    int i;for(i=0;i<status.count;i++)if(ble_addr_cmp(&candidates[i],&disc->addr)==0)break;
    if(i<REMOTE_CANDIDATES){candidates[i]=disc->addr;address_text(&disc->addr,status.candidates[i].address);status.candidates[i].rssi=disc->rssi;
        if(i==status.count)status.count++;
        status.revision++;}
    portEXIT_CRITICAL(&lock);
}
static int gap_event(struct ble_gap_event *event,void *arg){
    (void)arg;
    switch(event->type){
    case BLE_GAP_EVENT_DISC:scan_result(&event->disc);break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if(status.phase==REMOTE_SCANNING){phase(REMOTE_IDLE,0);retry_later();}break;
    case BLE_GAP_EVENT_CONNECT:
        if(user_stop){
            if(!event->connect.status)ble_gap_terminate(event->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
            phase(REMOTE_IDLE,0);break;
        }
        if(event->connect.status){connection=BLE_HS_CONN_HANDLE_NONE;failed(event->connect.status);break;}
        connection=event->connect.conn_handle;phase(REMOTE_PAIRING,0);
        {int rc=ble_gap_security_initiate(connection);if(rc)failed(rc);}break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if(event->enc_change.conn_handle==connection){if(event->enc_change.status)failed(event->enc_change.status);else {int rc=ble_gattc_exchange_mtu(connection,mtu_done,NULL);if(rc==BLE_HS_EALREADY)discover();else if(rc)failed(rc);}}break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection=BLE_HS_CONN_HANDLE_NONE;button_index=audio_index=enable_index=-1;
        portENTER_CRITICAL(&lock);status.buttons=0;portEXIT_CRITICAL(&lock);
        if(user_stop)phase(REMOTE_IDLE,0);
        else if(!link_error)phase(REMOTE_IDLE,event->disconnect.reason);
        retry_later();break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:{
        struct ble_gap_conn_desc d;if(!ble_gap_conn_find(event->repeat_pairing.conn_handle,&d))ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;}
    case BLE_GAP_EVENT_NOTIFY_RX:{
        if(event->notify_rx.conn_handle!=connection)break;
        uint16_t attr=event->notify_rx.attr_handle;uint16_t length=OS_MBUF_PKTLEN(event->notify_rx.om);
        if(button_index>=0&&attr==reports[button_index].value&&length>=2){
            uint8_t data[2];os_mbuf_copydata(event->notify_rx.om,0,2,data);uint16_t mask=data[0]|(data[1]<<8);
            portENTER_CRITICAL(&lock);status.buttons=mask;status.button_reports++;status.revision++;portEXIT_CRITICAL(&lock);
            ESP_LOGI(TAG,"buttons=0x%04x",mask);
        }else if(audio_index>=0&&attr==reports[audio_index].value){
            portENTER_CRITICAL(&lock);status.audio_packets++;status.revision++;portEXIT_CRITICAL(&lock);
        }break;}
    default:break;
    }return 0;
}
static void start_connection(const ble_addr_t *address){
    if(connection!=BLE_HS_CONN_HANDLE_NONE||ble_gap_conn_active())return;
    ble_npl_callout_stop(&retry_timer);link_error=false;user_stop=false;
    if(ble_gap_disc_active())ble_gap_disc_cancel();
    phase(REMOTE_CONNECTING,0);
    struct ble_gap_conn_params params={.scan_itvl=0x60,.scan_window=0x30,.itvl_min=12,.itvl_max=12,.latency=0,.supervision_timeout=200,.min_ce_len=0,.max_ce_len=0};
    int rc=ble_gap_connect(own_type,address,10000,&params,gap_event,NULL);if(rc)failed(rc);
}
static void retry_event(struct ble_npl_event *e){(void)e;if(auto_reconnect&&have_peer&&connection==BLE_HS_CONN_HANDLE_NONE&&!ble_gap_disc_active())start_connection(&saved_peer);}
static void command_handler(struct ble_npl_event *e){
    (void)e;command_t cmd;
    while(xQueueReceive(commands,&cmd,0)==pdTRUE){
        if(cmd.kind==REMOTE_CONNECT){auto_reconnect=false;start_connection(&cmd.address);}
        else if(cmd.kind==REMOTE_RECONNECT){auto_reconnect=true;if(have_peer)start_connection(&saved_peer);else phase(REMOTE_IDLE,0);}
        else if(cmd.kind==REMOTE_SCAN){
            if(connection!=BLE_HS_CONN_HANDLE_NONE||ble_gap_conn_active())continue;
            auto_reconnect=false;ble_npl_callout_stop(&retry_timer);if(ble_gap_disc_active())ble_gap_disc_cancel();
            portENTER_CRITICAL(&lock);status.count=0;portEXIT_CRITICAL(&lock);
            phase(REMOTE_SCANNING,0);
            struct ble_gap_disc_params p={.itvl=160,.window=144,.filter_policy=0,.limited=0,.passive=0,.filter_duplicates=1};
            int rc=ble_gap_disc(own_type,20000,&p,gap_event,NULL);if(rc)failed(rc);
        }else{
            auto_reconnect=false;link_error=false;user_stop=true;ble_npl_callout_stop(&retry_timer);
            if(ble_gap_disc_active())ble_gap_disc_cancel();
            if(ble_gap_conn_active())ble_gap_conn_cancel();
            if(connection!=BLE_HS_CONN_HANDLE_NONE)ble_gap_terminate(connection,BLE_ERR_REM_USER_CONN_TERM);
            if(cmd.kind==REMOTE_FORGET){
                nvs_handle_t h;esp_err_t err=nvs_open("siri_pad",NVS_READWRITE,&h);
                if(err==ESP_OK){err=nvs_erase_key(h,"ble_peer");if(err==ESP_OK||err==ESP_ERR_NVS_NOT_FOUND)err=nvs_commit(h);nvs_close(h);}
                if(err!=ESP_OK){phase(REMOTE_ERROR,err);continue;}
                if(have_peer){int rc=ble_store_util_delete_peer(&saved_peer);if(rc&&rc!=BLE_HS_ENOENT){phase(REMOTE_ERROR,rc);continue;}}
                have_peer=false;
                portENTER_CRITICAL(&lock);status.paired=false;status.peer[0]=0;portEXIT_CRITICAL(&lock);
            }
            phase(REMOTE_IDLE,0);
        }
    }
}
bool remote_command(remote_command_t kind,int candidate){
    command_t cmd={.kind=kind};bool valid;
    portENTER_CRITICAL(&lock);valid=status.driver_ready;
    if(kind==REMOTE_CONNECT){valid=valid&&candidate>=0&&candidate<status.count;if(valid)cmd.address=candidates[candidate];}
    portEXIT_CRITICAL(&lock);
    if(!valid||xQueueSend(commands,&cmd,0)!=pdTRUE)return false;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&command_event);return true;
}
static void synced(void){
    int rc=ble_hs_util_ensure_addr(0);if(!rc)rc=ble_hs_id_infer_auto(0,&own_type);if(rc){failed(rc);return;}
    portENTER_CRITICAL(&lock);status.driver_ready=true;portEXIT_CRITICAL(&lock);
    phase(REMOTE_IDLE,0);ESP_LOGI(TAG,"C6 Bluetooth HCI ready");retry_later();
}
static void reset(int reason){
    portENTER_CRITICAL(&lock);status.driver_ready=false;status.buttons=0;portEXIT_CRITICAL(&lock);
    connection=BLE_HS_CONN_HANDLE_NONE;phase(REMOTE_ERROR,reason);
}
static void host_task(void *arg){(void)arg;nimble_port_run();nimble_port_freertos_deinit();}
static void start_task(void *arg){
    (void)arg;nvs_handle_t h;
    if(nvs_open("siri_pad",NVS_READONLY,&h)==ESP_OK){size_t size=sizeof(saved_peer);
        if(nvs_get_blob(h,"ble_peer",&saved_peer,&size)==ESP_OK&&size==sizeof(saved_peer)&&saved_peer.type<=3){have_peer=true;auto_reconnect=true;portENTER_CRITICAL(&lock);status.paired=true;address_text(&saved_peer,status.peer);portEXIT_CRITICAL(&lock);}nvs_close(h);}
    int rc=nimble_port_init();if(rc){phase(REMOTE_ERROR,rc);vTaskDelete(NULL);return;}
    ble_npl_event_init(&command_event,command_handler,NULL);ble_npl_callout_init(&retry_timer,nimble_port_get_dflt_eventq(),retry_event,NULL);
    ble_hs_cfg.reset_cb=reset;ble_hs_cfg.sync_cb=synced;ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap=BLE_HS_IO_NO_INPUT_OUTPUT;ble_hs_cfg.sm_bonding=1;ble_hs_cfg.sm_mitm=0;ble_hs_cfg.sm_sc=1;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();ble_svc_gap_device_name_set("Siri Voice Pad P4");ble_store_config_init();ble_att_set_preferred_mtu(185);
    nimble_port_freertos_init(host_task);vTaskDelete(NULL);
}
static void diagnostic_task(void *arg);
void remote_start(void){xTaskCreate(diagnostic_task,"remote-console",4096,NULL,2,NULL);commands=xQueueCreate(8,sizeof(command_t));if(!commands){phase(REMOTE_ERROR,ESP_ERR_NO_MEM);return;}
    phase(REMOTE_STARTING,0);if(xTaskCreate(start_task,"remote-init",6144,NULL,4,NULL)!=pdPASS)phase(REMOTE_ERROR,ESP_ERR_NO_MEM);}

static void diagnostic_task(void *arg){
    (void)arg;char line[64];size_t used=0;
    for(;;){
        int ch=getchar();
        if(ch==EOF){clearerr(stdin);vTaskDelay(pdMS_TO_TICKS(20));continue;}
        if(ch=='\r'||ch=='\n'){
            line[used]=0;used=0;
            if(!strcmp(line,"status")){
                remote_status_t s;remote_get_status(&s);
                ESP_LOGI(TAG,"STATUS phase=%d driver=%d paired=%d peer=%s candidates=%d error=%d buttons=%04x reports=%lu audio=%lu",s.phase,s.driver_ready,s.paired,s.peer,s.count,s.error,s.buttons,(unsigned long)s.button_reports,(unsigned long)s.audio_packets);
                for(int i=0;i<s.count;i++)ESP_LOGI(TAG,"CANDIDATE %d %s RSSI=%d",i,s.candidates[i].address,s.candidates[i].rssi);
            }else if(!strcmp(line,"scan"))remote_command(REMOTE_SCAN,-1);
            else if(!strcmp(line,"reconnect"))remote_command(REMOTE_RECONNECT,-1);
            else if(!strcmp(line,"disconnect"))remote_command(REMOTE_DISCONNECT,-1);
            else if(!strncmp(line,"connect ",8)){int i;if(sscanf(line+8,"%d",&i)==1)remote_command(REMOTE_CONNECT,i);}
        }else if(used<sizeof(line)-1)line[used++]=(char)ch;
    }
}
