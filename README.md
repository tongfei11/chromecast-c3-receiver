# Chromecast C3 Receiver

通过 ESP32-C3 连接 Google TV / Chromecast 蓝牙遥控器，并通过 Arduino `Serial` 输出 HID 通知。固件使用 Arduino-ESP32 自带的 BLE 库，通过 HID 报告句柄 `002b` 转发按键。支持 ATVV 的遥控器还可通过串口转发控制和压缩音频通知，由主机负责发起协议命令、解码与音频输出。

## 配置目标遥控器

用手机或电脑上的 BLE 扫描工具查找遥控器。按住遥控器的「返回 + 主页」键，观察该设备在配对广播中显示的地址。将串口终端设为 `115200` 并启用换行发送。将下列命令里的示例地址换成实测地址。

```text
target aa:bb:cc:dd:ee:ff
```

收到 `TARGET_SAVED rebooting=1` 后，接收器会自动重启。必要时重新打开串口。发送 `target` 可确认已保存的地址。首次配对时，再让遥控器进入配对模式并发送 `p`。

从源码构建时，也可在 `firmware/RemoteReceiver/` 创建 `ReceiverConfig.local.h` 来设定出厂默认值。

```cpp
#pragma once
#define RECEIVER_TARGET_MAC "aa:bb:cc:dd:ee:ff"
// 可选。在 BLE 初始化时使用的接收器名称。
// #define RECEIVER_DEVICE_NAME "Chromecast C3 Receiver"
// 可选。覆盖 UART0 波特率时，主机侧也要使用同一值。
// #define RECEIVER_SERIAL_BAUD 115200
```

本地配置会被 Git 忽略。串口保存的地址写入板上的 NVS，重启后保留，并优先于编译默认值。未设置目标时，固件输出 `CONFIG_REQUIRED` 并等待串口配置。地址格式为六组以冒号分隔的两位十六进制数。

## 刷写预编译固件

