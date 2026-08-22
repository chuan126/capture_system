cmake_minimum_required(VERSION 3.8)
project(localization)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(interfaces REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(PCL 1.12 REQUIRED COMPONENTS common filters registration search)
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
  src/heading_rigid_alignment.cpp
  src/odometry_buffer.cpp
  src/similarity_alignment.cpp
  src/dead_reckoning.cpp
)
target_include_directories(localization_dead_reckoning_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_library(localization_fusion_core
  src/finite_attitude_correction.cpp
  src/fusion_navigator.cpp
  src/lidar_localizer.cpp
  src/sensor_synchronizer.cpp
)
target_include_directories(localization_fusion_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${PCL_INCLUDE_DIRS}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Attitude
)
target_link_libraries(localization_fusion_core
  PUBLIC
    Eigen3::Eigen
    localization_attitude_transform
    ${PCL_LIBRARIES}
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
ament_target_dependencies(dead_reckoning_node interfaces nav_msgs rclcpp sensor_msgs)

add_executable(fusion_navigation_node
  src/fusion_navigation_node.cpp
)
target_include_directories(fusion_navigation_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/Attitude
  ${PCL_INCLUDE_DIRS}
)
target_link_libraries(
  fusion_navigation_node
  localization_dead_reckoning_core
  localization_fusion_core
  localization_attitude_transform
)
ament_target_dependencies(
  fusion_navigation_node builtin_interfaces diagnostic_msgs geometry_msgs interfaces nav_msgs
  rclcpp sensor_msgs)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(attitude_matrix PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(localization_attitude_transform PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(localization_dead_reckoning_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(localization_fusion_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(dead_reckoning_node PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(fusion_navigation_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
  TARGETS
    attitude_matrix
    localization_attitude_transform
    localization_dead_reckoning_core
    localization_fusion_core
  EXPORT export_localization_attitude_matrix
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
install(TARGETS dead_reckoning_node fusion_navigation_node DESTINATION lib/${PROJECT_NAME})
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

  ament_add_gtest(
    test_heading_rigid_alignment
    test/test_heading_rigid_alignment.cpp
  )
  target_link_libraries(test_heading_rigid_alignment localization_dead_reckoning_core)

  ament_add_gtest(
    test_finite_attitude_correction
    test/test_finite_attitude_correction.cpp
  )
  target_link_libraries(test_finite_attitude_correction localization_fusion_core)

  ament_add_gtest(
    test_fusion_navigator
    test/test_fusion_navigator.cpp
  )
  target_link_libraries(test_fusion_navigator localization_fusion_core)

  ament_add_gtest(
    test_sensor_synchronizer
    test/test_sensor_synchronizer.cpp
  )
  target_link_libraries(test_sensor_synchronizer localization_fusion_core)

  ament_add_gtest(
    test_lidar_localizer
    test/test_lidar_localizer.cpp
  )
  target_link_libraries(test_lidar_localizer localization_fusion_core)
endif()

ament_export_include_directories(include)
ament_export_libraries(attitude_matrix localization_attitude_transform localization_dead_reckoning_core localization_fusion_core)
ament_export_targets(export_localization_attitude_matrix HAS_LIBRARY_TARGET)
ament_export_dependencies(Eigen3 PCL)
ament_package()
