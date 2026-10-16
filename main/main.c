/*
 * ESP32-S3 USB Host + 手搓 ADB (适配 ESP-IDF v5.3.1)
 * 目标：以 USB Host 连接华为 P40 (鸿蒙4.2)，完成 ADB 认证，下发 "adb tcpip 5555"
 * 芯片: ESP32-S3   D+=GPIO20 D-=GPIO19
 *
 * v5.3 说明：
 *  - 设备连接通过 client 事件 USB_HOST_CLIENT_EVENT_NEW_DEV 通知（设备已被库自动枚举）
 *  - USB 传输完成通过 transfer->callback 回调触发
 *  - usb_host_lib_handle_events() / usb_host_client_handle_events() 驱动事件
 *
 * 状态机:
 *   CNXN(带公钥) -> AUTH(TOKEN) -> AUTH(SIGNATURE) -> AUTH(RSAPUB) -> OKAY
 *   -> CNXN(banner) -> OKAY -> OPEN("shell:") -> OKAY -> WRTE("tcpip 5555\n") -> DONE
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "adb_packet.h"
#include "adb_auth.h"

static const char *TAG = "ADB";

/* 控制 VBUS 5V 输出的 GPIO（GOOUUU 板需外接 MOS 管） */
#define VBUS_EN_GPIO GPIO_NUM_4

/* ---------------- ADB 状态机 ---------------- */
typedef enum {
    ADB_ST_IDLE = 0,
    ADB_ST_WAIT_AUTH_TOKEN,  /* 已发 CNXN(带公钥)，等 AUTH(TOKEN) 20字节挑战 */
    ADB_ST_WAIT_AUTH_OK,     /* 已发签名，等 OKAY/RSAPUB */
    ADB_ST_WAIT_CNXN_OK,     /* 已发 CNXN(banner)，等 OKAY(连接建立) */
    ADB_ST_WAIT_OPEN_OK,     /* 已发 OPEN("shell:")，等 OKAY(通道就绪) */
    ADB_ST_WAIT_CMD_DONE,    /* 已发 WRTE("tcpip 5555\n") */
    ADB_ST_DONE,
    ADB_ST_ERR
} adb_state_t;

typedef struct {
    adb_state_t state;
    usb_device_handle_t dev;
    uint8_t ep_in_addr, ep_out_addr;
    usb_transfer_t *tx, *rx;
    uint32_t local_id, remote_id;
    uint8_t auth_token[20];
    uint32_t tx_len;
} adb_ctx_t;

static adb_ctx_t ctx;
static usb_host_client_handle_t g_client = NULL;

/* ---------------- USB 枚举：找 CDC/ADB 接口的两个 Bulk 端点 ---------------- */
static int find_bulk_eps(const usb_config_desc_t *cfg, uint16_t total_len,
                         uint8_t *out_ep_in, uint8_t *out_ep_out)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint16_t off = cfg->bLength;
    int have_ep_in = 0, have_ep_out = 0;

    while (off < total_len) {
        uint8_t blen = p[off];
        uint8_t btype = p[off + 1];
        if (blen == 0) break;

        if (btype == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            have_ep_in = 0; have_ep_out = 0;
        } else if (btype == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            uint8_t ep_addr = p[off + 2];
            uint8_t ep_attr = p[off + 3];
            if ((ep_attr & 0x03) == 0x02) {   /* Bulk */
                if (ep_addr & 0x80) { if(!have_ep_in){ *out_ep_in = ep_addr; have_ep_in=1; } }
                else                { if(!have_ep_out){ *out_ep_out = ep_addr; have_ep_out=1; } }
            }
            if (have_ep_in && have_ep_out) return 0;
        }
        off += blen;
    }
    return -1;
}

