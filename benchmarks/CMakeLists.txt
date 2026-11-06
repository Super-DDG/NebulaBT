# 核心性能基准:嵌入式选型数据(tick 吞吐/周转/journal 开销/内存峰值)
add_executable(nebulabt_bench bench_core.cpp)
target_link_libraries(nebulabt_bench PRIVATE nebulabt::nebulabt)