从 [GitHub Releases](https://github.com/tongfei11/chromecast-c3-receiver/releases/latest) 下载对应的 `.merged.bin`。固件面向配有至少 4 MB Flash 的 ESP32-C3，使用 DIO、40 MHz 和 `huge_app` 分区。

| 文件名后缀 | 运行时串口 |
| --- | --- |
| `uart0.merged.bin` | 板载或外接 USB 转 UART 桥接，默认 `115200` |
| `usb.merged.bin` | 芯片原生 USB Serial/JTAG 接口 |

将 `<PORT>` 换成 ESP32-C3 的串口，将 `<FIRMWARE>` 换成下载的镜像文件名。以下命令使用 [esptool 4.x](https://docs.espressif.com/projects/esptool/en/release-v4/esp32c3/esptool/basic-commands.html#write-binary-data-to-flash-write-flash)。

```sh
python -m pip install "esptool>=4.5.1,<5"
python -m esptool --chip esp32c3 --port "<PORT>" write_flash 0x0 "<FIRMWARE>"
```

图形刷写工具也可在 `0x0` 写入此合并镜像。如果需要进入下载模式，按住 BOOT，按下并松开 RESET，再松开 BOOT。完成后重置开发板并检查串口设备名。

刷入合并镜像会清除该范围内的 NVS 设置和 BLE 绑定，刷写后需按上节配置目标地址。使用相同分区布局升级并希望保留配置时，下载名称以 `components.zip` 结尾的压缩包，进入对应的 `uart0` 或 `usb` 目录，执行以下命令。

```sh
python -m esptool --chip esp32c3 --port "<PORT>" write_flash @flash_args.txt
```

## 从源码编译与烧录

以下 Arduino CLI 命令使用 Arduino-ESP32 `2.0.14` 与通用 ESP32-C3 板卡。也可在 Arduino IDE 选用开发板对应的板型。使用专用板型时，串口选项以该板型菜单为准。

```sh
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@2.0.14 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli board list
```

将下列命令的 `<PORT>` 替换成 `arduino-cli board list` 看到的端口。如果开发板有多个 USB 接口，选择对应的接口。

### USB 转 UART 桥接

使用 `CDCOnBoot=default`，固件通过 UART0 发送日志。将板载或外置 USB 转串口桥接到 UART0，默认波特率为 `115200`，语音通过二进制帧发送。

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=default,FlashMode=dio,FlashFreq=40,FlashSize=4M,PartitionScheme=huge_app --output-dir build/uart firmware/RemoteReceiver
arduino-cli upload --fqbn esp32:esp32:esp32c3:CDCOnBoot=default,FlashMode=dio,FlashFreq=40,FlashSize=4M,PartitionScheme=huge_app --port "<PORT>" --input-dir build/uart firmware/RemoteReceiver
```

### ESP32-C3 原生 USB

板卡的 USB 数据线连接到芯片原生 USB 接口时，使用 `CDCOnBoot=cdc`，固件通过原生 USB 串口发送日志。

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,FlashFreq=40,FlashSize=4M,PartitionScheme=huge_app --output-dir build/usb firmware/RemoteReceiver
arduino-cli upload --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,FlashFreq=40,FlashSize=4M,PartitionScheme=huge_app --port "<PORT>" --input-dir build/usb firmware/RemoteReceiver
```

如果烧录器需要下载模式，按住板卡的 BOOT 键，按下并松开 RESET 后再松开 BOOT。完成烧录后重置开发板，重新检查串口设备名。

## 配对与串口协议

启动后，固件会扫描已配置的遥控器，并优先使用已保存的 BLE 绑定连接。首次配对或更换绑定时，让遥控器进入配对模式，然后向接收器发送 `p`。此命令会断开当前连接，只删除已配置地址的本地绑定，再扫描并尝试配对。若找不到目标，扫描约一分钟后返回失败，可再次发送 `p`。

| 命令 | 行为 |
| --- | --- |
| `target` | 查询目标遥控器地址，需以 CR 或 LF 结尾 |
| `target MAC` | 保存目标地址并自动重启，需以 CR 或 LF 结尾 |
| `p` | 重新配对目标设备 |
| `s` | 查询连接、认证、订阅就绪和配对状态 |
| `x` | 停止扫描、取消配对并断开连接 |
| `tx HEX` | 将 1–32 字节连续十六进制数据写入 ATVV 命令特征，必须以 CR 或 LF 结尾 |

`p`、`s`、`x` 可用单个 ASCII 字符发送，允许以 CR/LF 结尾。串口日志逐行输出，其中 `ms` 为设备启动后的毫秒数，`handle` 和 `hex` 为十六进制。常用输出如下。

```text
BOOT RemoteReceiver build=0007
PAIRING_STARTED target=aa:bb:cc:dd:ee:ff
VOICE_AVAILABLE enabled=1
READY build=0007
PAIRING_SUCCEEDED target=aa:bb:cc:dd:ee:ff
STATUS build=0007 connected=1 authenticated=1 ready=1 pairing=0 voice=1
REPORT ms=1234 handle=002b len=2 hex=0000
TX_ACCEPTED len=6
ATVV kind=control len=2 hex=0000
DISCONNECTED
```

`PAIRING_FAILED reason=...` 表示本次配对失败，例如 `target_not_configured`（缺少目标地址）、`target_invalid`（地址格式错误）、`target_not_found`（未找到目标广播）或 `authentication_failed`（认证失败）。`PAIRING_BUSY` 表示已有配对任务。

`VOICE_AVAILABLE` 表示当前连接发现了可读写的 ATVV 特征；`READY` 表示按键链路已就绪。`TX_ACCEPTED` 表示 BLE 协议栈接受写入请求，遥控器是否响应由主机通过 ATVV 通知判断。缓冲空间不足时会优先保留按键、控制与连接事件，`ATVV_DROPPED audio=N priority=N` 报告该次输出前累计丢弃的数量。

### 音频二进制帧

控制通知通过 ASCII `ATVV kind=control` 行发送。音频通知使用下列二进制帧。文本行和二进制帧按 BLE 事件顺序发送。二进制帧不附加换行符。

| 字段 | 字节 | 格式 |
| --- | --- | --- |
| 帧头 | 2 | `A5 5A` |
| 类型 | 1 | `01` 表示音频 |
| 音频长度 | 2 | 小端序，最大 244 字节 |
| 音频数据 | N | 原始 ATVV 通知 |
| CRC | 2 | 小端序，CRC-16/CCITT-FALSE，初值 `FFFF`，多项式 `1021`，从类型字段计算到音频数据末尾 |

仓库源码按 [MIT 许可证](LICENSE) 授权。预编译固件使用 [Arduino-ESP32 2.0.14 源码与板卡组件](https://github.com/espressif/arduino-esp32/tree/2.0.14)，各组件适用其上游许可证。

## 验证范围

- 固件 `0006` 已在 ESP32-C3 开发板的 UART0 通道实测，使用 `115200`。
- VID `18D1` / PID `9450` 的 Chromecast 兼容遥控器已验证 14 个普通键的按下与松开。
- ATVV v1.0 / ADPCM 16 kHz 已通过 C3 → 串口 → Mac 解码得到实际语音，并完成微信输入法识别测试。
- 原生 USB 配置已编译验证，对应开发板仍需实机验收。

按键报告句柄当前为 `002b`。更换不同遥控器固件时，需要核对该句柄及主机侧的按键编码映射。ATVV 语音支持以实际特征、能力协商及音频测试为准。