/* 在配置描述符中找含指定端点的接口号 */
static uint8_t find_intf_of_ep(const usb_config_desc_t *cfg, uint16_t total_len, uint8_t ep_in)
{
    const uint8_t *p = (const uint8_t *)cfg;
    uint16_t off = cfg->bLength;
    uint8_t intf = 0;
    while (off < total_len) {
        uint8_t blen = p[off], btype = p[off+1];
        if (blen == 0) break;
        if (btype == USB_B_DESCRIPTOR_TYPE_INTERFACE) intf = p[off+2];
        if (btype == USB_B_DESCRIPTOR_TYPE_ENDPOINT && p[off+2] == ep_in) break;
        off += blen;
    }
    return intf;
}

/* ---------------- 发送 ADB 包 ---------------- */
static void send_packet(uint32_t cmd, uint32_t arg0, uint32_t arg1,
                        const uint8_t *payload, uint32_t len)
{
    adb_pkt_t pkt;
    adb_pkt_fill(&pkt, cmd, arg0, arg1, payload, len);
    memcpy(ctx.tx->data_buffer, &pkt, sizeof(pkt));
    if (len && payload) memcpy(ctx.tx->data_buffer + sizeof(pkt), payload, len);

    ctx.tx_len = sizeof(pkt) + len;
    ctx.tx->num_bytes       = ctx.tx_len;
    ctx.tx->device_handle   = ctx.dev;
    ctx.tx->bEndpointAddress = ctx.ep_out_addr;
    ctx.tx->timeout_ms      = 2000;
    ctx.tx->context         = (void *)1;   /* 1 = TX */
    /* callback 必须保持 transfer_cb（v5.3 靠它驱动状态机，不能置 NULL） */

    ESP_LOGI(TAG, "[TX] cmd=0x%08X arg0=%u arg1=%u len=%u", (unsigned)cmd,
             (unsigned)arg0, (unsigned)arg1, (unsigned)len);
    if (usb_host_transfer_submit(ctx.tx) != ESP_OK) {
        ctx.state = ADB_ST_ERR;
    }
}

static void start_rx(void)
{
    ctx.rx->num_bytes       = 4096;      /* IN: 应为 MPS(512) 整数倍，且 <= 缓冲区(4120) */
    ctx.rx->device_handle   = ctx.dev;
    ctx.rx->bEndpointAddress = ctx.ep_in_addr;
    ctx.rx->timeout_ms      = 5000;
    ctx.rx->context         = (void *)2;  /* 2 = RX */
    /* callback 必须保持 transfer_cb，否则收到的包不会被处理 */
    usb_host_transfer_submit(ctx.rx);
}

/* 用当前 auth_token 做 RSA 签名并发回 AUTH(SIGNATURE) */
static void sign_and_send_auth(void)
{
    uint8_t sig[256];
    if (adb_rsa_sign(adb_private_key_pem, ctx.auth_token, sig) == 0) {
        send_packet(ADB_CMD_AUTH, ADB_AUTH_SIGNATURE, 0, sig, 256);
    } else {
        ESP_LOGE(TAG, "RSA 签名失败");
        ctx.state = ADB_ST_ERR;
    }
}

