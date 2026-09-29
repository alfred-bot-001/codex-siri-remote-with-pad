#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { PAD_MIC_OFF, PAD_MIC_BOARD, PAD_MIC_REMOTE } pad_mic_source_t;
typedef struct {
    bool board_ready;
    pad_mic_source_t source;
    uint32_t frames, decoded_packets, lost_packets, errors, peak;
} pad_audio_status_t;
void audio_stream_start(void);
void audio_stream_stop(void);
bool audio_stream_toggle_board(void);
void audio_stream_remote_button(bool held);
void audio_stream_remote_packet(const uint8_t *data,size_t length);
void audio_stream_read(int16_t *out,size_t samples);
void audio_stream_status(pad_audio_status_t *out);
