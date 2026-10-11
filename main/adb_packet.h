#ifndef __ADB_PACKET_H
#define __ADB_PACKET_H

#include <stdint.h>
#include <stddef.h>

/* ADB 命令字（小端 uint32） */
#define ADB_CMD_CONNECT 0x4E584E43u  /* "CNXN" */
#define ADB_CMD_AUTH    0x48545541u  /* "AUTH" */
#define ADB_CMD_OKAY    0x59414B4Fu  /* "OKAY" */
#define ADB_CMD_OPEN    0x4E45504Fu  /* "OPEN" */
#define ADB_CMD_WRTE    0x45545257u  /* "WRTE" */
#define ADB_CMD_CLSE    0x45534C43u  /* "CLSE" */

/* AUTH 子类型 */
#define ADB_AUTH_TOKEN     1
#define ADB_AUTH_SIGNATURE 2
#define ADB_AUTH_RSAPUB    3

/* ADB 协议版本 & 最大包长 */
#define ADB_VERSION        0x01000000u  /* 小端即 version=1 */
#define ADB_MAX_PAYLOAD    0x00001000u  /* 4096 */

/* ADB 包头结构（24 字节，所有字段小端 uint32） */
typedef struct __attribute__((packed)) {
    uint32_t cmd;        /* 命令字 */
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_len;   /* payload 长度 */
    uint32_t data_crc32; /* payload CRC32 */
    uint32_t magic;      /* cmd ^ 0xFFFFFFFF */
} adb_pkt_t;

uint32_t adb_crc32(const uint8_t *buf, size_t len);
void adb_pkt_fill(adb_pkt_t *pkt, uint32_t cmd, uint32_t arg0, uint32_t arg1,
                  const uint8_t *payload, uint32_t payload_len);

#endif /* __ADB_PACKET_H */
