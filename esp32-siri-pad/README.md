# Siri Voice Pad（ESP32-S3 触屏版）

硬件：Waveshare ESP32-S3-Touch-LCD-3.5，物理面板 320×480、横屏逻辑画布 480×320、16 MB Flash、8 MB PSRAM；已测试的遥控器为 Apple Siri Remote A2854。

本版将原 5 寸 Pad 的博朗风格首页和设置功能移植到 3.5 寸横屏。当前首页设计稿在 `design/weekly-quota-landscape.html`；旧截图 `assets/pad-layout.png` 仅记录上一版布局。

## 操作

首页左侧从上到下显示 ChatGPT、Claude、Chrome，右侧为橙色板载麦克风；下方依次为左箭头、空格、大回车、右箭头。ChatGPT 卡片标注 **Codex 每周剩余**，Claude 卡片标注 **Claude 每周剩余**，避免把两者当作同一额度。底栏显示息屏时间。点右上角“设置”可打开蓝牙、按键和息屏三个页面。

额度来自所连接的 Mac，不保存在 Pad 上：电脑端通过现有 USB CDC 诊断接口发送 `Q <Codex每周剩余> <Claude每周剩余>\n`，各值为 0–100 或 `-1`（未知）。Pad 只接受合法范围；超过 15 分钟未收到更新会显示 `--`，不会保留过期数字。额度更新不计入屏幕的交互活动，因此不会阻止自动息屏。屏幕设计图中的 63%／93% 只是排版示例，不作为设备默认值。

屏幕相对上一版竖屏逆时针旋转 **90°**，使用 480×320 横屏。FT6336 保持原始 320×480 坐标，LVGL 与画面一同变换触摸坐标，避免重复旋转。

- 遥控器侧面语音键：按住发送右 Option，同时将遥控器麦克风声音送入 USB 麦克风；松开停止。
- 遥控器圆盘中央确认键：回车。
- 遥控器减号／加号键：左／右方向键，按下保持、松开释放。
- 遥控器播放／暂停键：键盘空格，按下保持、松开释放。
- 遥控器圆环左键／屏幕 ChatGPT：发送左 Control＋左 Option＋左 Command＋G，调用电脑上已有的唤起／隐藏快捷指令。
- 遥控器圆环右键／屏幕 Claude：发送左 Control＋左 Option＋左 Command＋C。
- 遥控器小电视键／屏幕输入法：发送左 Command＋空格，切换输入法。
- 屏幕下方左右箭头：移动光标；空格和回车按钮分别发送对应键。Chrome 按钮发送 Control＋Option＋Command＋B。
- 三种组合快捷键每次按下只发一次完整的按下／释放；遥控器需全部松开后才接受下一次动作。切换应用或输入法会先停止语音、释放旧按键，避免右 Option 混入组合键。
- 屏幕麦克风：点击开启板载麦克风并按下右 Option，再点停止并释放右 Option。
- 遥控器语音键优先于板载麦克风。切换到遥控器后，松开不会自动恢复板载录音。
- 蓝牙设置显示连接状态、保存的遥控器身份，可搜索附近 Apple TV 遥控器并点选连接，也可重连、断开或移除配对。开机自动重连保存的遥控器。首次连接后的首个按下用于唤醒，收到松开后才允许发送按键，避免连接瞬间意外回车。
- 按键设置可以修改 8 个已识别遥控器按键的键盘映射和组合键；语音键还能设置左／右 Option。映射保存在设备 NVS。
- 息屏默认 30 分钟，可选 1、5、15、30、60 分钟或永不。无触摸、遥控器按键、连接状态变化或语音输入时关闭背光；第一次触摸先唤醒屏幕。
- USB 断开／挂起、遥控器断连、音频停止超过一秒时释放语音按键。重新连接不会自动恢复录音。

