# 5 寸 P4 Pad 横屏测试固件

这是 ESP32-P4-WIFI6-Touch-LCD-5（已探测芯片 revision v3.2）的**界面与按键设置测试版**。使用微雪 LCD5 BSP、HX8394 DSI、GT911 触控和 LVGL 9，在屏幕上构建 1280×720 横屏主页与设置页。

蓝牙连接页目前显示真实的“未连接／驱动未接入”状态；搜索、重连等按钮只给出不可用提示。USB 键盘、USB 麦克风和遥控器音频也尚未移植，主页按钮只显示提示。这个版本用于验证屏幕、触控、中文字体、页面导航及按键设置保存，不能替代旧 3.5 寸 Pad 的工作固件。

按键映射页列出 13 个遥控器实体按键。每项可配置单键／组合快捷键、遥控器麦克风语音动作或无动作；触屏上可选择 Ctrl、Option、Command、Shift 和主键。保存后写入本板 NVS 的 `siri_pad/map_v1`；重新启动会读取，数据不合法时使用默认映射。恢复默认需要连续点按两次。此设置数据尚未连接到 BLE 和 USB HID 发送流程。

原有工厂分区表通过读回前 64 KiB 解析，`partitions.csv` 保持相同名称、偏移和大小。测试只允许在备份完成并确认目标 MAC `80:f1:b2:d5:c8:7f` 后，把**应用镜像**写入 `factory` 分区 `0x200000`；不得擦除整片、覆盖 bootloader／分区表／NVS／模型／存储分区。旧 S3 Pad 序列号 `1020BA4658B4-PAD1` 是另一台设备，不能用于本固件。

构建：在 ESP-IDF 5.5.5 环境下，运行 `idf.py -B build/rev3_x_555 -D SDKCONFIG=build/rev3_x_555/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rev3_x' build`。依赖版本见 `main/idf_component.yml` 和 `dependencies.lock`。当前开发环境已编译通过，固件 `build/rev3_x_555/siri_pad_p4.bin` 约 855 KiB，位于 8 MiB 应用分区内。镜像标明 ESP32-P4 最低 revision v3.0。

字形子集由 LVGL 9 附带的 Source Han Sans SC 字体生成，字体许可在 `licenses/SourceHanSansSC-OFL.txt`。重新生成需先让组件管理器取得 LVGL 9.5，并安装 `lv_font_conv` 1.5.3，运行 `python tools/generate_font.py /path/to/lv_font_conv`。
