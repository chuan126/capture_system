cmake_minimum_required(VERSION 3.8)
project(task_manager)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(interfaces REQUIRED)
find_package(rclcpp REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(task_manager_node src/task_manager_node.cpp)
ament_target_dependencies(task_manager_node interfaces rclcpp)
target_link_libraries(task_manager_node SQLite::SQLite3)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(task_manager_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS task_manager_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config launch DESTINATION share/${PROJECT_NAME})

ament_package()
