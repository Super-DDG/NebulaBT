add_executable(robot_delivery main.cpp)
target_link_libraries(robot_delivery PRIVATE nebulabt::nebulabt)
target_compile_definitions(robot_delivery PRIVATE
  ROBOT_DELIVERY_PACK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pack")
