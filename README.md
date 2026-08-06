# DJIC

DJI 第一代 4G 模块的 CLI 工具，通过 USB（vendor `0x2ca3`，product `0x4006`）与模块上的蜂窝模组通信，提供对 DJI 4G 模块的底层访问能力：数据上网、通话与短信，以及 eSIM 管理。

基于 C++23（C++ modules）实现，依赖 vcpkg 与 CMake >= 4.0，UI 输出为中文。

## 功能

- **`data`** — 切换到数据上网模式并重启模块；重启后模块向系统暴露 USB 网卡，通过 DHCP 转发蜂窝流量。
- **`status`** — 显示模块状态（固件、ICCID、IMSI、运营商、信号、网络注册、工作模式等）。
- **`sms`** — 通话&短信模式：
  - 交互模式：短信轮询、`send`/`list`/`refresh`/`clear-me`/原始 `at` 指令，以及通过 Asterisk（WebSocket）拨号/接听/挂断/发消息。
  - `sms send <号码> <内容>`、`sms list`、`sms clear` 快捷子命令。
- **`esim`** — eSIM 基础能力：`eid` / `list`（Profile 列表）/ `switch` / `enable` / `disable` / `delete` / `apdu`。

## 构建

```sh
cmake --preset default          # 配置到 build/（Ninja）
cmake --build build             # 或 --parallel
ctest --test-dir build --output-on-failure
```

## 使用

```
djic <data|status|sms|esim>
```

示例：

```sh
djic status
djic data
djic sms                # 进入通话&短信交互模式
djic sms send +8613800138000 "你好"
djic esim list
djic esim switch 89049032020000000000
```

> 注意：主程序需要真实 DJI 硬件 + libusb，普通机器上无法运行；开发时请依赖测试。

## 模块布局

| 文件 | 导出的模块 |
|------|------------|
| `src/at.cppm` | `usb`（USB + AT 通道，DJI VID/PID） |
| `src/pdu.cppm` | `smscodec`（PDU 编码） |
| `src/modem.cppm` | `modem` |
| `src/sms.cppm` | `sms` |
| `src/esim.cppm` | `esim` |
| `src/asterisk.cppm` | `asterisk`（Boost.Beast WS 客户端） |

新增模块需在 `CMakeLists.txt` 的两个 `target_sources`（`DJIC` 与 `DJIC_tests`）中同时注册。

## 测试

Boost.UT，单一二进制 `build/DJIC_tests`，按测试套件运行：

```sh
ctest --test-dir build -R DJIC_at_tests
```

## References
- [DJOneHub](https://github.com/ZenGeekLabs/DJOneHub)

## License

[GPL-3.0](LICENSE.md)
