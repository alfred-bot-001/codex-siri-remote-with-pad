# 5 寸 P4 Pad：横屏、板载 C6 蓝牙与 USB 键盘

适用 Waveshare ESP32-P4-WIFI6-Touch-LCD-5（本次实板 ESP32-P4 revision v3.2）。使用微雪 LCD5 BSP、HX8394 DSI、GT911 触控和 LVGL 9，显示 1280×720 横屏主页和设置页。

## 当前功能

- 板载 ESP32-C6 经 SDIO 提供真实 BLE 控制器；P4 运行 NimBLE 中心设备。设置页显示初始化、扫描、连接、配对、HID 初始化、已连接和错误状态。
- “搜索”扫描 20 秒，仅列出同时广播 HID 服务与 Siri Remote 厂商标识的设备，最多四台；点选设备后连接并执行加密配对。
- 发现 HID 报告描述符，订阅 `0xfb` 按键与 `0xfa` 音频通知，并向 `0xf0` 写入 `0xaf` 启用报告。仅在订阅成功且链路加密后显示“已连接”。设置页显示按键掩码、报告计数和音频包计数。
- 保存遥控器身份和蓝牙绑定到 P4 NVS；重启自动重连。设置页可以重连、断开和移除配对，移除需要在五秒内再次点按确认。扫描期间暂停自动重连，避免旧设备抢占搜索流程。
- 按键映射页可编辑 13 个遥控器实体按键，保存到 `siri_pad/map_v1`，重启读取并校验；可恢复默认。
- USB OTG 口模拟 HID 键盘。顶栏“电脑已连接”依据 USB 枚举回调显示。主屏为带本地图标的 ChatGPT、Claude、Chrome，以及空格、麦克风、回车六键；Chrome 固定输出 `Ctrl+Opt+Cmd+B`，需电脑端同组合的快捷指令。遥控器左/右键触发保存的 ChatGPT/Claude 快捷键，中央确认、播放/暂停、音量减/加、小电视键和语音键按当前映射输出。发送与 USB 事件处理在不同任务运行，避免事件等待阻塞按键队列。
- USB OTG 口同时声明 UAC 2.0 单声道 48 kHz 麦克风。按住遥控器语音键会按住配置的 Option 键并将遥控器 Opus 包解码为 PCM；屏幕麦克风可切换板载 ES7210 拾音，再点停止。麦克风按钮随两种输入源改变。Mac 已枚举该 USB 麦克风；实际语音录制仍待验证。
- “屏幕与息屏”页可选 1、5、15、30、60 分钟或“永不”，默认 30 分钟；选择保存在 `siri_pad/screen_min`。触摸、遥控器按键／语音和 USB／蓝牙连接状态变化重置计时；保持中的语音输入不会息屏。到时只关闭背光，不停止 USB 键盘、UAC 麦克风或 BLE。首次触摸只唤醒屏幕，不误触底下的按钮；遥控器按键和连接状态变化也可唤醒。

未识别的圆环上/下、返回、静音、电源键虽可在设置中编辑，但尚未确认其 BLE 掩码，不能视为已映射到 USB。

## 配对与诊断

在设置页点“搜索”，按住 Siri Remote 的返回键与音量加键约五秒，列表出现设备后点选。已绑定设备平时按一次中央确认键唤醒即可，无需重新配对。

UART 115200 波特率提供 `status`、`scan`、`connect 0`（列表索引）、`reconnect`、`disconnect`。`status` 返回真实阶段、配对、报告计数。串口适配器开关端口可能触发板子复位，连续诊断应保持同一串口连接。

## 构建

使用 **ESP-IDF v5.5.5** 并先执行其 `export.sh`，确保 `ESP_IDF_VERSION=5.5`；Wi-Fi Remote 的 Kconfig 依赖该环境变量选择 C6 目标配置。

```sh
idf.py -B build/p4_ble \
  -D SDKCONFIG=build/p4_ble/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rev3_x;sdkconfig.defaults.ble' build
```

依赖由 `dependencies.lock` 固定，本次使用 ESP-Hosted 1.4.7、Wi-Fi Remote 0.14.5、TinyUSB 0.19.0~3、USB Device UAC 1.3.1 和 ESP Audio Codec 2.3.0。刷入前核对生成的配置为 P4 最低 revision 3.0、PSRAM 250 MHz、SDIO 4 位／20 MHz、C6 目标及 Hosted NimBLE VHCI；不能仅凭 defaults 文件认定配置已生效。

原理图核对后的 P4→C6 接线：CLK GPIO18、CMD GPIO19、D0 GPIO14、D1 GPIO15、D2 GPIO16、D3 GPIO17、C6 EN GPIO54。没有修改或重新刷写 C6 固件；实板原有固件返回能力 `0x0d`，包括 HCI over SDIO 和 BLE。

## 刷写与备份

`partitions.csv` 与读回的原厂分区表一致。仅把 `build/p4_ble/siri_pad_p4.bin` 写入 `factory` 应用分区 **0x200000**；不要执行会同时刷 bootloader／分区表的默认 `idf.py flash`，不要擦除整片。原厂 8 MiB 应用、前 64 KiB 和 NVS 区域备份位于本地 `../backups/`，不上传 Git。

本次目标 P4 MAC 为 `80:f1:b2:d5:c8:7f`，USB UART 序列号为 `5B90124240`。旧 S3 Pad `1020BA4658B4-PAD1` 是另一台设备，不能刷本固件。

## 验证边界

已在实板验证：应用刷写哈希、1280×720 显示和触控驱动初始化、C6 SDIO 能力响应、蓝牙协议栈就绪、遥控器加密绑定、MTU 185、HID 通知订阅和保存身份后的自动重连；主动断开、扫描启动及 20 秒超时也已验证。2026-09-29 早期键盘固件曾被 Mac 识别为 `Siri Voice Pad P4 Keyboard`（VID 303a / PID 4015），但按键队列诊断发现 USB 事件等待阻塞发送任务，已改为独立任务。新复合固件 PID 4016 已刷入，显示、UAC 驱动、ES7210 48 kHz 和 C6 均成功初始化。随后 Mac 已枚举到 `Siri Voice Pad P4` 和 UAC `Mic stream`；用户已确认新主页布局和触摸反馈正常。实际键盘字符输入和录音质量仍需现场验收。主动断开后遥控器可能需要按键唤醒才能重连。

字形子集来自 Adobe 官方 Source Han Sans SC Bold，许可在 `licenses/SourceHanSansSC-OFL.txt`。主页标题使用 28px，应用快捷键和短暂提示使用 18px。重新生成需 `lv_font_conv` 1.5.3，分别运行 `python tools/generate_font.py /path/to/lv_font_conv 28` 与 `python tools/generate_font.py /path/to/lv_font_conv 18`；脚本首次从 Adobe 的 GitHub 发布分支下载固定 SHA-256 的字体源文件。
