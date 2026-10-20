cmake_minimum_required(VERSION 3.20)
project(chat LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(NET_BACKEND "proactor" CACHE STRING "Network backend (proactor or reactor)")

include_directories(backend)
set(NET_LIBRARIES pthread)
add_compile_options(-Wall -Wextra)

set(CHAT_LOGGING OFF)
if(CHAT_LOGGING)
  add_compile_definitions(LOGGING_DEBUG)
else()
  add_compile_definitions(LOGGING_RELEASE)
endif()

if(NET_BACKEND STREQUAL "proactor")
  add_compile_definitions(CHAT_NET_PROACTOR=1)
  set(NET_LIBRARIES ${NET_LIBRARIES} uring)
endif()
link_libraries(${NET_LIBRARIES})

file(GLOB_RECURSE SERVER_SOURCES CONFIGURE_DEPENDS backend/*.cc)
file(GLOB_RECURSE NET_SOURCES CONFIGURE_DEPENDS backend/net/src/*.cc)
list(FILTER SERVER_SOURCES EXCLUDE REGEX "/net/src/")
add_library(chat_net STATIC ${NET_SOURCES})
add_library(llhttp STATIC backend/llhttp/src/api.c backend/llhttp/src/http.c backend/llhttp/src/llhttp.c)
target_include_directories(llhttp PUBLIC backend/llhttp/include)
target_compile_options(llhttp PRIVATE -Wno-unused-parameter)
add_executable(server ${SERVER_SOURCES})
target_link_libraries(server chat_net curl llhttp)

# 在构建目录中用 make reactor / make proactor 切换后端并编译。
foreach(backend reactor proactor)
  add_custom_target(${backend}
    COMMAND ${CMAKE_COMMAND} -S "${CMAKE_SOURCE_DIR}" -B "${CMAKE_BINARY_DIR}" -DNET_BACKEND=${backend}
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target server
    USES_TERMINAL
    VERBATIM
  )
endforeach()

file(GLOB_RECURSE FRONTEND_SOURCES CONFIGURE_DEPENDS frontend/*)
list(FILTER FRONTEND_SOURCES EXCLUDE REGEX "/node_modules/")
add_custom_target(frontend SOURCES ${FRONTEND_SOURCES})
