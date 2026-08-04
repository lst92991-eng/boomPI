function(boompi_configure_lvgl)
  if(TARGET lvgl::lvgl)
    return()
  endif()
  if(NOT IS_ABSOLUTE "${BOOMPI_LVGL_ROOT}" OR
     NOT EXISTS "${BOOMPI_LVGL_ROOT}/lvgl.h" OR
     NOT EXISTS "${BOOMPI_LVGL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "BOOMPI_LVGL_ROOT must point to the validated LVGL 8.2 source tree")
  endif()
  file(REAL_PATH "${BOOMPI_LVGL_ROOT}" BOOMPI_LVGL_ROOT)
  file(STRINGS "${BOOMPI_LVGL_ROOT}/lvgl.h" _lvgl_major
    REGEX "^#define LVGL_VERSION_MAJOR [0-9]+$")
  file(STRINGS "${BOOMPI_LVGL_ROOT}/lvgl.h" _lvgl_minor
    REGEX "^#define LVGL_VERSION_MINOR [0-9]+$")
  if(NOT _lvgl_major STREQUAL "#define LVGL_VERSION_MAJOR 8" OR
     NOT _lvgl_minor STREQUAL "#define LVGL_VERSION_MINOR 2")
    message(FATAL_ERROR "boomPI currently validates LVGL 8.2 only")
  endif()
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies statically" FORCE)
  set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "Use lvgl.h" FORCE)
  set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "Use project lv_conf.h" FORCE)
  set(LV_CONF_PATH "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/lvgl/lv_conf.h" CACHE FILEPATH
    "boomPI LVGL configuration" FORCE)
  add_subdirectory("${BOOMPI_LVGL_ROOT}" "${CMAKE_BINARY_DIR}/_deps/lvgl" EXCLUDE_FROM_ALL)
  target_link_libraries(lvgl PUBLIC Freetype::Freetype)
endfunction()
