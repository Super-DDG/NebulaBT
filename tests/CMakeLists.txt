add_executable(nebulabt_tests test_core.cpp)
target_link_libraries(nebulabt_tests PRIVATE nebulabt::nebulabt)
add_test(NAME nebulabt_tests COMMAND nebulabt_tests)
