cmake_minimum_required(VERSION 3.8)
project(sensor_adapter)

find_package(ament_cmake REQUIRED)

install(
  DIRECTORY launch rviz
  DESTINATION share/${PROJECT_NAME}
  PATTERN "__pycache__" EXCLUDE
)

if(BUILD_TESTING)
  find_package(ament_cmake_pytest REQUIRED)
  ament_add_pytest_test(
    test_launch_files
    test/test_launch_files.py
    TIMEOUT 30
  )
endif()

ament_package()