USB 提供标准 HID 键盘与 UAC2 单声道 48 kHz／16 bit 麦克风，另带 CDC 诊断接口。Mac 的输入设备名称为 **Siri Voice Pad Microphone**。电脑会自动识别设备；输入法是否使用它，取决于系统或应用选中的输入设备，以及右 Option 的快捷键配置。

## 构建

使用 PlatformIO `espressif32@6.12.0`、ESP-IDF 5.5.0，组件版本见 `main/idf_component.yml` 和 `dependencies.lock`。不是 Arduino 固件。应用图标由 `design/icons/` 中的 SVG 通过 `tools/generate_icons.py` 转换为本机 RGB565；生成后的 `main/app_icons.c` 已包含，无需安装图像工具即可构建。

从仓库根目录执行（Python 3、PlatformIO Core）：

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install platformio pyserial esptool
pio run --project-dir esp32-siri-pad
```

生成文件在 `esp32-siri-pad/.pio/build/pad/`。首次安装在开发板进入下载模式后，可用 `pio run --project-dir esp32-siri-pad --target upload --upload-port <下载串口>`；先确认目标板和备份，再刷入。引导程序位于 0x0、分区表 0x8000、应用 0x10000，NVS 位于 0x9000。不要执行全片擦除。

已安装本固件且分区表未改变的实测设备可运行 `python esp32-siri-pad/tools/upgrade.py` 更新应用分区。**该工具当前锁定开发者的板子 MAC 和两个 macOS 串口路径**，移植到其他设备需先核实并修改参数；不会自动选择其他板子。它通过 CDC 进入下载模式，核对 MAC，以 no-reset 方式刷写，退出时使用看门狗和 USB 串口复位。升级不写 NVS。蓝牙配置保留两个通用 MAC 地址，以沿用原 Arduino 配对身份。

如果设备曾刷入其他固件，先备份当前 Flash，并核对分区表。其他项目可能采用不同的 OTA 布局，不能直接运行上述“只刷应用”工具。本工程完整恢复地址为 bootloader `0x0`、分区表 `0x8000`、应用 `0x10000`。本次实机此前使用 OTA 双分区，恢复时仅清理了新 NVS 和 otadata 范围 `0x9000–0x10000`；完整旧 Flash 备份留在本机忽略目录 `backups/`。

出厂默认自动连接先前实测遥控器；蓝牙设置页可以搜索并选择新的 Apple TV 遥控器，其身份保存至 NVS。搜索列表只接收 HID 服务且带 Apple 厂商数据的广播。

当前 VID/PID 0xCAFE/0x4015 仅供本机开发原型，不代表已分配的商业 USB 标识。

## 诊断

`tools/diagnostics.py --port /dev/cu.usbmodem… --seconds 15` 读取有时限的状态，不主动开启麦克风。

- `S`：搜索这只遥控器（120 秒）。
- `X`：立即取消语音。
- `B`：进入 ROM 下载模式，供后续升级。
- `P`：仅在未录音时读取 LVGL 帧缓冲。输出 `FRAME 480 320 307200` 后为 RGB565 原始数据；不采集摄像头或麦克风。
- `D`：读取屏幕、背光及电源诊断。
- `L`：将背光恢复至 80%。
- `V`：停止语音，复位并初始化屏幕、重绘界面，恢复背光。
- `Q <Codex> <Claude>\n`：更新两个每周剩余百分比；`-1` 表示当前不可用。

RGB565 已开启字节交换，截图原始数据按大端 16 位解释。启动时在外设初始化结束后执行屏幕恢复，避免程序正常运行但屏幕保持黑屏。

音频不写入 Flash，不走 Wi-Fi。仅在用户按住遥控器语音键或点击板载麦克风按钮时向电脑传送音频；未启用时 USB 音频为静音。

## 验证

主机逻辑测试覆盖右／左 Option 按下／松开、回车与方向键、连接时抑制已按住的按键、断连释放、语音源优先级、USB 重连、失去音频后的取消、快捷键及映射变更、队列溢出和异常数据包。测试不模拟真实 Opus 解码或板载音频硬件；这些必须实机检查。

```sh
clang++ -std=c++17 -fsanitize=address,undefined -g -Iesp32-siri-pad/tests/stubs -Iesp32-siri-pad/main esp32-siri-pad/main/pad.cpp esp32-siri-pad/tests/state_test.cpp -o /private/tmp/siri-pad-state-test
/private/tmp/siri-pad-state-test
```

实机验收状态见 `验收记录.md`。备份和现场录音位于原测试项目或本项目忽略目录，不应提交到公共仓库。

## 发布源码与字体

公开版本含 Noto Sans SC 派生的 **SiriPadCJK** 20 px 旧界面字形子集，以及本版新增的 Noto Sans SC 17 px 设置页字形子集，许可均为 SIL OFL 1.1，见 `licenses/SiriPadCJK-OFL.txt`。生成后的 C 文件已包含，可直接构建；不发布个人系统字体、现场录音、Flash/NVS 备份或工具缓存。

项目仍处于实机验收阶段，尤其音频缓冲丢弃和电脑端按键操作尚未完整验收。蓝牙地址等硬件目标常量不是通用配置，不要把本机验证结果当作所有遥控器、电脑均已兼容。

USB 接口位置改变后，升级工具可显式指定 `--port <固件串口> --download-port <下载串口>`；升级前校验 USB 序列号，进入下载模式后再次核对芯片 MAC。

## 电脑端每周额度桥接

`tools/weekly_quota_bridge.py` 在 Mac 上通过本地 `codex app-server` 的 `account/rateLimits/read` 读取 Codex 限额，按 `windowDurationMins == 10080` 识别七天窗口，不假设它一定在 `primary` 或 `secondary`。Claude Code 的 `statusLine` 接收官方 JSON，`tools/claude_statusline.py` 仅提取 `rate_limits.seven_day.used_percentage`，只缓存百分比与时间，不保存对话内容。Claude Code 的额度字段在会话首次 API 响应前可能缺失；Claude 桌面版的“用量”页面本身也不会调用 CLI 的 `statusLine`。若超过一小时没有收到 Claude Code 新状态，Pad 显示 `--`，而非沿用旧数值。

Mac 安装：在 `esp32-siri-pad/.venv` 安装 `pyserial==3.5`，运行 `tools/install_weekly_quota_bridge.py`。安装器在 `~/.claude/settings.json` 尚无 `statusLine` 时添加该 hook，先备份原文件，并创建登录自启的 LaunchAgent `local.siri-voice-pad.weekly-quota`。若已有自定义 `statusLine`，安装器会停止，不覆盖；需在现有脚本中调用缓存逻辑。桥接器只连接序列号 `1020BA4658B4-PAD1` 的本机 Pad，USB 断开后自动寻找并重连。当前屏幕数据由标准 USB CDC 发送，键盘和麦克风仍独立枚举。电脑换成其他 Mac 时，也要在那台 Mac 配置桥接器才能显示其账户额度。

## 应用按钮与电脑状态

ChatGPT / Claude 按钮只发送对应快捷键，不代表电脑当前前台、隐藏或忙碌状态。每周额度也不表示应用正在运行。没有实现忙碌检测、排队发送或松开自动回车；这些需要额外的电脑端状态回传。输入法切换仍由遥控器小电视键发送系统快捷键。

在每台目标 Mac 的“快捷指令”中给对应指令绑定上述快捷键，并在“系统设置 → 键盘 → 键盘快捷键 → 输入法”核对 Command＋空格。Pad 不会给其他 Mac 自动创建这些配置。

界面只在显示状态变化时重绘；独立监测发现界面心跳停滞超过 10 秒后会停止语音并重启。若发生这种恢复，USB 会短暂重连；该机制用于防止长期无响应，不代表底层卡住原因已完全消除。
