cmake_minimum_required(VERSION 3.16)
project(nebulabt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NEBULABT_BUILD_EXAMPLES "Build NebulaBT examples" ON)
option(NEBULABT_BUILD_TESTS "Build NebulaBT tests" ON)
option(NEBULABT_BUILD_TOOLS "Build the nebula CLI" ON)

# BehaviorTree.CPP: system package first, then local source, then git fetch.
set(NEBULABT_BT_CPP_SOURCE_DIR "" CACHE PATH
    "Local BehaviorTree.CPP source directory (skips find_package and git fetch)")

find_package(behaviortree_cpp 4 QUIET)
if(NOT behaviortree_cpp_FOUND)
  include(FetchContent)
  # Keep BT.CPP lean: no tools, no examples, no tests, no sqlite/zmq extras.
  set(BTCPP_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BTCPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(BTCPP_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BTCPP_SQLITE_LOGGING OFF CACHE BOOL "" FORCE)
  set(BTCPP_GROOT_INTERFACE OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  if(NEBULABT_BT_CPP_SOURCE_DIR)
    FetchContent_Declare(behaviortree_cpp
      SOURCE_DIR ${NEBULABT_BT_CPP_SOURCE_DIR})
  else()
    FetchContent_Declare(behaviortree_cpp
      GIT_REPOSITORY https://github.com/BehaviorTree/BehaviorTree.CPP.git
      GIT_TAG v4.9.0)
  endif()
  FetchContent_MakeAvailable(behaviortree_cpp)
endif()

# 目标名因来源而异:ROS/ament 提供 behaviortree_cpp::behaviortree_cpp,
# FetchContent/普通安装提供 behaviortree_cpp
if(TARGET behaviortree_cpp::behaviortree_cpp)
  set(NEBULABT_BT_CPP_TARGET behaviortree_cpp::behaviortree_cpp)
elseif(TARGET behaviortree_cpp)
  set(NEBULABT_BT_CPP_TARGET behaviortree_cpp)
else()
  message(FATAL_ERROR "BehaviorTree.CPP target not found")
endif()

add_library(nebulabt
  src/core/failure.cpp
  src/core/journal.cpp
  src/core/scheduler.cpp
  src/core/schema.cpp
  src/core/engine.cpp
  src/nodes/checkpoint.cpp
  src/nodes/for_each.cpp
  src/nodes/retry_with_delay.cpp
  src/nodes/switch_n.cpp
  src/nodes/while_loop.cpp)
add_library(nebulabt::nebulabt ALIAS nebulabt)

target_include_directories(nebulabt PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

target_compile_features(nebulabt PUBLIC cxx_std_17)
target_link_libraries(nebulabt PUBLIC ${NEBULABT_BT_CPP_TARGET})

if(NEBULABT_BUILD_EXAMPLES)
  add_subdirectory(examples/hello_tree)
  add_subdirectory(examples/robot_delivery)
endif()

if(NEBULABT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(NEBULABT_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
