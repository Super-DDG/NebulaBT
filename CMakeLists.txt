cmake_minimum_required(VERSION 3.16)
project(nebulabt VERSION 1.1.0 LANGUAGES CXX)

# 提前开启:examples 子目录也注册 add_test(示例即测试)
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NEBULABT_BUILD_EXAMPLES "Build NebulaBT examples" ON)
option(NEBULABT_BUILD_TESTS "Build NebulaBT tests" ON)
option(NEBULABT_BUILD_TOOLS "Build the nebula CLI" ON)
option(NEBULABT_BUILD_BENCH "Build the core performance benchmark" ON)
# 树包清单可选 YAML 支持(规范格式为 pack.json,零依赖不变;
# 开启后 <pack>/pack.yaml 亦可,需系统安装 yaml-cpp)
option(NEBULABT_WITH_YAML "Support pack.yaml manifests via yaml-cpp" OFF)

# 消毒器(测试加固):address | undefined | thread | 组合(逗号分隔)
#   cmake -S . -B build-asan -DNEBULABT_SANITIZE=address
set(NEBULABT_SANITIZE "" CACHE STRING "Sanitizer: address|undefined|thread")
if(NEBULABT_SANITIZE)
  add_compile_options(-fsanitize=${NEBULABT_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${NEBULABT_SANITIZE})
endif()

# 树嵌入(M7):nebulabt_embed_trees() 随安装包分发,下游可 include
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# BehaviorTree.CPP: system package first, then local source, then git fetch.
set(NEBULABT_BT_CPP_SOURCE_DIR "" CACHE PATH
    "Local BehaviorTree.CPP source directory (skips find_package and git fetch)")

find_package(behaviortree_cpp 4 QUIET)
if(NOT behaviortree_cpp_FOUND)
  include(FetchContent)
  # Keep BT.CPP lean: no tools, no examples, no tests, no sqlite extras.
  set(BTCPP_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BTCPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(BTCPP_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BTCPP_SQLITE_LOGGING OFF CACHE BOOL "" FORCE)
  set(BTCPP_GROOT_INTERFACE ${NEBULABT_WITH_GROOT2} CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  if(NEBULABT_BT_CPP_SOURCE_DIR)
    FetchContent_Declare(behaviortree_cpp
      SOURCE_DIR ${NEBULABT_BT_CPP_SOURCE_DIR})
  else()
    FetchContent_Declare(behaviortree_cpp
      GIT_REPOSITORY https://github.com/BehaviorTree/BehaviorTree.CPP.git
      # 注意:BT.CPP 的 tag 不带 v 前缀(tag 4.9.0,不是 v4.9.0)
      GIT_TAG 4.9.0)
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
  src/core/journal_file.cpp
  src/core/pack.cpp
  src/core/scheduler.cpp
  src/core/schema.cpp
  src/core/engine.cpp
  src/core/engine_core.cpp
  src/core/bridge_node.cpp
  src/core/node_stats.cpp
  src/core/manifest_io.cpp
  src/core/nebula_ctx.cpp
  src/core/task.cpp
  src/core/tree_check.cpp
  src/nodes/checkpoint.cpp
  src/nodes/compensable.cpp
  src/nodes/for_each.cpp
  src/nodes/llm_decision.cpp
  src/nodes/reactive_guard.cpp
  src/nodes/retry_with_delay.cpp
  src/nodes/switch_n.cpp
  src/nodes/while_loop.cpp)
add_library(nebulabt::nebulabt ALIAS nebulabt)

target_include_directories(nebulabt PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

target_compile_features(nebulabt PUBLIC cxx_std_17)
target_link_libraries(nebulabt PUBLIC ${NEBULABT_BT_CPP_TARGET})

if(NEBULABT_WITH_YAML)
  find_package(yaml-cpp REQUIRED)
  target_link_libraries(nebulabt PUBLIC yaml-cpp)
  target_compile_definitions(nebulabt PUBLIC NEBULABT_HAVE_YAML)
endif()

# Groot2 实时可视化(M7):BT.CPP 编译期开启 BTCPP_GROOT_INTERFACE 才有
# Groot2Publisher 符号(需 ZeroMQ;CI/系统包默认带)。显式关闭本选项时,
# 代码以 NEBULABT_NO_GROOT2 守护,TaskSpec.groot2_port 仅告警不生效
option(NEBULABT_WITH_GROOT2 "Enable Groot2 live visualization (needs ZeroMQ)" ON)
if(NOT NEBULABT_WITH_GROOT2)
  target_compile_definitions(nebulabt PUBLIC NEBULABT_NO_GROOT2)
endif()

if(NEBULABT_BUILD_EXAMPLES)
  add_subdirectory(examples/hello_tree)
  add_subdirectory(examples/robot_delivery)
  add_subdirectory(examples/game_npc)
  add_subdirectory(examples/order_fulfillment)
  add_subdirectory(examples/zero_compile)
endif()

if(NEBULABT_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(NEBULABT_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(NEBULABT_BUILD_BENCH)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# 安装与 CMake 导出:下游可 find_package(nebulabt) 消费
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS nebulabt
        EXPORT nebulabtTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/nebulabt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT nebulabtTargets
        FILE nebulabtTargets.cmake
        NAMESPACE nebulabt::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nebulabt)

# 最小 Config:让 find_package(nebulabt) 找到已导出的 target
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nebulabtConfig.cmake.in"
"@PACKAGE_INIT@
include(\"\${CMAKE_CURRENT_LIST_DIR}/nebulabtTargets.cmake\")
check_required_components(nebulabt)
")
include(CMakePackageConfigHelpers)
configure_package_config_file(
  "${CMAKE_CURRENT_BINARY_DIR}/nebulabtConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/nebulabtConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nebulabt)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/nebulabtConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/nebulabtConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/nebulabtConfigVersion.cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/nebulabt-embed.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nebulabt)
