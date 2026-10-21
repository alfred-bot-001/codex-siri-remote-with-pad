#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
using nvs_handle_t=int;
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
inline esp_err_t nvs_open(const char*,int,nvs_handle_t*){return -1;}
inline esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*){return -1;}
inline esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t){return -1;}
inline esp_err_t nvs_commit(nvs_handle_t){return -1;}
inline void nvs_close(nvs_handle_t){}
