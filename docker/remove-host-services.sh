#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "请使用 sudo bash docker/remove-host-services.sh" >&2
    exit 1
fi

systemctl disable --now mysql.service redis-server.service
DEBIAN_FRONTEND=noninteractive apt-get purge -y \
    mysql-server mysql-server-core mysql-client mysql-client-core mysql-shell \
    redis-server redis-tools
rm -rf -- /var/lib/mysql /var/lib/mysql-files /var/lib/mysql-keyring /var/lib/redis
