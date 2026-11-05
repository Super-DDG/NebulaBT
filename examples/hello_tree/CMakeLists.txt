add_executable(hello_tree main.cpp)
target_link_libraries(hello_tree PRIVATE nebulabt::nebulabt)
add_test(NAME hello_tree COMMAND hello_tree)
set_tests_properties(hello_tree PROPERTIES TIMEOUT 30)
