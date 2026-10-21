#include "pad.h"
#include <NimBLEDevice.h>
#include <atomic>
#include <cstring>
#include <strings.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG="remote";
static const char *default_identity="28:2d:7f:3f:50:6e";
static NimBLEClient *client=nullptr;
static NimBLEAddress target;
static bool have_target=false,auto_reconnect=true;
static std::atomic<bool> found{false},disconnected{false},scan_for_all{false};
static std::atomic<int> command{0},selected_index{-1};
static std::atomic<uint16_t> audio_handle{0},button_handle{0};
static portMUX_TYPE status_mux=portMUX_INITIALIZER_UNLOCKED;
static pad_ble_status_t status{};
static char identity[18]="28:2d:7f:3f:50:6e";
static char found_address[18]{};
static uint8_t found_type,candidate_type[4]{};

static void phase(pad_ble_phase_t value){
 portENTER_CRITICAL(&status_mux);status.phase=value;portEXIT_CRITICAL(&status_mux);
}
void pad_ble_status(pad_ble_status_t *out){
 if(!out)return;
 portENTER_CRITICAL(&status_mux);*out=status;portEXIT_CRITICAL(&status_mux);
}
void pad_ble_scan(){command=1;}
void pad_ble_connect_candidate(unsigned index){if(index<4){selected_index=(int)index;command=2;}}
void pad_ble_reconnect(){command=3;}
void pad_ble_disconnect(){command=4;}
void pad_ble_forget(){command=5;}

class Events:public NimBLEClientCallbacks {
 void onDisconnect(NimBLEClient*,int reason)override{
  pad_remote_connected(false);disconnected=true;phase(PAD_BLE_IDLE);
  ESP_LOGI(TAG,"Disconnected %d",reason);
 }
} events;

class ScanEvents:public NimBLEScanCallbacks {
 void onResult(const NimBLEAdvertisedDevice *dev)override{
  if(dev->getRSSI()<-85||!dev->isAdvertisingService(NimBLEUUID(uint16_t(0x1812))))return;
  auto m=dev->getManufacturerData();
  if(m.size()<14||uint8_t(m[0])!=0x4c||m[1]!=0||m[2]!=7||m[3]!=13)return;
  char id[18];snprintf(id,sizeof(id),"%02x:%02x:%02x:%02x:%02x:%02x",uint8_t(m[8]),uint8_t(m[9]),uint8_t(m[10]),uint8_t(m[11]),uint8_t(m[12]),uint8_t(m[13]));
  std::string address=dev->getAddress().toString();bool matching=false;
  portENTER_CRITICAL(&status_mux);
  if(scan_for_all){
   int slot=-1;for(int i=0;i<status.count;i++)if(!strcmp(status.candidates[i].identity,id))slot=i;
   if(slot<0&&status.count<4)slot=status.count++;
   if(slot>=0){
    snprintf(status.candidates[slot].identity,18,"%s",id);
    snprintf(status.candidates[slot].address,18,"%s",address.c_str());
    status.candidates[slot].rssi=dev->getRSSI();candidate_type[slot]=dev->getAddress().getType();
   }
  }else if(!strcmp(id,identity)){
   snprintf(found_address,sizeof(found_address),"%s",address.c_str());
   found_type=dev->getAddress().getType();matching=true;
  }
  portEXIT_CRITICAL(&status_mux);
  if(matching){found=true;NimBLEDevice::getScan()->stop();}
 }
} scans;

