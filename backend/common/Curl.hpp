#pragma once
#include <curl/curl.h>
#include <stdexcept>

namespace transport
{
inline void InitCurl()
{
  struct Runtime
  {
    Runtime()
    {
      if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("Failed to initialize libcurl");
    }
    ~Runtime() { curl_global_cleanup(); }
  };
  static Runtime runtime;
}
}
