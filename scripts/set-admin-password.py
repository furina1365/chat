#!/usr/bin/env python3
"""设置独立的管理密码，配置中仅保存 scrypt 哈希。"""
import argparse
import getpass
import hashlib
import json
from pathlib import Path
import secrets

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--config', type=Path, default=Path('config.json'))
parser.add_argument('--generate', action='store_true', help='生成随机密码')
args = parser.parse_args()
config = json.loads(args.config.read_text())
if not isinstance(config, dict):
    parser.error('配置须为 JSON 对象')
password = secrets.token_urlsafe(18) if args.generate else getpass.getpass('管理密码（8–128 个字符）: ')
if not 8 <= len(password) <= 128:
    parser.error('密码须为 8–128 个字符')
if not args.generate and password != getpass.getpass('再次输入管理密码: '):
    parser.error('两次密码不一致')
salt = secrets.token_hex(16)
digest = hashlib.scrypt(password.encode(), salt=salt.encode(), n=32768, r=8, p=3,
                        maxmem=64 * 1024 * 1024, dklen=32).hex()
config.setdefault('auth', {})['admin_password_hash'] = f'scrypt$32768$8$3${salt}${digest}'
args.config.write_text(json.dumps(config, ensure_ascii=False, indent=2) + '\n')
if args.generate:
    print('管理密码: ' + password)
print('管理密码已设置，重启服务后生效。')
