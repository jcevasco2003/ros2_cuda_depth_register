cmake_minimum_required(VERSION 3.8)

project(cuda_depth_register)

# ---- CUDA setup (IGUAL al que funciona) ----
set(CMAKE_CUDA_HOST_COMPILER /usr/bin/gcc-12)
set(CMAKE_CUDA_COMPILER "/usr/local/cuda/bin/nvcc")
enable_language(CUDA)

set(CMAKE_CXX_STANDARD 17)

# ---- dependencies ----
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(image_transport REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(tf2_geometry_msgs REQUIRED)
find_package(CUDAToolkit REQUIRED)

# ---- includes ----
include_directories(
  include
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# ---- CUDA library ----
add_library(depth_register_cuda
  src/depth_register.cu
)

target_include_directories(depth_register_cuda
  PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(depth_register_cuda PROPERTIES
  CUDA_STANDARD 17
  CUDA_STANDARD_REQUIRED ON
)

# ---- executable ----
add_executable(cuda_depth_register_node
  src/cuda_depth_register_node.cpp
)

target_link_libraries(cuda_depth_register_node
  depth_register_cuda
  CUDA::cudart
)

ament_target_dependencies(cuda_depth_register_node
  rclcpp
  sensor_msgs
  cv_bridge
  image_transport
  tf2
  tf2_ros
  tf2_geometry_msgs
)

# ---- install ----
install(TARGETS cuda_depth_register_node
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()