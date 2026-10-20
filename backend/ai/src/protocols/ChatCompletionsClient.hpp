#pragma once

#include "ai/ModelClient.hpp"

namespace ai
{
  class ChatCompletionsClient final : public ModelClient
  {
  public:
    ChatCompletionsClient(ModelConfig config, std::string api_key);

  private:
    const char *Endpoint() const override;
    nlohmann::json RequestBody(const ChatRequest &request, bool stream) const override;
    void ParseBody(const nlohmann::json &body, ModelResponseState &state) const override;
    bool ParseEvent(const SseEvent &event, ModelResponseState &state) const override;
  };
} // namespace ai
