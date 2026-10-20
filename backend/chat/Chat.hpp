#pragma once

#include "ServerConfig.hpp"

// 组合 HTTP 路由、模型客户端和有界 worker，提供网页聊天服务。
int RunChatServer(const ServerConfig &config);
