#include "core/Chat.hpp"
#include "ServerConfig.hpp"
#include "net/Config.hpp"
#include <iostream>
#include <string_view>

int main(int argc, char **argv)
{
  if (argc == 2 && std::string_view(argv[1]) == "--network-backend")
  {
    std::cout << (CHAT_NET_PROACTOR ? "proactor" : "reactor") << '\n';
    return 0;
  }
  if (argc != 1)
  {
    std::cerr << "Usage: server [--network-backend] (edit config.json to configure startup)\n";
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