/* ---------------- 处理收到的 ADB 包（推进状态机） ---------------- */
static void handle_rx_data(const uint8_t *buf, size_t len)
{
    if (len < sizeof(adb_pkt_t)) return;
    const adb_pkt_t *hdr = (const adb_pkt_t *)buf;
    const uint8_t *payload = buf + sizeof(adb_pkt_t);
    uint32_t cmd = hdr->cmd;

    if (hdr->magic != (cmd ^ 0xFFFFFFFFu)) {
        ESP_LOGE(TAG, "包校验失败 magic=0x%08X", (unsigned)hdr->magic);
        return;
    }
    if (hdr->data_len && adb_crc32(payload, hdr->data_len) != hdr->data_crc32) {
        ESP_LOGE(TAG, "包 CRC32 校验失败");
        return;
    }
    ESP_LOGI(TAG, "[RX] cmd=0x%08X arg0=%u arg1=%u len=%u", (unsigned)cmd,
             (unsigned)hdr->arg0, (unsigned)hdr->arg1, (unsigned)hdr->data_len);

    switch (ctx.state) {

    case ADB_ST_WAIT_AUTH_TOKEN:
        if (cmd == ADB_CMD_AUTH && hdr->arg0 == ADB_AUTH_TOKEN && hdr->data_len >= 20) {
            memcpy(ctx.auth_token, payload, 20);
            sign_and_send_auth();
            ctx.state = ADB_ST_WAIT_AUTH_OK;
        }
        break;

    case ADB_ST_WAIT_AUTH_OK:
        if (cmd == ADB_CMD_AUTH && hdr->arg0 == ADB_AUTH_RSAPUB) {
            /* adbd 请求公钥用于授权确认弹窗 */
            send_packet(ADB_CMD_AUTH, ADB_AUTH_RSAPUB, 0,
                        (const uint8_t *)adb_openssh_pubkey, strlen(adb_openssh_pubkey));
        } else if (cmd == ADB_CMD_AUTH && hdr->arg0 == ADB_AUTH_TOKEN && hdr->data_len >= 20) {
            /* 第二次 TOKEN：弹窗授权后 adbd 重新发挑战，需再次签名（状态保持，等 OKAY） */
            memcpy(ctx.auth_token, payload, 20);
            sign_and_send_auth();
        } else if (cmd == ADB_CMD_OKAY) {
            ESP_LOGI(TAG, "认证成功(OKAY)，重发 CNXN banner");
            uint8_t ba[8 + 6];
            uint32_t ver = ADB_VERSION, mpl = ADB_MAX_PAYLOAD;
            memcpy(ba, &ver, 4);
            memcpy(ba + 4, &mpl, 4);
            memcpy(ba + 8, "host::", 6);
            send_packet(ADB_CMD_CONNECT, 0, 0, ba, 8 + 6);
            ctx.state = ADB_ST_WAIT_CNXN_OK;
        }
        break;

    case ADB_ST_WAIT_CNXN_OK:
        if (cmd == ADB_CMD_OKAY) {
            ESP_LOGI(TAG, "连接建立，打开 shell: 通道");
            ctx.local_id = 1;
            send_packet(ADB_CMD_OPEN, ctx.local_id, 0,
                        (const uint8_t *)"shell:", 7);   /* 含 '\0' */
            ctx.state = ADB_ST_WAIT_OPEN_OK;
        }
        break;

    case ADB_ST_WAIT_OPEN_OK:
        if (cmd == ADB_CMD_OKAY) {
            ctx.remote_id = hdr->arg0;
            ESP_LOGI(TAG, "shell 通道就绪 remote_id=%u", (unsigned)ctx.remote_id);
            send_packet(ADB_CMD_WRTE, ctx.local_id, ctx.remote_id,
                        (const uint8_t *)"tcpip 5555\n", 11);
            ctx.state = ADB_ST_WAIT_CMD_DONE;
        }
        break;

    case ADB_ST_WAIT_CMD_DONE:
        if (cmd == ADB_CMD_WRTE && hdr->arg0 == ctx.remote_id) {
            ESP_LOGI(TAG, "收到命令回显：%.*s", (int)hdr->data_len, (const char *)payload);
        }
        if (cmd == ADB_CMD_OKAY) {
            ESP_LOGI(TAG, "命令执行完成，任务 DONE。手机已开启 tcpip 5555");
            ctx.state = ADB_ST_DONE;
        }
        break;

    default:
        break;
    }
}

/* ---------------- 传输完成回调（v5.3: 由 client handle_events 驱动） ---------------- */
static void transfer_cb(usb_transfer_t *t)
{
    if ((int)t->context == 2) {                 /* RX 完成 */
        if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
            handle_rx_data(t->data_buffer, (size_t)t->actual_num_bytes);
            if (ctx.state != ADB_ST_ERR) start_rx();
        } else {
            ESP_LOGW(TAG, "RX status=%d", (int)t->status);
            start_rx();
        }
    }
    /* context==1 为 TX，无需处理 */
}

