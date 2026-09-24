#include "pad.h"
#include "esp_timer.h"
#include <array>
#include <vector>
#include <cassert>
#include <cstdio>
using Report=std::array<uint8_t,8>;
static std::vector<Report> drain(){std::vector<Report> rs;Report r;while(pad_next_hid(r.data()))rs.push_back(r);return rs;}
static pad_status_t status(){pad_status_t s;pad_status(&s);return s;}
static void reset(){pad_usb_connected(false);pad_remote_connected(false);pad_usb_connected(true);pad_remote_connected(true);pad_remote_buttons(0);pad_set_board_ready(true);drain();}
int main(){
 pad_init();reset();
 // Right Option down/up must survive even a short press without audio.
 pad_remote_buttons(0x20);pad_remote_buttons(0);auto r=drain();assert(r.size()==2&&r[0][0]==0x40&&r[1]==Report{});
 // Screen arrows remain cursor keys; center remains Return.
 pad_touch_key(0x50,true);pad_touch_key(0x4f,true);pad_remote_buttons(0x8);
 r=drain();assert(r.back()[2]==0x28&&r.back()[3]==0x50&&r.back()[4]==0x4f);
 pad_touch_key(0x50,false);pad_touch_key(0x4f,false);pad_remote_buttons(0);assert(drain().back()==Report{});
 // A held button during connection must not inject input until a release.
 reset();pad_remote_connected(true);pad_remote_buttons(0x28);assert(drain().empty());assert(!status().voice);pad_remote_buttons(0);pad_remote_buttons(0x20);assert(status().source==MIC_REMOTE);
 // Disconnect releases Option and drops all buffered audio.
 pad_remote_connected(false);assert(drain().back()==Report{});assert(!status().voice);
 // Remote takes priority and must not restart a previously toggled board mic.
 reset();pad_toggle_board_mic();assert(status().source==MIC_BOARD);pad_remote_buttons(0x20);assert(status().source==MIC_REMOTE);pad_toggle_board_mic();assert(status().source==MIC_REMOTE);pad_remote_buttons(0);assert(status().source==MIC_OFF);
 // USB loss cancels toggle and held keys. Reconnect emits a neutral report.
 reset();pad_toggle_board_mic();pad_touch_key(0x28,true);pad_usb_connected(false);assert(!status().voice);pad_usb_connected(true);r=drain();assert(r.size()==1&&r[0]==Report{});
 // Stalled audio releases Option and cannot restart from another held report.
 reset();pad_remote_buttons(0x20);test_time_ms+=1001;pad_service();assert(!status().voice);pad_remote_buttons(0x28);assert(!status().voice);pad_remote_buttons(0);pad_remote_buttons(0x20);assert(status().voice);
 // Old board capture sessions may never leak into a new capture session.
 reset();pad_toggle_board_mic();auto old=pad_board_token();pad_toggle_board_mic();pad_toggle_board_mic();int16_t input[960],out[48];for(auto &v:input)v=1234;
 pad_board_pcm(input,960,old);pad_read_pcm(out,48);for(auto v:out)assert(v==0);
 pad_board_pcm(input,960,pad_board_token());pad_read_pcm(out,48);for(auto v:out)assert(v==1234);
 pad_toggle_board_mic();pad_read_pcm(out,48);for(auto v:out)assert(v==0);
 // HID backlog overflow must fail closed instead of keeping Option held.
 reset();pad_toggle_board_mic();for(int i=0;i<70;i++)pad_touch_key(0x28,i%2==0);assert(!status().voice);r=drain();assert(r.back()==Report{});
 // Malformed voice packets cannot access outside supplied data.
 reset();pad_remote_buttons(0x20);uint8_t short_frame[5]={0,0,0,0,94};pad_remote_audio(short_frame,5);pad_remote_audio(short_frame,0);assert(status().audio_errors==0);
 // A voice-end sentinel releases the modifier immediately.
 short_frame[4]=0;pad_remote_audio(short_frame,5);assert(!status().voice);assert(drain().back()==Report{});
 // Application shortcuts use only LEFT modifiers and produce a complete pulse.
 reset();pad_remote_buttons(0x1000);r=drain();assert(r.size()==2&&r[0][0]==0x0d&&r[0][2]==0x0a&&r[1]==Report{});assert(status().last_app==PAD_ACTION_CHATGPT);
 pad_remote_buttons(0x1000);assert(drain().empty());pad_remote_buttons(0);pad_remote_buttons(0x400);r=drain();assert(r.size()==2&&r[0][0]==0x0d&&r[0][2]==0x06&&r[1]==Report{});assert(status().last_app==PAD_ACTION_CLAUDE);
 // TV sends Command+Space, not Return, once until a complete release.
 pad_remote_buttons(0);pad_remote_buttons(1);r=drain();assert(r.size()==2&&r[0][0]==8&&r[0][2]==0x2c&&r[1]==Report{});pad_remote_buttons(1);assert(drain().empty());
 // Touch app switch stops recording and releases Option before the chord.
 reset();pad_toggle_board_mic();drain();pad_shortcut(PAD_ACTION_CLAUDE);r=drain();assert(r.size()==3&&r[0]==Report{}&&r[1][0]==0x0d&&r[1][2]==0x06&&r[2]==Report{});assert(!status().voice);
 // A remote shortcut during voice does not restart until all buttons release.
 reset();pad_remote_buttons(0x20);drain();pad_remote_buttons(0x1020);r=drain();assert(r.size()==3&&r[0]==Report{}&&r[1][0]==0x0d&&r[2]==Report{});pad_remote_buttons(0x20);assert(!status().voice);pad_remote_buttons(0);pad_remote_buttons(0x20);assert(status().voice);
 // No shortcut before arming, while disconnected or on conflicting directions.
 reset();pad_remote_connected(true);pad_remote_buttons(0x1000);assert(drain().empty());pad_remote_buttons(0);pad_remote_buttons(0x1400);assert(drain().empty());
 pad_usb_connected(false);pad_shortcut(PAD_ACTION_CHATGPT);assert(drain().empty());pad_usb_connected(true);r=drain();assert(r.size()==1&&r[0]==Report{}&&status().last_app==PAD_ACTION_NONE);
 // Refuse a pulse atomically if the report queue has insufficient space.
 reset();for(int i=0;i<62;i++)pad_touch_key(0x28,i%2==0);pad_shortcut(PAD_ACTION_CHATGPT);r=drain();assert(r.size()==62&&r.back()==Report{});for(auto q:r)assert(q[0]==0);
 // Play/Pause is an ordinary Space key with held-state deduplication.
 reset();pad_remote_buttons(0x100);pad_remote_buttons(0x100);r=drain();Report space{};space[2]=0x2c;assert(r.size()==1&&r[0]==space);pad_remote_buttons(0);r=drain();assert(r.size()==1&&r[0]==Report{});
 // BLE loss releases Space; a held key on reconnect stays suppressed.
 reset();pad_remote_buttons(0x100);drain();pad_remote_connected(false);r=drain();assert(r.size()==1&&r[0]==Report{});pad_remote_connected(true);pad_remote_buttons(0x100);assert(drain().empty());pad_remote_buttons(0);pad_remote_buttons(0x100);assert(drain().back()==space);
 // USB loss also clears Space until a full release after reconnect.
 pad_usb_connected(false);pad_usb_connected(true);assert(drain().back()==Report{});pad_remote_buttons(0x100);assert(drain().empty());pad_remote_buttons(0);pad_remote_buttons(0x100);assert(drain().back()==space);pad_remote_buttons(0);assert(drain().back()==Report{});
 puts("PASS: 20 state, source-isolation, shortcut pulse and disconnect scenarios");
}
