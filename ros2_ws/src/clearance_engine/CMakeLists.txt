cmake_minimum_required(VERSION 3.8)
project(clearance_engine)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(interfaces REQUIRED)
find_package(PCL 1.12 REQUIRED COMPONENTS
  common
  features
  filters
  sample_consensus
  search
  segmentation
)
find_package(rclcpp REQUIRED)
find_package(rcl_interfaces REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)

add_library(clearance_engine_core
  src/clearance_estimator.cpp
  src/surface_candidate.cpp
  src/surface_detector.cpp
)
target_include_directories(clearance_engine_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${PCL_INCLUDE_DIRS}
)
target_link_libraries(clearance_engine_core
  ${PCL_LIBRARIES}
)

add_executable(clearance_engine_node
  src/clearance_engine_node.cpp
)
target_link_libraries(clearance_engine_node
  clearance_engine_core
)
ament_target_dependencies(clearance_engine_node
  interfaces
  rclcpp
  rcl_interfaces
  sensor_msgs
  std_msgs
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(clearance_engine_core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(clearance_engine_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
  TARGETS
    clearance_engine_core
    clearance_engine_node
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
  find_package(launch_testing_ament_cmake REQUIRED)
  ament_add_gtest(
    test_clearance_estimator
    test/test_clearance_estimator.cpp
  )
  target_link_libraries(
    test_clearance_estimator
    clearance_engine_core
  )
  ament_add_gtest(
    test_surface_detector
    test/test_surface_detector.cpp
  )
  target_link_libraries(
    test_surface_detector
    clearance_engine_core
  )
  add_launch_test(
    test/test_clearance_engine_node.py
    TIMEOUT 30
  )
endif()

ament_export_include_directories(include)
ament_package()
