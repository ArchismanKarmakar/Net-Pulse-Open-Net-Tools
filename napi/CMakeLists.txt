# NetPulse native engine as a Node-API addon, built with CMake.js so the binary
# is independent of the Node.js version (works for Node and Electron ABIs).
# Pattern follows ArchismanKarmakar/Nodejs-Napi-Addon-Using-Cmake.
cmake_minimum_required(VERSION 3.15)
project(netpulse)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake.js-provided includes/libs (node headers + delay-load hook).
include_directories(${CMAKE_JS_INC})

set(SOURCE_FILES
  napi.cpp
  ../core/src/icmp.cpp
  ../core/src/stats.cpp
  ../core/src/transport.cpp
  ../core/src/session.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCE_FILES} ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")
target_include_directories(${PROJECT_NAME} PRIVATE ../core/include)
target_link_libraries(${PROJECT_NAME} ${CMAKE_JS_LIB})

# node-addon-api headers (header-only C++ wrapper).
execute_process(COMMAND node -p "require('node-addon-api').include"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NODE_ADDON_API_DIR)
string(REPLACE "\n" "" NODE_ADDON_API_DIR ${NODE_ADDON_API_DIR})
string(REPLACE "\"" "" NODE_ADDON_API_DIR ${NODE_ADDON_API_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${NODE_ADDON_API_DIR})

add_definitions(-DNAPI_VERSION=8 -DNAPI_CPP_EXCEPTIONS)

if(WIN32)
  add_definitions(-DNOMINMAX)
  target_link_libraries(${PROJECT_NAME} ws2_32 iphlpapi winmm)
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()