static void notify(NimBLERemoteCharacteristic *ch,uint8_t *data,size_t n,bool){
 if(ch->getHandle()==button_handle&&n>=2)pad_remote_buttons(data[0]|(data[1]<<8));
 else if(ch->getHandle()==audio_handle)pad_remote_audio(data,n);
}
static bool connect_remote(){
 pad_remote_connected(false);pad_remote_connecting(true);phase(PAD_BLE_CONNECTING);audio_handle=button_handle=0;
 if(client){NimBLEDevice::deleteClient(client);client=nullptr;}
 client=NimBLEDevice::createClient();client->setClientCallbacks(&events,false);
 client->setConnectTimeout(10000);client->setConnectionParams(12,12,0,200);
 if(!client->connect(target)||!client->secureConnection())return false;
 auto *hid=client->getService(NimBLEUUID(uint16_t(0x1812)));if(!hid)return false;
 auto chars=hid->getCharacteristics(true);NimBLERemoteCharacteristic *a=nullptr,*b=nullptr,*enable=nullptr;
 for(auto *ch:chars){
  if(ch->getUUID()!=NimBLEUUID(uint16_t(0x2a4d)))continue;
  auto *ref=ch->getDescriptor(NimBLEUUID(uint16_t(0x2908)));if(!ref)continue;
  auto v=ref->readValue();if(v.size()<2)continue;
  if(uint8_t(v[0])==0xfa&&v[1]==1)a=ch;
  if(uint8_t(v[0])==0xfb&&v[1]==1)b=ch;
  if(uint8_t(v[0])==0xf0&&(v[1]==2||v[1]==3))enable=ch;
 }
 if(!a||!b||!enable)return false;
 audio_handle=a->getHandle();button_handle=b->getHandle();
 if(!a->subscribe(true,notify)||!b->subscribe(true,notify))return false;
 uint8_t value=0xaf;if(!enable->writeValue(&value,1,!enable->canWriteNoResponse()))return false;
 pad_remote_connected(true);phase(PAD_BLE_READY);
 ESP_LOGI(TAG,"Ready interval=%u",client->getConnInfo().getConnInterval());return true;
}
static void persist_identity(){
 nvs_handle_t h;if(nvs_open("siri_pad",NVS_READWRITE,&h)!=ESP_OK)return;
 nvs_set_str(h,"remote_id",identity);nvs_commit(h);nvs_close(h);
}
static void clear_identity(){
 nvs_handle_t h;if(nvs_open("siri_pad",NVS_READWRITE,&h)!=ESP_OK)return;
 nvs_erase_key(h,"remote_id");nvs_commit(h);nvs_close(h);
}
static void ble_task(void*){
 NimBLEDevice::init("Siri Voice Pad");NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);NimBLEDevice::setSecurityAuth(true,false,true);NimBLEDevice::setMTU(185);
 auto *scan=NimBLEDevice::getScan();scan->setScanCallbacks(&scans,false);scan->setActiveScan(true);scan->setInterval(100);scan->setWindow(90);
 nvs_handle_t h;if(nvs_open("siri_pad",NVS_READONLY,&h)==ESP_OK){size_t n=sizeof(identity);nvs_get_str(h,"remote_id",identity,&n);nvs_close(h);}
 portENTER_CRITICAL(&status_mux);snprintf(status.identity,18,"%s",identity);portEXIT_CRITICAL(&status_mux);
 ESP_LOGI(TAG,"Bonds=%d identity=%s",NimBLEDevice::getNumBonds(),identity);
 for(int i=0;i<NimBLEDevice::getNumBonds();i++){
  auto addr=NimBLEDevice::getBondedAddress(i);
  if(strcasecmp(addr.toString().c_str(),identity)==0){target=addr;have_target=true;break;}
 }
 if(!have_target){target=NimBLEAddress(identity,0);have_target=true;}
 phase(PAD_BLE_IDLE);uint32_t next=pad_millis()+1000;unsigned attempt=0;
 for(;;){
  int cmd=command.exchange(0);
  if(cmd==1){
   auto_reconnect=false;if(client&&client->isConnected())client->disconnect();
   scan->stop();scan_for_all=true;found=false;
   portENTER_CRITICAL(&status_mux);status.count=0;portEXIT_CRITICAL(&status_mux);
   phase(PAD_BLE_SCANNING);scan->start(20000,false,true);
  }else if(cmd==2){
   int index=selected_index.exchange(-1);char addr[18]{};uint8_t type=0;
   portENTER_CRITICAL(&status_mux);
   if(index>=0&&index<status.count){
    snprintf(identity,18,"%s",status.candidates[index].identity);
    snprintf(status.identity,18,"%s",identity);
    snprintf(addr,18,"%s",status.candidates[index].address);type=candidate_type[index];
   }
   portEXIT_CRITICAL(&status_mux);
   if(addr[0]){scan->stop();scan_for_all=false;target=NimBLEAddress(addr,type);have_target=true;persist_identity();auto_reconnect=true;next=pad_millis();attempt=0;}
  }else if(cmd==3){scan->stop();scan_for_all=false;auto_reconnect=true;next=pad_millis();attempt=0;}
  else if(cmd==4){auto_reconnect=false;scan->stop();scan_for_all=false;if(client&&client->isConnected())client->disconnect();phase(PAD_BLE_IDLE);}
  else if(cmd==5){
   auto_reconnect=false;scan->stop();scan_for_all=false;
   if(client&&client->isConnected())client->disconnect();
   if(have_target)NimBLEDevice::deleteBond(target);
   clear_identity();snprintf(identity,18,"%s",default_identity);
   portENTER_CRITICAL(&status_mux);snprintf(status.identity,18,"%s",identity);status.count=0;portEXIT_CRITICAL(&status_mux);
   have_target=false;phase(PAD_BLE_IDLE);
  }
  if(found.exchange(false)&&!scan_for_all){
   char addr[18];uint8_t type;
   portENTER_CRITICAL(&status_mux);snprintf(addr,18,"%s",found_address);type=found_type;portEXIT_CRITICAL(&status_mux);
   target=NimBLEAddress(addr,type);have_target=true;next=pad_millis();attempt=0;
  }
  if(disconnected.exchange(false)&&auto_reconnect)next=pad_millis()+2000;
  if(scan_for_all&&!scan->isScanning()){scan_for_all=false;phase(PAD_BLE_IDLE);}
  if(auto_reconnect&&have_target&&(!client||!client->isConnected())&&!scan_for_all&&int32_t(pad_millis()-next)>=0){
   scan->stop();
   if(connect_remote()){attempt=0;}
   else{
    if(client&&client->isConnected())client->disconnect();
    phase(PAD_BLE_ERROR);attempt++;
    if(attempt%2==1){scan_for_all=false;scan->start(5000,false,true);}
   }
   pad_remote_connecting(false);next=pad_millis()+(attempt<5?7000:15000);
  }
  vTaskDelay(pdMS_TO_TICKS(50));
 }
}
void pad_ble_init(){phase(PAD_BLE_STARTING);xTaskCreatePinnedToCore(ble_task,"remote",12288,nullptr,4,nullptr,0);}
