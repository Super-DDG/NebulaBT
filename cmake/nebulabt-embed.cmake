# nebulabt-embed.cmake — 把树 XML 编译进二进制(M7)
#
# 无文件系统的 MCU / 现场防树文件被改坏:构建期把 XML 转成字符串常量头,
# 业务代码直接引用 `nebulabt_embed::NEBULA_TREE_<NAME>` 提交。
#
# 用法:
#   include(nebulabt-embed)   # 随 nebulabt 安装于 lib/cmake/nebulabt/
#   add_executable(my_app main.cpp)
#   nebulabt_embed_trees(my_app
#     patrol  trees/patrol.xml      # NAME = 路径
#     recover trees/recover.xml)
#
# 生成(构建目录,不污染源码树):
#   nebulabt_embedded/<name>.h
#     namespace nebulabt_embed {
#       static const char NEBULA_TREE_PATROL[] = R"nebula_xml(...)nebula_xml";
#     }
#
# 配置期生成:改 XML 需重新 configure(常规 CI 全量构建无感)。

function(nebulabt_embed_trees TARGET)
  if(NOT ARGN)
    message(FATAL_ERROR "nebulabt_embed_trees: need NAME=path pairs")
  endif()
  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/nebulabt_embedded")
  file(MAKE_DIRECTORY "${out_dir}")
  set(pairs "")
  set(prev "")
  foreach(arg ${ARGN})
    if(arg MATCHES "=")
      list(APPEND pairs "${arg}")
    else()
      # 两种写法都支持:NAME path  或  NAME=path
      if(NOT prev STREQUAL "")
        list(APPEND pairs "${prev}=${arg}")
        set(prev "")
      else()
        set(prev "${arg}")
      endif()
    endif()
  endforeach()
  if(NOT prev STREQUAL "")
    message(FATAL_ERROR "nebulabt_embed_trees: dangling NAME '${prev}' without path")
  endif()
  foreach(pair ${pairs})
    string(FIND "${pair}" "=" eq)
    string(SUBSTRING "${pair}" 0 ${eq} name)
    string(SUBSTRING "${pair}" ${eq} -1 rel_path)
    string(SUBSTRING "${rel_path}" 1 -1 rel_path)
    if(IS_ABSOLUTE "${rel_path}")
      set(xml_path "${rel_path}")
    else()
      set(xml_path "${CMAKE_CURRENT_SOURCE_DIR}/${rel_path}")
    endif()
    if(NOT EXISTS "${xml_path}")
      message(FATAL_ERROR "nebulabt_embed_trees: tree not found: ${xml_path}")
    endif()
    string(TOUPPER "${name}" name_upper)
    string(REGEX REPLACE "[^A-Z0-9_]" "_" name_upper "${name_upper}")
    file(READ "${xml_path}" content)
    # 原始字符串定界符:XML 内出现 ")nebula_xml\"" 的概率可忽略;
    # 尾部换行保留(与文件内容一致)
    set(header "${out_dir}/${name}.h")
    file(WRITE "${header}"
"// 由 nebulabt_embed_trees 生成;勿手改。源: ${xml_path}
namespace nebulabt_embed {
static const char NEBULA_TREE_${name_upper}[] = R\"nebula_xml(${content})nebula_xml\";
}
")
    set_property(TARGET ${TARGET} APPEND PROPERTY
      NEBULABT_EMBED_HEADERS "${header}")
  endforeach()
  get_property(headers TARGET ${TARGET} PROPERTY NEBULABT_EMBED_HEADERS)
  target_include_directories(${TARGET} PRIVATE "${out_dir}")
endfunction()
