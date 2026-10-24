#!/usr/bin/env python3
"""从 config.json 读取 MySQL 凭据，再执行 Docker Compose。"""

import json
import os
from pathlib import Path
import sys


def main():
    root = Path(__file__).resolve().parent.parent
    try:
        config = json.loads((root / "config.json").read_text())
        mysql = config["mysql"]
        # 初始化 SQL 使用固定的 chat 数据库和应用账号。
        if mysql["database"] != "chat" or mysql["username"] != "chat":
            raise ValueError("Docker MySQL 初始化要求 database 和 username 均为 chat")
        credentials = {
            "CHAT_MYSQL_PASSWORD": mysql["password"],
            "CHAT_MYSQL_ROOT_PASSWORD": config["docker"]["mysql_root_password"],
        }
        if any(not isinstance(value, str) or not value or "\0" in value
               for value in credentials.values()):
            raise ValueError("请在 config.json 中填写 MySQL 应用密码和 root 密码")
    except (OSError, ValueError, KeyError, TypeError) as error:
        # JSONDecodeError 的原文可能包含实际凭据。
        message = "config.json 不是有效 JSON" if isinstance(error, json.JSONDecodeError) else str(error)
        sys.exit(f"无法加载 Docker 配置：{message}")

    environment = os.environ.copy()
    environment.update(credentials)
    os.chdir(root)
    os.execvpe("docker", ["docker", "compose", "--env-file", "/dev/null",
                         "-f", str(root / "compose.yaml"), *sys.argv[1:]], environment)


if __name__ == "__main__":
    main()
