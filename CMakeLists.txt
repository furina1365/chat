cmake_minimum_required(VERSION 3.23)
project(chat LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(OpenSSL REQUIRED)
include(FetchContent)
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(DISABLE_TESTS ON)
FetchContent_Declare(hiredis
  URL https://github.com/redis/hiredis/archive/refs/tags/v1.3.0.tar.gz
  URL_HASH SHA256=25cee4500f359cf5cad3b51ed62059aadfc0939b05150c1f19c7e2829123631c)
FetchContent_MakeAvailable(hiredis)

# redis-plus-plus 需要 hiredis/hiredis.h 形式的头文件和可查找的 CMake package。
set(HIREDIS_BUILD_INCLUDE "${hiredis_BINARY_DIR}/include")
file(GLOB HIREDIS_HEADERS "${hiredis_SOURCE_DIR}/*.h")
file(COPY ${HIREDIS_HEADERS} DESTINATION "${HIREDIS_BUILD_INCLUDE}/hiredis")
file(WRITE "${hiredis_BINARY_DIR}/hiredis-config.cmake"
  "set(hiredis_FOUND TRUE)\nset(hiredis_INCLUDE_DIRS \"${HIREDIS_BUILD_INCLUDE}\")\n")
set(hiredis_DIR "${hiredis_BINARY_DIR}")
set(REDIS_PLUS_PLUS_BUILD_SHARED OFF)
set(REDIS_PLUS_PLUS_BUILD_TEST OFF)
FetchContent_Declare(redis_plus_plus
  URL https://github.com/sewenew/redis-plus-plus/archive/refs/tags/1.3.15.tar.gz
  URL_HASH SHA256=e9288fda74f1501c62dd9ab7d6a8dc67de51aa4d5b7c71770baa51987a945941)
FetchContent_MakeAvailable(redis_plus_plus)

FetchContent_Declare(fmt
  URL https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz
  URL_HASH SHA256=bc23066d87ab3168f27cef3e97d545fa63314f5c79df5ea444d41d56f962c6af)
FetchContent_MakeAvailable(fmt)
set(SOCI_LTO OFF)
set(WITH_BOOST OFF)
set(SOCI_MYSQL ON)
foreach(backend DB2 EMPTY FIREBIRD ODBC ORACLE POSTGRESQL SQLITE3)
  set(SOCI_${backend} OFF)
endforeach()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
FetchContent_Declare(soci
  URL https://github.com/SOCI/soci/archive/refs/tags/v4.1.4.tar.gz
  URL_HASH SHA256=144f017cccc2e2d806badb3313d6ab3c67a1925bccaa747a46fb3907108a615d)
FetchContent_MakeAvailable(soci)
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
target_link_libraries(server chat_net curl llhttp SOCI::MySQL redis++::redis++_static OpenSSL::Crypto)

# 在构建目录中用 make reactor / make proactor 切换后端并编译。
foreach(backend reactor proactor)
  add_custom_target(${backend}
    COMMAND ${CMAKE_COMMAND} -S "${CMAKE_SOURCE_DIR}" -B "${CMAKE_BINARY_DIR}" -DNET_BACKEND=${backend}
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target server
    USES_TERMINAL
    VERBATIM
  )
endforeach()
