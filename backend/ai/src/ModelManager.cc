#include "ai/ModelManager.hpp"
#include "protocols/ChatCompletionsClient.hpp"
#include "protocols/ResponsesClient.hpp"
#include "protocols/MessagesClient.hpp"

#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <climits>
#include <cmath>
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
        throw std::runtime_error(std::string("Missing or invalid model configuration: ") + name);
      auto text = value[name].get<std::string>();
      if (text.empty())
        throw std::runtime_error(std::string("Missing or invalid model configuration: ") + name);
      if (text.find_first_of("\r\n") != text.npos || text.find('\0') != text.npos)
        throw std::runtime_error(std::string("Model configuration contains control characters: ") + name);
      return text;
    }
    std::uint64_t Positive(const Json &value, const char *name, std::uint64_t fallback,
                           std::uint64_t maximum)
    {
      if (!value.contains(name))
        return fallback;
      const auto &number = value[name];
      if (!number.is_number_unsigned() && !(number.is_number_integer() && number.get<std::int64_t>() > 0))
        throw std::runtime_error(std::string("Model configuration must be a positive integer: ") + name);
      auto result = number.get<std::uint64_t>();
      if (!result || result > maximum)
        throw std::runtime_error(std::string("Model configuration is out of range: ") + name);
      return result;
    }
    std::optional<double> Sampling(const Json &value, const char *name, double maximum)
    {
      if (!value.contains(name) || value[name].is_null())
        return {};
      if (!value[name].is_number())
        throw std::runtime_error(std::string("Model configuration must be a number: ") + name);
      double result = value[name].get<double>();
      if (!std::isfinite(result) || result < 0 || result > maximum)
        throw std::runtime_error(std::string("Model configuration is out of range: ") + name);
      return result;
    }
    void ValidateUrl(const std::string &url)
    {
      auto handle = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(curl_url(), curl_url_cleanup);
      if (!handle || curl_url_set(handle.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("Invalid model base_url");
      char *scheme = nullptr;
      if (curl_url_get(handle.get(), CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK)
        throw std::runtime_error("Model base_url is missing a scheme");
      std::string protocol(scheme);
      curl_free(scheme);
      if (protocol != "http" && protocol != "https")
        throw std::runtime_error("base_url only supports HTTP/HTTPS");
      for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT})
      {
        char *text = nullptr;
        auto code = curl_url_get(handle.get(), part, &text, 0);
        curl_free(text);
        if (code == CURLUE_OK)
          throw std::runtime_error("base_url must not contain credentials, a query, or a fragment");
      }
    }
  }

  ModelManager::ModelManager(const std::string &config_path)
  {
    std::ifstream file(config_path, std::ios::binary);
    if (!file)
      throw std::runtime_error("Failed to read model configuration file");
    file.seekg(0, std::ios::end);
    auto size = file.tellg();
    if (size < 0 || size > 1024 * 1024)
      throw std::runtime_error("Model configuration file is too large");
    file.seekg(0);
    Json models;
    try
    {
      auto root = Json::parse(file);
      models = root.at("models");
    }
    catch (...)
    {
      throw std::runtime_error("Model configuration is not valid JSON");
    }
    if (!models.is_array() || models.empty() || models.size() > 128)
      throw std::runtime_error("Model configuration must be an array with 1 to 128 entries");
    for (const auto &item : models)
    {
      if (!item.is_object())
        throw std::runtime_error("Model configuration entry must be an object");
      ModelConfig config;
      config.provider = Required(item, "provider");
      config.model = Required(item, "model");
      config.base_url = Required(item, "base_url");
      while (!config.base_url.empty() && config.base_url.back() == '/')
        config.base_url.pop_back();
      ValidateUrl(config.base_url);
      auto api = Required(item, "api");
      if (api == "openai-completions")
        config.api = Api::OpenAICompletions;
      else if (api == "openai-responses")
        config.api = Api::OpenAIResponses;
      else if (api == "anthropic-messages")
        config.api = Api::AnthropicMessages;
      else
        throw std::runtime_error("Unknown model API type");
      if (item.contains("max_output_tokens") && !item["max_output_tokens"].is_null())
        config.max_output_tokens = Positive(item, "max_output_tokens", 4096, INT_MAX);
      config.temperature = Sampling(item, "temperature", config.api == Api::AnthropicMessages ? 1 : 2);
      config.top_p = Sampling(item, "top_p", 1);
      if (config.api == Api::AnthropicMessages && config.temperature && config.top_p)
        throw std::runtime_error("Messages requires setting only one of temperature and top_p");
      config.timeout = std::chrono::milliseconds(Positive(item, "timeout_ms", 60000, INT_MAX));
      config.connect_timeout = std::chrono::milliseconds(Positive(item, "connect_timeout_ms", 10000, INT_MAX));
      config.max_response_bytes = Positive(item, "max_response_bytes", 4 * 1024 * 1024, 64 * 1024 * 1024);
      auto key = Required(item, "api_key");
      ModelKey id{config.provider, config.model};
      if (models_.count(id))
        throw std::runtime_error("Duplicate model provider/model");
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
      result.push_back({item.first.first, item.first.second, item.second->SupportsReasoningEffort()});
    return result;
  }
  const ModelClient *ModelManager::Find(const std::string &provider, const std::string &model) const
  {
    auto found = models_.find({provider, model});
    return found == models_.end() ? nullptr : found->second.get();
  }
} // namespace ai
