# 0 编译新增任务演示:宿主只编译一次,新任务 = 新 pack 目录 + load
add_executable(task_host main.cpp)
target_link_libraries(task_host PRIVATE nebulabt::nebulabt)

add_test(NAME zero_compile
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/demo.sh
                 $<TARGET_FILE:task_host>)
set_tests_properties(zero_compile PROPERTIES TIMEOUT 60)
