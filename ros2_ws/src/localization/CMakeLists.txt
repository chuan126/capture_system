cmake_minimum_required(VERSION 3.8)
project(localization)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)

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

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(attitude_matrix PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(localization_attitude_transform PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
  TARGETS
    attitude_matrix
    localization_attitude_transform
  EXPORT export_localization_attitude_matrix
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)
install(
  DIRECTORY include/
  DESTINATION include
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
endif()

ament_export_include_directories(include)
ament_export_targets(export_localization_attitude_matrix HAS_LIBRARY_TARGET)
ament_package()
