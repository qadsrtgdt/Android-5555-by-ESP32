#include "adb_auth.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include <string.h>

/* =========================================================================
 * ADB RSA 私钥（PKCS#8 PEM，2048bit）
 *
 * ⚠️⚠️⚠️ 以下密钥为【公开占位 / DEMO 密钥】，仅用于让工程可直接编译。
 * 提交仓库前请务必运行 gen_key.py 生成你自己的密钥并替换！
 *   python gen_key.py      # 生成后自动写入本文件
 * 否则你的手机将信任一个"众所周知"的密钥，存在被冒用的风险。
 * ========================================================================= */
const char adb_private_key_pem[] =
"-----BEGIN PRIVATE KEY-----\n"
"MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCssC/F1Axjwvc8\n"
"miRIS8zXNk+KBum+WvkduQaa195PKes8a1LCvGUJZsBkXKy5bAQBbDlNsnPSSUlT\n"
"rtK6De9l7xE2ng+Oz7NFJ/Z5cdkFUAaXLot4L9j73Z3bj/M6Zvy7CGEnYm2v3lHL\n"
"2ItlnoOq/6fpWGbAneI6ZmxYmyZaoFa+ye1P2YzYUbhxsDNwG/OOOTTScr4M9plA\n"
"t1thL9FtZn97q/Gou3DIopL/NI4F3JaEw/YhgGBy03dWIDJe+ykfbTKrLt2MINCw\n"
"BmbUg3VRmEj53Sgap2S/OQTR4LQf7hPvhDjxorrA7ZMb7Wdw2xQ7Kn2CEN7vzVGB\n"
"UABz5uebAgMBAAECggEAHswtOPLgYspanYw//ETIe3lMJimldjVzSEG/U84775Ji\n"
"SZw6XlCLFGom1b9RbqA89qP+dETnMxGu6iW7n0Ls9xjz0PHPhIC2UBcYdgA/3Bqx\n"
"kjTS2tRttj2H8ahykG/dkt2jtgpv1XWo0NoFwlHK2axOnecOLDv8JL4vhH3nETJM\n"
"M6YBPvB0/VLxapdDC1bpuPvuRPdBdy74kVwd9BNoMiAqnZfnjJ6+Zeze8jUYRN1/\n"
"fSuAFaP/YZlT25dOzqiDcXGgnUv1iG0rseh59Vk8t09Tjx+xRoshXyOizbiJwETr\n"
"Uxnc3lnr1/AVcrUlf8iMREi5DMR42msrAFSNDvriAQKBgQDb67fZE0TCb2pIAsUy\n"
"lrU942ppikKDUFbLqU5HLxvoIaEhTmpOUfrQtUIKk/W/aWm3oNkR+blUWnUgIVFV\n"
"ovFOA20njtdapjamQYAnOrNGqserFM114jWbWyC1b2WEPRvz2F8SIWTQLJYO0GbW\n"
"jFFvKyrGJgemi5FgmAuSCceGOwKBgQDJBMkUTbc5Mu/goTEk41iUukddDCQsZxsq\n"
"RuF4xOJnr3bwiV5DCusjY+hmGHOrhhGzuiYtMrolYT7bN133abLmR960v+ZBca7Z\n"
"rKMTUTN89s2TPXo5AVQAL5gBNKzJXPrfm6zGQt0lPP68evRALrMVcxDFbqnzjax9\n"
"icfnxDAuIQKBgCvlQ7/vSaUpieQvsV6kmC5vKvk/4IDHU1u5QmeQ7pgJxliYhz10\n"
"vYRU+YaU+9Ml3jUJxOoCl0xP9drg65QVPubcZjgkGbX9QSE0J7kszUXIpqvuLghe\n"
"i2aj7MZqIrAfZmnogSlkQvQOQjFYxX4Daemviw0b4CEM8IqALqcKjqmrAoGBAJON\n"
"qbP3n+iilSXD7HjxUJJLchrA0MSvImeUA+lMTAxj5eCGVOTNXPBrY6mdhTqAfPn6\n"
"SA+hTrYlfagYKid0NNeIJddzjSa4teoeKM4DKtItr5TG6bJyYSCJzwADlFlyoMwG\n"
"T3pwljfBYBMmBagfH2E9RzBFHQckFVtJCa38eolBAoGANiIHvsPpT59Xj3GvSRss\n"
"xHPQZuqkAFEExHkNhzKG8yJ9aWgOTwq7+hjH38xGLSKfvP+ZwlVDS8eEPvLDUlvH\n"
"dF+5LwoQG9gksmfYhwggCezhgiMZIys8kxiZuPc4sQ8pL3/IwtWDtG2B6sVxwu38\n"
"eWYzXsdhf2hOU0RzzLGC14Q=\n"
"-----END PRIVATE KEY-----\n"
"\0";;

/* ============ ADB 公钥（OpenSSH 单行格式，用于 CNXN 包） ============
 * DEMO 占位公钥，请随 gen_key.py 一起替换 */
const char adb_openssh_pubkey[] =
"ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQCssC/F1Axjwvc8miRIS8zXNk+KBum+WvkduQaa195PKes8a1LCvGUJZsBkXKy5bAQBbDlNsnPSSUlTrtK6De9l7xE2ng+Oz7NFJ/Z5cdkFUAaXLot4L9j73Z3bj/M6Zvy7CGEnYm2v3lHL2ItlnoOq/6fpWGbAneI6ZmxYmyZaoFa+ye1P2YzYUbhxsDNwG/OOOTTScr4M9plAt1thL9FtZn97q/Gou3DIopL/NI4F3JaEw/YhgGBy03dWIDJe+ykfbTKrLt2MINCwBmbUg3VRmEj53Sgap2S/OQTR4LQf7hPvhDjxorrA7ZMb7Wdw2xQ7Kn2CEN7vzVGBUABz5ueb";;

/* mbedTLS 随机源（RSA 解析需要） */
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;
static int rng_ready = 0;

static void ensure_rng(void)
{
    if (rng_ready) return;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                          (const unsigned char *)"adb-auth", 8);
    rng_ready = 1;
}

/* 输入 20 字节 challenge，输出 256 字节 RSA PKCS#1 v1.5 + SHA1 签名 */
int adb_rsa_sign(const char *pem_key, const uint8_t *challenge, uint8_t *out_sig)
{
    mbedtls_pk_context pk;
    size_t sig_len = 256;
    int ret;

    ensure_rng();
    mbedtls_pk_init(&pk);

    ret = mbedtls_pk_parse_key(&pk,
                               (const unsigned char *)pem_key,
                               strlen(pem_key),
                               NULL, 0,
                               mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return ret;
    }

    ret = mbedtls_pk_sign(&pk,
                          MBEDTLS_MD_SHA1,
                          challenge, 20,
                          out_sig, 256, &sig_len,
                          mbedtls_ctr_drbg_random, &ctr_drbg);
    mbedtls_pk_free(&pk);
    return (ret == 0 && sig_len == 256) ? 0 : (ret ? ret : -1);
}
