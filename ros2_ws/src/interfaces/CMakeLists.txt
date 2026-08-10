cmake_minimum_required(VERSION 3.8)
project(interfaces)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_msgs REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/ClearanceResult.msg"
  "msg/LocalizationStatus.msg"
  "msg/RtkStatus.msg"
  "msg/TaskStatus.msg"
  "msg/RecordingStatus.msg"
  "srv/StartTask.srv"
  "srv/TaskCommand.srv"
  "srv/PrepareRecording.srv"
  "srv/RecordingCommand.srv"
  DEPENDENCIES std_msgs
)

ament_export_dependencies(rosidl_default_runtime)
ament_package()
