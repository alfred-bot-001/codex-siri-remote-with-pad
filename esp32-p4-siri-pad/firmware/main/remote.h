#pragma once
#include <stdbool.h>
#include <stdint.h>
#define REMOTE_CANDIDATES 4

typedef enum { REMOTE_STARTING, REMOTE_IDLE, REMOTE_SCANNING, REMOTE_CONNECTING,
    REMOTE_PAIRING, REMOTE_DISCOVERING, REMOTE_READY, REMOTE_ERROR } remote_phase_t;
typedef struct {
    remote_phase_t phase;
    bool driver_ready, paired;
    int error, count;
    char peer[18];
    struct { char address[18]; int rssi; } candidates[REMOTE_CANDIDATES];
    uint16_t buttons;
    uint32_t button_reports, audio_packets, revision;
} remote_status_t;
typedef enum { REMOTE_SCAN, REMOTE_CONNECT, REMOTE_RECONNECT, REMOTE_DISCONNECT, REMOTE_FORGET } remote_command_t;
void remote_start(void);
void remote_get_status(remote_status_t *out);
bool remote_command(remote_command_t command, int candidate);
