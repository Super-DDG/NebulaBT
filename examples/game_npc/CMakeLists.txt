add_executable(game_npc main.cpp)
target_link_libraries(game_npc PRIVATE nebulabt::nebulabt)
add_test(NAME game_npc COMMAND game_npc)
set_tests_properties(game_npc PROPERTIES TIMEOUT 60)
