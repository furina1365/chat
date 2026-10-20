#include "chat/Chat.hpp"
#include "ServerConfig.hpp"
#include <iostream>

int main(int argc, char **)
{
  if (argc != 1)
  {
    std::cerr << "用法：server（启动配置请修改 config.json）\n";
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
}
