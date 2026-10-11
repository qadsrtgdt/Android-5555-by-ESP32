#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_key.py — 为 esp32_usb_adb 生成专属 RSA 密钥对，并自动写入 adb_auth.c。

用法:
    python gen_key.py [--bits 2048] [--cfile main/adb_auth.c]

依赖:
    cryptography (pip install cryptography) 或系统 openssl 命令(二选一)

它做三件事:
    1. 生成 RSA 密钥对 (默认 2048 bit)
    2. 私钥导出为 PKCS#8 PEM, 公钥导出为 OpenSSH 单行 ssh-rsa 格式
    3. 用正则定位替换 adb_auth.c 里的 adb_private_key_pem[] 和 adb_openssh_pubkey[]

注意:
    - 每次运行都会生成全新密钥, 手机需在弹窗里重新授权一次
    - 运行后必须重新编译烧录才能生效
    - 生成的 keys/ 目录已被 .gitignore 忽略, 不会进仓库
"""
import argparse
import base64
import os
import re
import shutil
import struct
import sys
import tempfile

DEFAULT_CFILE = os.path.join("main", "adb_auth.c")


def gen_with_openssl(bits: int, out_dir: str):
    """优先用系统 openssl 生成, 返回 (pem_path, pub_path)。"""
    pem = os.path.join(out_dir, "id_rsa.pem")
    pub = os.path.join(out_dir, "id_rsa.pub")
    if shutil.which("openssl") is None:
        return None
    # PKCS#8 PEM 私钥
    r = os.system(f'openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:{bits} '
                  f'-out "{pem}" >nul 2>&1')
    if r != 0 or not os.path.exists(pem):
        return None
    # OpenSSH 单行公钥
    r = os.system(f'openssl pkey -in "{pem}" -pubout -outform PEM '
                  f'| openssl ssh-keygen -i -m PKCS8 -f /dev/stdin > "{pub}" 2>nul')
    if r != 0 or not os.path.exists(pub):
        # 备用: 用 -m 直接导 pubkey 行
        r = os.system(f'openssl rsa -in "{pem}" -pubout -outform DER > "{pub}.der" 2>nul && '
                      f'python -c "import base64,sys;print(b\'ssh-rsa \'.decode()+\'\'.join(base64.b64encode(open(\'{pub}.der\',\'rb\').read()).decode(\'utf-8\').split(\'\\n\')))" > "{pub}" 2>nul')
        os.remove(f"{pub}.der")
        if not os.path.exists(pub):
            return None
    return pem, pub


def gen_with_cryptography(bits: int, out_dir: str):
    """用 cryptography 库生成, 返回 (pem, pub) 内容字符串。"""
    try:
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        return None
    key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    pem_bytes = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )
    pub_bytes = key.public_key().public_bytes(
        encoding=serialization.Encoding.OpenSSH,
        format=serialization.PublicFormat.OpenSSH,
    )
    pem = pem_bytes.decode("utf-8").strip() + "\n"
    pub = pub_bytes.decode("utf-8").strip()  # 形如 ssh-rsa AAA...==
    return pem, pub


def make_c_private_pem(pem: str) -> str:
    """把 PEM 私钥文本转成 C 多行字符串数组体。"""
    lines = pem.rstrip("\n").split("\n")
    body = "".join('"' + l + '\\n"\n' for l in lines)
    return body + '"\\0";'


def make_c_pubkey(pub: str) -> str:
    """OpenSSH 公钥单行转 C 字符串。"""
    return '"' + pub + '";'


def patch_adb_auth(cfile: str, pem: str, pub: str) -> bool:
    with open(cfile, "r", encoding="utf-8") as f:
        src = f.read()

    priv_body = make_c_private_pem(pem)
    pub_body = make_c_pubkey(pub)

    # 定位 adb_private_key_pem[] = ... ;   (DOTALL 到第一个分号)
    pat_priv = re.compile(r'(adb_private_key_pem\[\]\s*=\s*)(.*?)(;)', re.S)
    if not pat_priv.search(src):
        return False
    src = pat_priv.sub(lambda m: m.group(1) + priv_body + m.group(3), src, count=1)

    # 定位 adb_openssh_pubkey[] = ... ;
    pat_pub = re.compile(r'(adb_openssh_pubkey\[\]\s*=\s*)(.*?)(;)', re.S)
    if not pat_pub.search(src):
        return False
    src = pat_pub.sub(lambda m: m.group(1) + pub_body + m.group(3), src, count=1)

    with open(cfile, "w", encoding="utf-8") as f:
        f.write(src)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bits", type=int, default=2048)
    ap.add_argument("--cfile", default=DEFAULT_CFILE)
    args = ap.parse_args()

    keys_dir = os.path.join("keys")
    os.makedirs(keys_dir, exist_ok=True)

    pem = pub = None
    # 1) 尝试 openssl
    r = gen_with_openssl(args.bits, keys_dir)
    if r:
        with open(r[0], "r", encoding="utf-8") as f:
            pem = f.read().strip() + "\n"
        with open(r[1], "r", encoding="utf-8") as f:
            pub = f.read().strip()
        print("[ok] 使用系统 openssl 生成密钥")

    # 2) 否则尝试 cryptography
    if not pem or not pub:
        r = gen_with_cryptography(args.bits, keys_dir)
        if r:
            pem, pub = r
            with open(os.path.join(keys_dir, "id_rsa.pem"), "w", encoding="utf-8") as f:
                f.write(pem)
            with open(os.path.join(keys_dir, "id_rsa.pub"), "w", encoding="utf-8") as f:
                f.write(pub + "\n")
            print("[ok] 使用 cryptography 生成密钥")

    if not pem or not pub:
        print("[x] 未找到 openssl 或 cryptography。请先安装其一:")
        print("      pip install cryptography")
        sys.exit(1)

    if not patch_adb_auth(args.cfile, pem, pub):
        print(f"[x] 无法在 {args.cfile} 中找到密钥数组占位, 未修改。")
        sys.exit(1)

    print("[ok] 已写入 adb_auth.c:")
    print("     adb_private_key_pem[]   (PKCS#8 PEM)")
    print("     adb_openssh_pubkey[]    (ssh-rsa 单行)")
    print("[提示] 重新编译烧录: idf.py build && idf.py -p COMx flash")
    print("[提示] 手机需在弹窗中重新授权一次")


if __name__ == "__main__":
    main()
