add_executable(order_fulfillment main.cpp)
target_link_libraries(order_fulfillment PRIVATE nebulabt::nebulabt)
add_test(NAME order_fulfillment COMMAND order_fulfillment)
set_tests_properties(order_fulfillment PROPERTIES TIMEOUT 60)
