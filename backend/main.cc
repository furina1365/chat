#include "core/Chat.hpp"
#include "ServerConfig.hpp"
#include <iostream>

int main(int argc, char **)
{
  if (argc != 1)
  {
    std::cerr << "Usage: server (edit config.json to configure startup)\n";
    return 1;
  }

  try
  {
    return RunChatServer(LoadServerConfig());
  }
  catch (const std::exception &error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
  catch (const db::Error &)
  {
    std::cerr << "Database unavailable; startup failed.\n";
    return 1;
  }
}
