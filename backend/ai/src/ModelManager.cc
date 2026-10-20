#include "ai/ModelManager.hpp"
#include "protocols/ChatCompletionsClient.hpp"
#include "protocols/ResponsesClient.hpp"
#include "protocols/MessagesClient.hpp"

#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace ai
{
  namespace
  {
    using Json = nlohmann::json;
    std::string Required(const Json &value, const char *name)
    {
      if (!value.contains(name) || !value[name].is_string())
        throw std::runtime_error(std::string("模型配置缺失或无效：") + name);
      auto text = value[name].get<std::string>();
      if (text.empty())
        throw std::runtime_error(std::string("模型配置缺失或无效：") + name);
      if (text.find_first_of("\r\n") != text.npos || text.find('\0') != text.npos)
        throw std::runtime_error(std::string("模型配置包含控制字符：") + name);
      return text;
    }
    std::uint64_t Positive(const Json &value, const char *name, std::uint64_t fallback,
                           std::uint64_t maximum)
    {
      if (!value.contains(name))
        return fallback;
      const auto &number = value[name];
      if (!number.is_number_unsigned() && !(number.is_number_integer() && number.get<std::int64_t>() > 0))
        throw std::runtime_error(std::string("模型配置必须是正整数：") + name);
      auto result = number.get<std::uint64_t>();
      if (!result || result > maximum)
        throw std::runtime_error(std::string("模型配置超出范围：") + name);
      return result;
    }
    std::optional<double> Sampling(const Json &value, const char *name, double maximum)
    {
      if (!value.contains(name) || value[name].is_null())
        return {};
      if (!value[name].is_number())
        throw std::runtime_error(std::string("模型配置必须是数字：") + name);
      double result = value[name].get<double>();
      if (!std::isfinite(result) || result < 0 || result > maximum)
        throw std::runtime_error(std::string("模型配置超出范围：") + name);
      return result;
    }
    void ValidateUrl(const std::string &url)
    {
      auto handle = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(curl_url(), curl_url_cleanup);
      if (!handle || curl_url_set(handle.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("模型 base_url 无效");
      char *scheme = nullptr;
      if (curl_url_get(handle.get(), CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK)
        throw std::runtime_error("模型 base_url 缺少协议");
      std::string protocol(scheme);
      curl_free(scheme);
      if (protocol != "http" && protocol != "https")
        throw std::runtime_error("base_url 只支持 HTTP/HTTPS");
      for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT})
      {
        char *text = nullptr;
        auto code = curl_url_get(handle.get(), part, &text, 0);
        curl_free(text);
        if (code == CURLUE_OK)
          throw std::runtime_error("base_url 不得包含用户凭据、查询或片段");
      }
    }
  }

  ModelManager::ModelManager(const std::string &config_path)
  {
    std::ifstream file(config_path, std::ios::binary);
    if (!file)
      throw std::runtime_error("无法读取模型配置文件");
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    if (size < 0 || size > 1024 * 1024)
      throw std::runtime_error("模型配置文件过大");
    file.seekg(0);
    Json models;
    try
    {
      models = Json::parse(file);
    }
    catch (...)
    {
      throw std::runtime_error("模型配置不是有效 JSON");
    }
    if (!models.is_array() || models.empty() || models.size() > 128)
      throw std::runtime_error("模型配置必须是包含 1 到 128 项的数组");
    for (const auto &item : models)
    {
      if (!item.is_object())
        throw std::runtime_error("模型配置项必须是对象");
      ModelConfig config;
      config.provider = Required(item, "provider");
      config.model = Required(item, "model");
      config.base_url = Required(item, "base_url");
      while (!config.base_url.empty() && config.base_url.back() == '/')
        config.base_url.pop_back();
      ValidateUrl(config.base_url);
      config.api_key_env = Required(item, "api_key_env");
      auto api = Required(item, "api");
      if (api == "openai-completions")
        config.api = Api::OpenAICompletions;
      else if (api == "openai-responses")
        config.api = Api::OpenAIResponses;
      else if (api == "anthropic-messages")
        config.api = Api::AnthropicMessages;
      else
        throw std::runtime_error("未知模型 API 类型");
      if (item.contains("max_output_tokens") && !item["max_output_tokens"].is_null())
        config.max_output_tokens = Positive(item, "max_output_tokens", 4096, INT_MAX);
      config.temperature = Sampling(item, "temperature", config.api == Api::AnthropicMessages ? 1 : 2);
      config.top_p = Sampling(item, "top_p", 1);
      if (config.api == Api::AnthropicMessages && config.temperature && config.top_p)
        throw std::runtime_error("Messages 请只配置 temperature 或 top_p 中的一项");
      config.timeout = std::chrono::milliseconds(Positive(item, "timeout_ms", 60000, INT_MAX));
      config.connect_timeout = std::chrono::milliseconds(Positive(item, "connect_timeout_ms", 10000, INT_MAX));
      config.max_response_bytes = Positive(item, "max_response_bytes", 4 * 1024 * 1024, 64 * 1024 * 1024);
      const char *secret = std::getenv(config.api_key_env.c_str());
      if (!secret || !*secret)
        throw std::runtime_error("模型 API Key 环境变量未设置");
      std::string key(secret);
      if (key.find_first_of("\r\n") != key.npos)
        throw std::runtime_error("模型 API Key 格式无效");
      ModelKey id{config.provider, config.model};
      if (models_.count(id))
        throw std::runtime_error("模型配置 provider/model 重复");
      std::unique_ptr<ModelClient> client;
      switch (config.api)
      {
      case Api::OpenAICompletions:
        client = std::make_unique<ChatCompletionsClient>(config, key);
        break;
      case Api::OpenAIResponses:
        client = std::make_unique<ResponsesClient>(config, key);
        break;
      case Api::AnthropicMessages:
        client = std::make_unique<MessagesClient>(config, key);
        break;
      }
      models_.emplace(id, std::move(client));
    }
  }
  ModelManager::~ModelManager() = default;
  std::vector<ModelInfo> ModelManager::List() const
  {
    std::vector<ModelInfo> result;
    for (const auto &item : models_)
      result.push_back({item.first.first, item.first.second});
    return result;
  }
  const ModelClient *ModelManager::Find(const std::string &provider, const std::string &model) const
  {
    auto found = models_.find({provider, model});
    return found == models_.end() ? nullptr : found->second.get();
  }
} // namespace ai
