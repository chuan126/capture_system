cmake_minimum_required(VERSION 3.8)
project(rtk_driver LANGUAGES C CXX)

if(NOT CMAKE_C_STANDARD)
  set(CMAKE_C_STANDARD 11)
endif()
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(interfaces REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

# 已验证的解析器独立构建，业务外围不得修改或另写一套解析逻辑。
add_library(gnss_nmea STATIC
  NMEA0183/gnss_nmea.c
)
target_include_directories(gnss_nmea
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/NMEA0183>
)
target_link_libraries(gnss_nmea PUBLIC m)

add_library(rtk_serial_port
  src/serial_port.cpp
)
target_include_directories(rtk_serial_port
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_executable(rtk_driver_node
  src/rtk_driver_node.cpp
)
target_link_libraries(rtk_driver_node
  gnss_nmea
  rtk_serial_port
)
ament_target_dependencies(rtk_driver_node
  diagnostic_msgs
  interfaces
  rclcpp
  sensor_msgs
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rtk_serial_port PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(rtk_driver_node PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(
  TARGETS
    gnss_nmea
    rtk_serial_port
    rtk_driver_node
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config launch DESTINATION share/${PROJECT_NAME})
install(
  FILES NMEA0183/gnss_nmea.h
  DESTINATION include/${PROJECT_NAME}/NMEA0183
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_gnss_nmea test/test_gnss_nmea.cpp)
  target_link_libraries(test_gnss_nmea gnss_nmea)

  ament_add_gtest(test_serial_port test/test_serial_port.cpp)
  target_link_libraries(test_serial_port rtk_serial_port util)

  add_test(
    NAME test_gnss_nmea_immutable
    COMMAND ${CMAKE_COMMAND}
      -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/验证GNSS解析器哈希.cmake
  )
endif()

ament_export_include_directories(include)
ament_package()
