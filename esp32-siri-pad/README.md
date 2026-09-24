# Siri Voice Pad（ESP32-S3 触屏版）

硬件：Waveshare ESP32-S3-Touch-LCD-3.5，320×480 竖屏、16 MB Flash、8 MB PSRAM；已测试的遥控器为 Apple Siri Remote A2854。

![Pad 当前布局](assets/pad-layout.png)

截图为设备界面帧缓冲，实体屏幕保持 180° 旋转。

## 操作

回车按钮采用蓝色；麦克风直径缩至 120 像素，状态文字紧随图标，下方光标／回车按钮加高。

屏幕默认相对初版旋转 **180°**，保持 320×480 竖屏。LVGL 同步旋转触摸坐标，触摸驱动无需再翻转。

- 遥控器侧面语音键：按住发送右 Option，同时将遥控器麦克风声音送入 USB 麦克风；松开停止。
- 遥控器圆盘中央确认键：回车。
- 遥控器播放／暂停键：键盘空格，按下保持、松开释放。
- 遥控器圆环左键／屏幕 ChatGPT：发送左 Control＋左 Option＋左 Command＋G，调用电脑上已有的唤起／隐藏快捷指令。
- 遥控器圆环右键／屏幕 Claude：发送左 Control＋左 Option＋左 Command＋C。
- 遥控器小电视键／屏幕输入法：发送左 Command＋空格，切换输入法。
- 屏幕底部左右箭头：移动光标；中间大回车按钮：回车。
- 三种组合快捷键每次按下只发一次完整的按下／释放；遥控器需全部松开后才接受下一次动作。切换应用或输入法会先停止语音、释放旧按键，避免右 Option 混入组合键。
- 屏幕麦克风：点击开启板载麦克风并按下右 Option，再点停止并释放右 Option。
- 遥控器语音键优先于板载麦克风。切换到遥控器后，松开不会自动恢复板载录音。
- 蓝牙卡片显示连接状态；长按卡片可重新搜索已验证的这只遥控器。开机自动重连。首次连接后的首个按下用于唤醒，收到松开后才允许发送按键，避免连接瞬间意外回车。
- USB 断开／挂起、遥控器断连、音频停止超过一秒时释放语音按键。重新连接不会自动恢复录音。

USB 提供标准 HID 键盘与 UAC2 单声道 48 kHz／16 bit 麦克风，另带 CDC 诊断接口。Mac 的输入设备名称为 **Siri Voice Pad Microphone**。电脑会自动识别设备；输入法是否使用它，取决于系统或应用选中的输入设备，以及右 Option 的快捷键配置。

## 构建

使用 PlatformIO `espressif32@6.12.0`、ESP-IDF 5.5.0，组件版本见 `main/idf_component.yml` 和 `dependencies.lock`。不是 Arduino 固件。

从仓库根目录执行（Python 3、PlatformIO Core）：

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install platformio pyserial esptool
pio run --project-dir esp32-siri-pad
```

生成文件在 `esp32-siri-pad/.pio/build/pad/`。首次安装在开发板进入下载模式后，可用 `pio run --project-dir esp32-siri-pad --target upload --upload-port <下载串口>`；先确认目标板和备份，再刷入。引导程序位于 0x0、分区表 0x8000、应用 0x10000，NVS 位于 0x9000。不要执行全片擦除。

已安装本固件的实测设备可运行 `python esp32-siri-pad/tools/upgrade.py` 更新应用分区。**该工具当前锁定开发者的板子 MAC 和两个 macOS 串口路径**，移植到其他设备需先核实并修改参数；不会自动选择其他板子。它通过 CDC 进入下载模式，核对 MAC，以 no-reset 方式刷写，退出时使用看门狗和 USB 串口复位。升级不写 NVS。蓝牙配置保留两个通用 MAC 地址，以沿用原 Arduino 配对身份。

目前自动连接与配对筛选也锁定实测遥控器身份，见 `main/ble.cpp`；更换遥控器需调整目标身份并重新配对，本版尚未提供通用设备选择菜单。

当前 VID/PID 0xCAFE/0x4015 仅供本机开发原型，不代表已分配的商业 USB 标识。

## 诊断

`tools/diagnostics.py --port /dev/cu.usbmodem… --seconds 15` 读取有时限的状态，不主动开启麦克风。

- `S`：搜索这只遥控器（120 秒）。
- `X`：立即取消语音。
- `B`：进入 ROM 下载模式，供后续升级。
- `P`：仅在未录音时读取 LVGL 帧缓冲。输出 `FRAME 320 480 307200` 后为 RGB565 原始数据；不采集摄像头或麦克风。
- `D`：读取屏幕、背光及电源诊断。
- `L`：将背光恢复至 80%。
- `V`：停止语音，复位并初始化屏幕、重绘界面，恢复背光。

RGB565 已开启字节交换，截图原始数据按大端 16 位解释。启动时在外设初始化结束后执行屏幕恢复，避免程序正常运行但屏幕保持黑屏。

音频不写入 Flash，不走 Wi-Fi。仅在用户按住遥控器语音键或点击板载麦克风按钮时向电脑传送音频；未启用时 USB 音频为静音。

## 验证

主机逻辑测试覆盖右 Option 按下／松开、回车与方向键、连接时抑制已按住的按键、断连释放、语音源优先级、USB 重连、失去音频后的取消、快速切换隔离、队列溢出和异常数据包。测试不模拟真实 Opus 解码或板载音频硬件；这些必须实机检查。

```sh
clang++ -std=c++17 -fsanitize=address,undefined -g -Iesp32-siri-pad/tests/stubs -Iesp32-siri-pad/main esp32-siri-pad/main/pad.cpp esp32-siri-pad/tests/state_test.cpp -o /private/tmp/siri-pad-state-test
/private/tmp/siri-pad-state-test
```

实机验收状态见 `验收记录.md`。备份和现场录音位于原测试项目或本项目忽略目录，不应提交到公共仓库。

## 发布源码与字体

公开版本的中文字体为 Noto Sans SC 派生的 **SiriPadCJK** 20 px 字形子集，许可为 SIL OFL 1.1，见 `licenses/SiriPadCJK-OFL.txt`。生成后的 C 文件已包含，可直接构建；不发布个人系统字体、现场录音、Flash/NVS 备份或工具缓存。新布局固件已采用此公开字体子集。

项目仍处于实机验收阶段，尤其音频缓冲丢弃和电脑端按键操作尚未完整验收。蓝牙地址等硬件目标常量不是通用配置，不要把本机验证结果当作所有遥控器、电脑均已兼容。

USB 接口位置改变后，升级工具可显式指定 `--port <固件串口> --download-port <下载串口>`；升级前校验 USB 序列号，进入下载模式后再次核对芯片 MAC。

## 应用按钮与电脑状态

顶部 ChatGPT / Claude 按钮高亮只表示最近由 Pad 发出的应用快捷键，不代表电脑当前前台、隐藏或忙碌状态。输入法按钮只显示“切换”，不推测当前语言。没有实现忙碌检测、排队发送或松开自动回车；这些需要电脑端状态回传，当前仍是标准 USB 键盘与麦克风方案。

在每台目标 Mac 的“快捷指令”中给对应指令绑定上述快捷键，并在“系统设置 → 键盘 → 键盘快捷键 → 输入法”核对 Command＋空格。Pad 不会给其他 Mac 自动创建这些配置。

界面只在显示状态变化时重绘；独立监测发现界面心跳停滞超过 10 秒后会停止语音并重启。若发生这种恢复，USB 会短暂重连；该机制用于防止长期无响应，不代表底层卡住原因已完全消除。
