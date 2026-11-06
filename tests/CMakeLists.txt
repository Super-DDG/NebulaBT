add_executable(nebulabt_tests test_core.cpp)
target_link_libraries(nebulabt_tests PRIVATE nebulabt::nebulabt)
add_test(NAME nebulabt_tests COMMAND nebulabt_tests)

# 并发压力 + 引擎析构时序(测试加固)
add_executable(nebulabt_stress test_stress.cpp)
target_link_libraries(nebulabt_stress PRIVATE nebulabt::nebulabt)
add_test(NAME nebulabt_stress COMMAND nebulabt_stress)

# nebula CLI 冒烟(测试加固):需要 nebula 二进制路径
if(NEBULABT_BUILD_TOOLS)
  add_executable(nebulabt_cli_test test_cli.cpp)
  target_link_libraries(nebulabt_cli_test PRIVATE nebulabt::nebulabt)
  target_compile_definitions(nebulabt_cli_test PRIVATE
      NEBULA_BIN="$<TARGET_FILE:nebula>")
  add_test(NAME nebulabt_cli COMMAND nebulabt_cli_test)
endif()

# 树嵌入(M7):XML 编译进二进制,无文件系统提交
include(nebulabt-embed)
add_executable(nebulabt_embed_test test_embed.cpp)
target_link_libraries(nebulabt_embed_test PRIVATE nebulabt::nebulabt)
nebulabt_embed_trees(nebulabt_embed_test embed_me trees/embed_me.xml)
add_test(NAME nebulabt_embed COMMAND nebulabt_embed_test)

# 恢复性质测试(M6):随机交错 × 200 轮,断言检查点副作用恰好一次
add_executable(nebulabt_property test_property_recovery.cpp)
target_link_libraries(nebulabt_property PRIVATE nebulabt::nebulabt)
add_test(NAME nebulabt_property COMMAND nebulabt_property)

# kill -9 实机恢复测试(依赖 fork/SIGKILL,仅 POSIX)
if(UNIX AND NOT APPLE)
  add_executable(nebulabt_kill9 test_kill9.cpp)
  target_link_libraries(nebulabt_kill9 PRIVATE nebulabt::nebulabt)
  add_test(NAME nebulabt_kill9 COMMAND nebulabt_kill9)
  set_tests_properties(nebulabt_kill9 PROPERTIES TIMEOUT 60)
endif()
