#pragma once

#include "ai/ModelClient.hpp"

namespace ai
{
  class MessagesClient final : public ModelClient
  {
  public:
    MessagesClient(ModelConfig config, std::string api_key);

  private:
    const char *Endpoint() const override;
    std::vector<std::string> AuthHeaders(const std::string &key) const override;
    nlohmann::json RequestBody(const ChatRequest &request, bool stream) const override;
    void ParseBody(const nlohmann::json &body, ModelResponseState &state) const override;
    bool ParseEvent(const SseEvent &event, ModelResponseState &state) const override;
  };
} // namespace ai
