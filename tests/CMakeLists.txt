add_executable(nebulabt_tests test_core.cpp)
target_link_libraries(nebulabt_tests PRIVATE nebulabt::nebulabt)
add_test(NAME nebulabt_tests COMMAND nebulabt_tests)

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
