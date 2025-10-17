# main/CMakeLists.txt
idf_component_register(
  SRCS "hello_world_main.c"
  INCLUDE_DIRS "."
  REQUIRES freertos esp_system esp_timer
)
