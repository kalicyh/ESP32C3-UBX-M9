# ESP32-C3 + u-blox M9 数据读取

ESP-IDF C 工程。默认 UART1 连接 GNSS，UART0 以 **115200、8N1** 输出 JSON 行和诊断日志。已在 ESP-IDF 6.1 编译，并完成 ESP32-C3 实机刷写及 UART GNSS / USB 输出验证。仓库默认配置仍为 UART0 控制台，原生 USB 输出配置见下文。


## 蓝牙网页仪表盘

使用 ESP32-C3 BLE（NimBLE），不启动 Wi-Fi 热点。GNSS 串口输出继续保留。网页由电脑或 HTTPS 静态站点提供，数据通过浏览器直接连接 C3 蓝牙读取。

1. 电脑启动网页：`python3 tools/serve_web.py`。
2. 使用 Chrome / Edge 打开 `http://127.0.0.1:8765/`，点击 **连接蓝牙**，选择 `UBX-M9-BLE-xxxxxx`。
3. 安卓使用支持 Web Bluetooth 的 Chrome，通过可信 HTTPS 打开相同网页文件，再点击连接。普通局域网 HTTP 地址不满足蓝牙安全上下文要求。网页文件位于 `main/web/`，可部署到静态 HTTPS 站点；本项目未替你发布公网网站。

本地服务器支持 `--host`、`--port`、`--cert`、`--key`；使用证书时必须得到客户端信任。Safari / iOS 不支持此网页连接方式。系统蓝牙列表不一定显示 BLE 外设，应从网页连接按钮中选择。

仪表盘包含有效定位状态、经纬度、高度、UTC、速度曲线、卫星天空图、信号强度表、局部轨迹、接收统计、原始帧及 JSON 下载。轨迹为相对首点的平面投影，不是道路地图。无效定位和过期数据不显示实时坐标。

每次快照接收完成后约 1 秒请求下一份；原始数据页才请求原始帧。卫星最多 128 颗，原始帧保留最近 8 条、每条最多 512 字节并标记截断。BLE 是限量快照，完整实际接收 UBX 帧仍从串口取得，不保证 BLE 全量流记录。

BLE 服务 UUID 为 `8c7b0001-6b8c-4a36-9f86-3ecfe03b6a91`；数据特征末段前缀 `8c7b0002`（Indicate），请求特征 `8c7b0003`（Write）。请求字节 1 为普通快照，2 包含原始帧。每包头 8 字节：版本、开始/结束标志、uint16 LE 序号、偏移、总长度；后接 UTF-8 JSON，最大 49152 字节。每包等待确认，网页验证分包顺序和完整性。

一次允许一个客户端。当前未配置配对认证或强制加密，附近可连接的 BLE 客户端能读取定位数据。断线后重新广播，网页点击连接可恢复。

## 接线

| 模块信号 | ESP32-C3 GPIO | 说明 |
| --- | --- | --- |
| RX | 2 | ESP TX → 模块 RX |
| TX | 1 | ESP RX ← 模块 TX |
| SCL | 3 | 可选 I²C 时钟 |
| SDA | 4 | 可选 I²C 数据 |
| GND | GND | 必须共地 |

默认只使用 UART；四根信号线可以接好，但程序一次只启用一种读取接口。GPIO2 是 C3 启动配置相关引脚，模块侧不要强行拉低影响启动。信号须为 3.3V 电平，模块供电按模块板规格连接。I²C 地址使用 7 位 `0x42`，100 kHz；SDA/SCL 应有外部上拉到 3.3V（例如 4.7 kΩ）。

电脑输出使用板载 USB-UART，或 USB-TTL 的 RX 接 GPIO21、GND 共地。GPIO1/2 是 GNSS 通道，不是电脑输出口。原生 USB 板请选择下方 USB 控制台配置。

## 编译、烧录

先进入 ESP-IDF 环境，再在项目目录执行：

```sh
idf.py build
idf.py -p PORT flash monitor
```

`PORT` 替换为实际串口名。工程已固定目标 `esp32c3`。当前机器可用环境：

```sh
export PATH=/opt/homebrew/bin:$PATH
. /Users/kalicyh/.espressif/v6.1/esp-idf/export.sh
```

配置入口：`idf.py menuconfig` → `UBX-M9 reader`。

- 默认 GNSS UART 初始 38400，使用 MON-VER 校验响应扫描常见波特率；不修改模块波特率或保存 Flash 配置。
- I²C：选择 `I2C DDC`。独立构建可用：

```sh
idf.py -B build-i2c -D SDKCONFIG=build-i2c/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.i2c' build
```

- 原生 USB（GPIO18/19）：在 ESP-IDF 控制台设置中选择 `USB Serial/JTAG Controller`，或使用独立构建：

```sh
idf.py -B build-usb -D SDKCONFIG=build-usb/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb' build
idf.py -B build-usb -p PORT flash monitor
```

已有 sdkconfig 优先于 defaults，切换现有构建请使用 menuconfig。默认主输出为 UART0；USB 副控制台可能丢弃数据，长期采集原生 USB 请使用 USB 主控制台构建。

## 数据范围和输出

