cmake_minimum_required(VERSION 3.8)
project(localization)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(interfaces REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(rcl_interfaces REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

add_library(attitude_matrix STATIC
  Attitude/attitude_matrix.cpp
)
target_include_directories(attitude_matrix
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Attitude>
    $<INSTALL_INTERFACE:include/${PROJECT_NAME}/Attitude>
)

add_library(localization_attitude_transform
  src/attitude_transform.cpp
)
target_include_directories(localization_attitude_transform
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Attitude
)
target_link_libraries(localization_attitude_transform PUBLIC attitude_matrix)

add_library(localization_dead_reckoning_core
  src/geodesy.cpp
  src/heading_alignment.cpp
  src/similarity_alignment.cpp
  src/dead_reckoning.cpp
)
target_include_directories(localization_dead_reckoning_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_executable(dead_reckoning_node
  src/dead_reckoning_node.cpp
)
target_include_directories(dead_reckoning_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Attitude)
target_link_libraries(
  dead_reckoning_node
  localization_dead_reckoning_core
  localization_attitude_transform
)
ament_target_dependencies(dead_reckoning_node interfaces nav_msgs rcl_interfaces rclcpp sensor_msgs)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(attitude_matrix PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(localization_attitude_transform PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(localization_dead_reckoning_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(dead_reckoning_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
  TARGETS
    attitude_matrix
    localization_attitude_transform
    localization_dead_reckoning_core
  EXPORT export_localization_attitude_matrix
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
install(TARGETS dead_reckoning_node DESTINATION lib/${PROJECT_NAME})
install(
  DIRECTORY include/
  DESTINATION include
)
install(
  DIRECTORY config
  DESTINATION share/${PROJECT_NAME}
)
install(
  FILES Attitude/attitude_matrix.h
  DESTINATION include/${PROJECT_NAME}/Attitude
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(
    test_attitude_matrix
    test/test_attitude_matrix.cpp
  )
  target_link_libraries(test_attitude_matrix localization_attitude_transform)

  ament_add_gtest(
    test_dead_reckoning_math
    test/test_dead_reckoning_math.cpp
  )
  target_link_libraries(test_dead_reckoning_math localization_dead_reckoning_core)
endif()

ament_export_include_directories(include)
ament_export_libraries(attitude_matrix localization_attitude_transform localization_dead_reckoning_core)
ament_export_targets(export_localization_attitude_matrix HAS_LIBRARY_TARGET)
ament_package()
