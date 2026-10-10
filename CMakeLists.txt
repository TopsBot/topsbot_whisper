cmake_minimum_required(VERSION 3.8)
project(topsbot_whisper_asr)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(std_msgs REQUIRED)

if(CMAKE_SYSROOT)
  set(TOPSBOT_WHISPER_SYSTEM_ROOT "${CMAKE_SYSROOT}/usr/local")
else()
  set(TOPSBOT_WHISPER_SYSTEM_ROOT "/usr/local")
endif()
set(TOPSBOT_WHISPER_SYSTEM_INCLUDE "${TOPSBOT_WHISPER_SYSTEM_ROOT}/include")
set(TOPSBOT_WHISPER_SYSTEM_LIB "${TOPSBOT_WHISPER_SYSTEM_ROOT}/lib")

# Pure audio/decoding logic, linkable by tests on the host as well as on target.
add_library(topsbot_whisper_utils SHARED
  src/whisper_audio.cpp
)
target_include_directories(topsbot_whisper_utils PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# RAII wrapper around ta-llmruntime (engine lifecycle + generate).
add_library(topsbot_whisper_engine SHARED
  src/whisper_engine.cpp
)
target_include_directories(topsbot_whisper_engine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  PRIVATE
  ${TOPSBOT_WHISPER_SYSTEM_INCLUDE}
  ${TOPSBOT_WHISPER_SYSTEM_INCLUDE}/ta-llmruntime
)
target_link_directories(topsbot_whisper_engine PUBLIC
  ${TOPSBOT_WHISPER_SYSTEM_LIB}
)
target_link_libraries(topsbot_whisper_engine
  ta_llm
)

# Independent ROS2 ASR node.
add_executable(whisper_asr_node
  src/whisper_asr_node.cpp
)
target_include_directories(whisper_asr_node PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  ${TOPSBOT_WHISPER_SYSTEM_INCLUDE}
  ${TOPSBOT_WHISPER_SYSTEM_INCLUDE}/ta-llmruntime
)
target_link_libraries(whisper_asr_node
  topsbot_whisper_engine
  topsbot_whisper_utils
)
ament_target_dependencies(whisper_asr_node
  rclcpp
  std_msgs
)

# Offline CLI: transcribe a single s16le PCM file through WhisperEngine.
add_executable(whisper_file_cli
  src/whisper_file_cli.cpp
)
target_include_directories(whisper_file_cli PRIVATE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  ${TOPSBOT_WHISPER_SYSTEM_INCLUDE}
  ${TOPSBOT_WHISPER_SYSTEM_INCLUDE}/ta-llmruntime
)
target_link_libraries(whisper_file_cli
  topsbot_whisper_engine
  topsbot_whisper_utils
)

install(TARGETS
  topsbot_whisper_utils
  topsbot_whisper_engine
  whisper_asr_node
  whisper_file_cli
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)
install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config launch DESTINATION share/${PROJECT_NAME})
install(PROGRAMS
  scripts/ensure_models.sh
  scripts/download_models.sh
  scripts/model_manager.py
  DESTINATION share/${PROJECT_NAME}/scripts
)
# audio_source_node.py / mic_source_node.py are launchable executables
# (ros2 launch `executable=`), so they must live in lib/<pkg>, not share/scripts.
install(PROGRAMS
  scripts/audio_source_node.py
  scripts/mic_source_node.py
  DESTINATION lib/${PROJECT_NAME}
)
install(FILES README.md DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_whisper_audio test/test_whisper_audio.cpp)
  if(TARGET test_whisper_audio)
    target_link_libraries(test_whisper_audio topsbot_whisper_utils)
  endif()
endif()

ament_export_include_directories(include)
ament_export_libraries(
  topsbot_whisper_utils
  topsbot_whisper_engine)
ament_package()