这里的“全部数据”指读取下列可轮询数据，并保留**每条实际收到且校验正确的 UBX 消息的完整二进制帧**，不是宣称 M9 的所有固件都支持所有消息，也不是所有字段都已转为中文或独立 JSON 字段。

- NAV：PVT、SAT、SIG、DOP、STATUS、POSECEF、POSLLH、VELECEF、VELNED、各 GNSS 时间、UTC、CLOCK、TIMELS、ORB、COV、SBAS、SLAS、GEOFENCE、ODO、AOPSTATUS。
- MON：VER、COMMS、GNSS、HW、HW2、HW3、RF、SPAN、PATCH。
- PVT、SAT、DOP、VER 额外解码为 JSON；其他消息及未知消息以 `frame_hex` 完整返回，含同步头、长度和校验和。
- 可选请求 RXM-RAWX/SFRBX 周期输出。是否支持由模块型号及固件决定，检查 CFG-MSG 的 ACK/NAK 和后续实际消息；ACK 不代表已有有效观测数据。此选项会改变模块当前消息输出配置，但不主动保存到 Flash。重启 ESP 不会自动撤销模块配置；需要时关闭模块周期输出或恢复模块配置。

不会发送复位、清除星历或重置里程等命令。不读取所有 CFG 配置项、日志数据库和辅助数据存储；不启用所有事件型消息。NMEA 文本不转发，因为本项目以 UBX 为数据接口。模块必须允许所选端口的 UBX 输入和输出，若此前禁用需先恢复。

输出格式：

- `UBX`：class/id 为十进制；`frame_hex` 可无损还原 UBX 帧。最大 payload 8192 字节，超过上限计入 oversize。
- `PVT`：经纬度（度）、椭球高/海拔（米）、N/E/D 和地速（米/秒）、航向（度）、精度、UTC、卫星数、fix_type、fix_ok、valid_flags。UTC 字符串只是字段表示，必须检查 valid_flags 的日期/时间有效位；位置必须检查 fix_ok、fix_type 和 flags3 的 invalidLlh。车辆航向需检查 flags 中 headVehValid。
- `SAT`：每颗卫星的星座编号、卫星编号、载噪比、仰角、方位角、伪距残差和状态位。通过 iTOW_ms 关联同一历元。
- `DOP`：几何、位置、时间、垂直、水平、北向、东向精度因子。
- `VERSION`：软件、硬件及扩展版本文本。
- `ACK/NAK`：配置接受或拒绝。其他日志不属于 JSON，电脑程序只解析以 `{` 开头的行。

PVT 尽量每约 1–2 秒轮询一次；其余消息轮转读取，一轮耗时取决于响应和超时，不保证同历元同步快照。超时只表示未收到匹配响应，不能据此断定模块不支持或没有定位。接收与串口输出分任务，输出拥塞时丢弃整条输出并增加 output_drop，避免阻塞接收；不保证任意波特率、任意消息流量下无丢包。高流量 RAWX 建议使用原生 USB 主输出并监控统计。

每 10 秒输出累计 checksum、oversize、output_drop、io_errors、uart_overflow。持续无响应会报告并重试。无定位时仍返回模块报告的有效性标志，不生成模拟坐标。

## 验证

主机协议测试（无需 ESP-IDF）：

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/ubx.c tests/test_ubx.c -o /tmp/esp32c3-test-ubx
/tmp/esp32c3-test-ubx
```

覆盖已知校验向量、逐字节拆包、同步噪声、坏校验后恢复、超长帧、最大长度、连续帧和有符号数。

实机验收：确认出现 VERSION，户外接天线后观察 fix_ok、经纬度、SAT；确认统计无持续增长的丢包/错误。2026-10-12 实机刷写校验通过，读取到 SPG 4.04 / PROTVER 32.01，以及 PVT、SAT、DOP、VERSION。约 18 秒采样期间，校验错误、超长帧、输出丢包、I/O 错误和 UART 溢出累计统计均为 0；部分 MON 消息轮询超时。当时 fix_ok=false、参与定位卫星数为 0，坐标和 UTC 不可用。尚未验证有效定位、I²C 实机通信及长期高流量稳定性。

协议依据：[u-blox M9 SPG 4.04 Interface Description](https://content.u-blox.com/sites/default/files/u-blox-M9-SPG-4.04_InterfaceDescription_UBX-21022436.pdf)。串口输出选择参考 [ESP-IDF USB Serial/JTAG Console](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-guides/usb-serial-jtag-console.html)。

### 网页相关验证

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I tests/host -I main main/web_codec.c main/gnss_web.c main/ubx.c tests/test_web.c -o /tmp/esp32c3-test-web
/tmp/esp32c3-test-web
```

测试覆盖 JSON 转义、容量边界、最大卫星数、原始帧截断。`tests/host` 为单线程主机测试，不代替 FreeRTOS 并发验证。

```sh
node tests/test_ble_transport.cjs
```

蓝牙分包测试覆盖 UTF-8 跨包、最小 MTU 分包、缺包、乱序、长度限制和结束标志。`tests/browser_checks.js` 用于 Playwright CLI，使用明确的合成 BLE 数据检查页面；不代表真实定位结果。真实无线连接、有效 GNSS 定位和长期运行需分别验收。
