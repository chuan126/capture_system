cmake_minimum_required(VERSION 3.8)
project(system_monitor)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

add_library(system_monitor_core src/system_monitor_core.cpp)
target_include_directories(system_monitor_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

add_executable(system_monitor_node src/system_monitor_node.cpp)
target_link_libraries(system_monitor_node system_monitor_core)
ament_target_dependencies(system_monitor_node diagnostic_msgs rclcpp sensor_msgs)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(system_monitor_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(system_monitor_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS system_monitor_core system_monitor_node
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include)
install(
  DIRECTORY config launch
  DESTINATION share/${PROJECT_NAME}
  PATTERN "__pycache__" EXCLUDE
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_system_monitor_core test/test_system_monitor_core.cpp)
  target_link_libraries(test_system_monitor_core system_monitor_core)
endif()

ament_export_include_directories(include)
ament_package()
