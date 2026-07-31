cmake_minimum_required(VERSION 3.8)
project(cloud_visualization)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

add_library(cloud_preview_converter
  src/cloud_preview_converter.cpp
)
target_include_directories(cloud_preview_converter
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(cloud_preview_converter
  sensor_msgs
)

add_executable(cloud_visualization_node
  src/cloud_visualization_node.cpp
)
target_link_libraries(cloud_visualization_node
  cloud_preview_converter
)
ament_target_dependencies(cloud_visualization_node
  rclcpp
  sensor_msgs
)

# 在开发构建中保持常用警告，避免固定二进制契约附近出现隐式窄化等问题。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cloud_preview_converter PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(cloud_visualization_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
  TARGETS
    cloud_preview_converter
    cloud_visualization_node
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(
  DIRECTORY include/
  DESTINATION include
)

install(
  DIRECTORY config
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(
    test_cloud_preview_converter
    test/test_cloud_preview_converter.cpp
  )
  target_link_libraries(
    test_cloud_preview_converter
    cloud_preview_converter
  )
  ament_target_dependencies(
    test_cloud_preview_converter
    sensor_msgs
  )
endif()

ament_export_include_directories(include)
ament_export_dependencies(sensor_msgs)

ament_package()
