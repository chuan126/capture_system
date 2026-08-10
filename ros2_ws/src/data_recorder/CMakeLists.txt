cmake_minimum_required(VERSION 3.8)
project(data_recorder)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(interfaces REQUIRED)
find_package(localization REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(data_recorder_node src/data_recorder_node.cpp)
ament_target_dependencies(data_recorder_node interfaces localization nav_msgs rclcpp sensor_msgs)
target_link_libraries(data_recorder_node SQLite::SQLite3)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(data_recorder_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS data_recorder_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config launch DESTINATION share/${PROJECT_NAME})

ament_package()
