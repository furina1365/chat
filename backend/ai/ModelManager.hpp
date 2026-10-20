#pragma once

#include "ModelClient.hpp"

#include <map>
#include <utility>

namespace ai
{
  class ModelManager
  {
  public:
    // 启动时读取 model.json 数组，校验配置、读取密钥并创建客户端。
    // 生成参数与连接配置一起载入 ModelConfig，由客户端持有，不接受前端覆盖。
    // 生成参数缺省或 null 使用上游默认值；显式值按对应协议校验。
    // timeout_ms、connect_timeout_ms、max_response_bytes 缺省使用本地默认值，
    // 显式配置必须为正整数，且能由对应的 ModelConfig 字段表示。
    // 未知 API 类型、重复 provider/model、缺失字段或密钥均抛出启动错误。
    explicit ModelManager(const std::string &config_path);
    ~ModelManager();
    ModelManager(const ModelManager &) = delete;
    ModelManager &operator=(const ModelManager &) = delete;

    // 不返回地址、密钥或完整配置；加载成功后允许并发只读访问。
    std::vector<ModelInfo> List() const;

    // 未配置则返回 nullptr。返回指针的有效期与本管理器一致。
    const ModelClient *Find(const std::string &provider,
                            const std::string &model) const;

  private:
    using ModelKey = std::pair<std::string, std::string>;
    std::map<ModelKey, std::unique_ptr<ModelClient>> models_;
  };
} // namespace ai
