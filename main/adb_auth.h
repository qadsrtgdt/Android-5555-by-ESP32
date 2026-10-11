#ifndef __ADB_AUTH_H
#define __ADB_AUTH_H

#include <stdint.h>

/* 用 PKCS#8 PEM 私钥对 20 字节 ADB challenge 做 RSA PKCS#1 v1.5 + SHA1 签名
 * 成功返回 0，输出固定 256 字节签名到 out_sig */
int adb_rsa_sign(const char *pem_key, const uint8_t *challenge, uint8_t *out_sig);

/* 内嵌密钥（在 adb_auth.c 中定义） */
extern const char adb_private_key_pem[];
extern const char adb_openssh_pubkey[];

#endif /* __ADB_AUTH_H */
