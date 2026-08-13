cmake_minimum_required(VERSION 3.8)
project(motion_compensation)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(localization REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(rcl_interfaces REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

add_library(motion_compensation_core
  src/enu_cloud_transformer.cpp
  src/enu_processing_diagnostics.cpp)
target_include_directories(motion_compensation_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
ament_target_dependencies(motion_compensation_core localization)

add_executable(enu_cloud_transform_node src/enu_cloud_transform_node.cpp)
target_link_libraries(enu_cloud_transform_node motion_compensation_core)
ament_target_dependencies(
  enu_cloud_transform_node
  builtin_interfaces diagnostic_msgs nav_msgs rcl_interfaces rclcpp sensor_msgs)

add_executable(odometry_timestamp_adapter_node src/odometry_timestamp_adapter_node.cpp)
ament_target_dependencies(
  odometry_timestamp_adapter_node builtin_interfaces nav_msgs rclcpp)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(motion_compensation_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(enu_cloud_transform_node PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(odometry_timestamp_adapter_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS motion_compensation_core enu_cloud_transform_node odometry_timestamp_adapter_node
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(launch_testing_ament_cmake REQUIRED)
  ament_add_gtest(test_enu_cloud_transformer test/test_enu_cloud_transformer.cpp)
  target_link_libraries(test_enu_cloud_transformer motion_compensation_core)
  ament_add_gtest(
    test_enu_processing_diagnostics test/test_enu_processing_diagnostics.cpp)
  target_link_libraries(test_enu_processing_diagnostics motion_compensation_core)
  add_launch_test(test/test_enu_cloud_transform_node.py TIMEOUT 30)
endif()

ament_export_include_directories(include)
ament_package()