/* ---------------- 设备连接后初始化（client NEW_DEV 事件） ---------------- */
static void open_and_init(uint8_t addr)
{
    esp_err_t err = usb_host_device_open(g_client, addr, &ctx.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open 失败 %d", (int)err);
        return;
    }
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(ctx.dev, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "获取配置描述符失败"); return;
    }
    uint16_t total_len = cfg->wTotalLength;

    uint8_t ep_in, ep_out;
    if (find_bulk_eps(cfg, total_len, &ep_in, &ep_out) != 0) {
        ESP_LOGE(TAG, "未找到 Bulk IN/OUT 端点"); return;
    }
    ctx.ep_in_addr = ep_in; ctx.ep_out_addr = ep_out;
    ESP_LOGI(TAG, "找到端点 IN=0x%02X OUT=0x%02X", (unsigned)ep_in, (unsigned)ep_out);

    uint8_t intf = find_intf_of_ep(cfg, total_len, ep_in);
    if (usb_host_interface_claim(g_client, ctx.dev, intf, 0) != ESP_OK) {
        ESP_LOGE(TAG, "claim 接口 %u 失败", (unsigned)intf); return;
    }
    /* v5.3: claim 接口后端点自动分配，传输直接使用端点地址，无需 ep_alloc */

    /* 启动 ADB 握手：发 CNXN(带公钥) */
    uint8_t cbuf[8 + 512];
    uint32_t ver = ADB_VERSION, mpl = ADB_MAX_PAYLOAD;
    memcpy(cbuf, &ver, 4);
    memcpy(cbuf + 4, &mpl, 4);
    size_t kl = strlen(adb_openssh_pubkey);
    memcpy(cbuf + 8, adb_openssh_pubkey, kl);
    send_packet(ADB_CMD_CONNECT, 0, 0, cbuf, 8 + (uint32_t)kl);
    ctx.state = ADB_ST_WAIT_AUTH_TOKEN;
    start_rx();
}

/* ---------------- client 事件回调 ---------------- */
static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "NEW_DEV addr=%u", (unsigned)event_msg->new_dev.address);
        open_and_init(event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "DEV_GONE");
        ctx.state = ADB_ST_IDLE;
        break;
    default:
        break;
    }
}

/* ---------------- 主任务 ---------------- */
static void adb_task(void *arg)
{
    if (usb_host_transfer_alloc(24 + 4096, 0, &ctx.tx) != ESP_OK) {
        ESP_LOGE(TAG, "tx alloc 失败"); vTaskDelete(NULL); return;
    }
    if (usb_host_transfer_alloc(24 + 4096, 0, &ctx.rx) != ESP_OK) {
        ESP_LOGE(TAG, "rx alloc 失败"); vTaskDelete(NULL); return;
    }
    ctx.tx->callback = transfer_cb;
    ctx.rx->callback = transfer_cb;

    while (1) {
        uint32_t lib_flags = 0;
        usb_host_lib_handle_events(20, &lib_flags);
        usb_host_client_handle_events(g_client, 20);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 USB Host ADB 启动");

    /* VBUS 控制 GPIO */
    gpio_config_t io = {0};
    io.pin_bit_mask = 1ULL << VBUS_EN_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    gpio_set_level(VBUS_EN_GPIO, 0);

    usb_host_config_t host_cfg = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    if (usb_host_install(&host_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install 失败"); return;
    }

    usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 10,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    if (usb_host_client_register(&client_cfg, &g_client) != ESP_OK) {
        ESP_LOGE(TAG, "client_register 失败"); return;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "打开 VBUS (GPIO%d) 给手机供电", VBUS_EN_GPIO);
    gpio_set_level(VBUS_EN_GPIO, 1);

    xTaskCreate(adb_task, "adb_task", 8192, NULL, 5, NULL);
}
