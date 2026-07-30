cmake_minimum_required(VERSION 3.24)

project(hyperion VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Wt's package-version file matches exact minor versions when a minor is
# requested. Ask for API major 4 so 4.10+ remain valid.
find_package(Wt 4 CONFIG REQUIRED COMPONENTS Wt HTTP)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

find_package(hiredis CONFIG QUIET)
if(NOT TARGET hiredis::hiredis)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
  add_library(hiredis::hiredis ALIAS PkgConfig::HIREDIS)
endif()

find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann/json.hpp not found (need nlohmann-json3-dev)")
  endif()
  add_library(nlohmann_json_header INTERFACE)
  target_include_directories(nlohmann_json_header INTERFACE ${NLOHMANN_JSON_INCLUDE_DIR})
  add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json_header)
endif()

add_executable(hyperion
  src/main.cpp
  src/options.cpp
  src/redis_mux.cpp
  src/http_client.cpp
  src/book_state.cpp
  src/hedge_pdf.cpp
  src/session_feed.cpp
  src/server_panel.cpp
  src/hyperion_app.cpp
)

target_include_directories(hyperion PRIVATE src)
target_link_libraries(hyperion PRIVATE
  Wt::Wt
  Wt::HTTP
  CURL::libcurl
  Threads::Threads
  hiredis::hiredis
  nlohmann_json::nlohmann_json
)

install(TARGETS hyperion RUNTIME DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/hyperion/static)
install(FILES
  config/hyperion.example.json
  config/account.env.example
  DESTINATION share/hyperion/config
)
