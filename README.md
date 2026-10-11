# ESP32-S3 USB Host ADB — 自动开启安卓无线调试

用 **ESP32-S3 作为 USB Host**，完全手写 **ADB 协议**，插入安卓手机后**自动完成 RSA 认证**并下发
`adb tcpip 5555`，打开手机的无线调试端口。烧录一次后，**仅"板子供电 + 手机插 OTG"即可自动执行，全程无需电脑**。

> 与常见的"ESP32 模拟 HID 键盘打字"方案不同：本方案让手机作为 **USB Device**、ESP32 作为 **USB Host**，
> 由 ESP32 主动握有控制权执行命令，而不是去"模拟打字"。

---

## 目录

- [特性](#特性)
- [工作原理](#工作原理)
- [硬件要求](#硬件要求)
- [引脚接线](#引脚接线)
- [快速开始](#快速开始)
- [使用说明 (Usage)](#使用说明-usage)
- [密钥管理与生成](#密钥管理与生成)
- [故障排查 (FAQ)](#故障排查-faq)
- [安全说明](#安全说明)
- [目录结构](#目录结构)
- [License](#license)

---

## 特性

- ✅ **独立运行**：板子供电 + 手机插 OTG 即自动执行，全程无需电脑
- ✅ **手写 ADB 协议**：USB Host 枚举 → CDC-ACM → Bulk → ADB 状态机
- ✅ **RSA 认证自动完成**：`CNXN` / `AUTH(TOKEN)` / `AUTH(SIGNATURE)` / `AUTH(RSAPUB)`
- ✅ **自动下发 `tcpip 5555`**，开启手机无线调试
- ✅ **认证通过后拔掉 ESP32 也能远程连接**（手机重启 / 重插前有效）

## 工作原理

```
┌─────────────────────────────────────────────┐
│  应用层   下发 "tcpip 5555" 打开无线调试      │
├─────────────────────────────────────────────┤
│  ADB协议层  CNXN/AUTH/OPEN/WRTE 包 + RSA认证 │
├─────────────────────────────────────────────┤
│  USB逻辑层  枚举手机→找CDC-ACM→拿Bulk端点     │
├─────────────────────────────────────────────┤
│  USB物理层  ESP32供VBUS 5V，D+/D-(G19/20)    │
└─────────────────────────────────────────────┘
```

- **USB 规则：谁向外供电谁是 Host**。ESP32 经 GPIO4→MOS 管向手机输出 5V，手机被切换为 Device，
  其 `adbd`（ADB 守护进程）作为 USB 设备暴露出来。
- **枚举**：`usb_host` 库自动枚举手机，遍历配置描述符，找到 `adbd` 使用的 CDC-ACM 接口及 Bulk IN/OUT 端点。
- **ADB 认证**：`adbd` 发 20 字节随机 challenge → ESP32 用私钥做 RSA 签名返回 → `adbd` 用公钥验签 →
  手机弹窗授权（首次需勾选"始终允许"）→ OKAY 放行。
- **下发命令**：认证通过后打开 `shell:` 流，向流内写入 `tcpip 5555\n`，`adbd` 执行后在 `0.0.0.0:5555`
  起 TCP 监听，USB 传输切换为网络传输。

## 硬件要求

- **GOOUUU ESP32-S3 N16R8 v3**（原生 USB OTG PHY，D+/D- 固定于 GPIO19/20）
- 安卓手机（本项目实测 **华为 P40 ANA-AN00，HarmonyOS 4.2.0.121**）
- 5V 供电（充电头 / 充电宝 / 电池）
- 可选：N-MOS 管（VBUS 受控输出）、OTG 转接头 / 线

## 引脚接线

| 信号 | ESP32-S3 引脚 | 说明 |
|------|---------------|------|
| D+ | GPIO20 | 芯片原生 USB PHY |
| D- | GPIO19 | 芯片原生 USB PHY |
| VBUS 使能 | GPIO4 | 经 N-MOS 输出 5V 给手机（Host 必需） |
| VBUS 5V | GPIO4 → N-MOS → 手机 VBUS | 手机供电 |
| GND | 与手机共地 | 必须共地 |

> ⚠️ **接手机必须走 OTG 口**（只有它接到芯片原生 USB）；**TTL 口**仅用于烧录 / 串口日志 / 板子供电。
> 板子自身供电走 **TTL 口** 或 **V5IN 引脚**，与接手机的 OTG 口互不冲突。

## 快速开始

```bash
# 1. 生成你自己的密钥（必做，仓库只含 DEMO 占位密钥）
pip install cryptography
python gen_key.py

# 2. 编译
idf.py build

# 3. 烧录（板子 TTL 口连电脑）
idf.py -p COM口 flash
```

烧录完成后，给板子供电、把手机插到 OTG 口，ESP32 自动完成 ADB 认证并开启手机无线调试。

## 使用说明 (Usage)

### 启动步骤（先板子，后手机）

1. **烧录**：TTL 口连电脑，`idf.py -p COM口 flash`，成功后板子自动重启运行。
2. **板子供电**：TTL 口或 V5IN 接 5V 电源。串口（monitor）应打印：
   ```
   I (338) ADB: ESP32-S3 USB Host ADB 启动
   I (1378) ADB: 打开 VBUS (GPIO4) 给手机供电
   ```
3. **手机准备**：
   - 开发者选项 → **USB 调试** 开启
   - 设置 → 更多连接 → **OTG 连接** 开启
4. **接线**：手机用 OTG 转接头 / 线接到板子 **OTG 口**，手机由 ESP32 供 VBUS 供电。
5. **首次授权**：手机弹窗 **"允许 USB 调试 / RSA 授权"** → 勾选 **"始终允许"** → 确定（只此一次）。
   USB 用途需选 **传输文件**（不能仅充电）。
6. **自动执行**：ESP32 自动完成 ADB 握手并下发 `tcpip 5555`。monitor 中应看到状态机推进到
   `命令执行完成，任务 DONE`。

### 验证无线调试已开启

手机已开 5555 后（**此时可拔掉 ESP32**）：

```bash
# 电脑 / 另一台手机，与目标手机同一 WiFi
adb connect 手机IP:5555
adb shell
```

### 运行过程串口日志参考

```
ESP32-S3 USB Host ADB 启动
打开 VBUS (GPIO4) 给手机供电
NEW_DEV addr=x            ← 手机被识别
[TX] cmd=0x... (CNXN)
[RX] cmd=0x... (AUTH token)
[TX] cmd=0x... (AUTH signature)
[RX] cmd=0x... (OKAY)
...
[TX] cmd=0x... (WRTE "tcpip 5555")
命令执行完成，任务 DONE
```

## 密钥管理与生成

固件需要一对 RSA 密钥：私钥在 ESP32 内签名，公钥随 `CNXN` 发给手机验签。**请勿提交真实私钥。**

> 仓库中的 `adb_auth.c` 仅含 **DEMO 占位密钥**（公开无害，方便直接编译）。**使用前必须运行 `gen_key.py`**
> 生成你自己的密钥。

### 生成自己的密钥（推荐）

```bash
pip install cryptography   # 首次
python gen_key.py          # 生成并自动写入 adb_auth.c
idf.py build && idf.py -p COM口 flash
```

`gen_key.py` 会：

1. 生成 RSA 2048 密钥对
2. 私钥导出为 PKCS#8 PEM，公钥导出为 OpenSSH 单行 `ssh-rsa`
3. 自动替换 `main/adb_auth.c` 中的 `adb_private_key_pem[]` 与 `adb_openssh_pubkey[]`
4. 密钥文件存到 `keys/`（已被 `.gitignore` 忽略）

> 每次重新生成后，手机需在弹窗中重新授权一次。

### 使用系统 openssl 生成（备选）

```bash
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out keys/id_rsa.pem
openssl pkey -in keys/id_rsa.pem -pubout -outform PEM | openssl ssh-keygen -i -m PKCS8
# 然后把 PEM 私钥和 ssh-rsa 公钥填入 adb_auth.c
```

## 故障排查 (FAQ)

**Q1：手机插入后串口没有任何 `NEW_DEV`，也没反应？**
- 检查 VBUS：GPIO4→N-MOS→手机 VBUS 是否供电成功；手机不供电无法枚举。
- 确认接的是 **OTG 口**（D+/D- 在 GPIO19/20），不是 TTL 口。
- 确认手机已开启 **USB 调试 + OTG 连接**，USB 用途选 **传输文件**。

**Q2：卡在 `AUTH` 阶段，没有 OKAY？**
- 首次连接需在手机弹窗勾选 **"始终允许"** 并确定，否则 `adbd` 拒绝该公钥。
- 若之前授权过旧公钥、而你重新生成了密钥，需在手机上删除旧的授权记录后重新授权。

**Q3：烧录失败，`Connecting...` 卡住？**
- 按住 **BOOT 键** → 按一下 **RST** → 松开，进入下载模式后再 `idf.py flash`。

**Q4：`idf.py build` 报 GBK / 中文注释乱码？**
- 本项目 `sdkconfig.defaults` 已全英文 ASCII；请勿向其中添加中文注释。

**Q5：手机关闭 5555 了怎么重新开启？**
- 重插 ESP32 让它重新下发 `tcpip 5555`，或手动 `adb tcpip 5555`。

## 安全说明

- ⚠️ **私钥硬编码进固件**：固件（`.bin`）可被反解提取私钥。**仅用于一次性 / 低风险项目**，勿用于生产或连接重要设备。
- ⚠️ **私钥泄露风险**：拿到私钥者可冒充你的设备连接信任该公钥的手机。
- ✅ 开源提交前务必运行 `gen_key.py` 生成专属密钥并替换，**不要让真实私钥进入仓库**。

## 目录结构

```
esp32_usb_adb/
├── CMakeLists.txt          # 顶层工程
├── sdkconfig.defaults      # 默认配置（全英文 ASCII）
├── gen_key.py              # 密钥生成脚本（写入 adb_auth.c）
├── keys/                   # 生成的密钥（.gitignore 忽略，不进仓库）
├── .gitignore
├── README.md
└── main/
    ├── CMakeLists.txt      # 组件构建（usb / mbedtls / driver / esp_timer）
    ├── main.c              # USB Host 枚举 + ADB 状态机
    ├── adb_auth.c/h        # RSA 签名（mbedtls）
    └── adb_packet.c/h      # ADB 包头格式 + CRC32
```

## License

MIT
