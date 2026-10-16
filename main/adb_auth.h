#ifndef __ADB_AUTH_H
#define __ADB_AUTH_H

#include <stdint.h>
#include <stddef.h>

/* 用 PKCS#8 PEM 私钥对 20 字节 ADB challenge 做 RSA PKCS#1 v1.5 + SHA1 签名
 * 成功返回 0，输出固定 256 字节签名到 out_sig */
int adb_rsa_sign(const char *pem_key, const uint8_t *challenge, uint8_t *out_sig);

/* 获取 ADB wire 格式的公钥 blob（OpenSSH 二进制，base64 解码后的 "ssh-rsa"+e+n）
 * ADB 协议要求公钥以二进制 blob 发送，而非文本 "ssh-rsa AAA..."。
 * 成功返回 0，*out 指向静态缓冲，*out_len 为 blob 长度。 */
int adb_pubkey_blob(const uint8_t **out, size_t *out_len);

/* 内嵌密钥（在 adb_auth.c 中定义） */
extern const char adb_private_key_pem[];
extern const char adb_openssh_pubkey[];

#endif /* __ADB_AUTH_H */
