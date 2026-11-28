#pragma once

#include <cstddef>

namespace chat
{
  // Combined UTF-8 bytes of the assistant's reply and reasoning in history.
  inline constexpr std::size_t kMaxAssistantTextBytes = 4 * 1024 * 1024;
}
