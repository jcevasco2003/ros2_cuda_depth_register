# CUDA Depth Register

ROS2 node for GPU-accelerated registration of depth images into the RGB camera frame using CUDA.

This package was developed as a supporting component for VIGS-Fusion LC and performs depth-to-color reprojection on the GPU, allowing downstream SLAM and reconstruction pipelines to consume depth measurements aligned with RGB imagery.

---

## Features

* CUDA-accelerated depth registration.
* ROS2 native implementation.
* Supports RGB-D camera setups with distinct depth and color optical frames.
* Uses TF2 transforms between camera frames.
* Publishes depth images already aligned to the RGB camera frame.
* Designed for real-time operation.

---

## Requirements

### Software

* ROS2 Rolling
* CUDA Toolkit 12.x
* OpenCV
* cv_bridge
* image_transport
* TF2

### Build Dependencies

```bash
sudo apt install \
    ros-rolling-cv-bridge \
    ros-rolling-image-transport \
    ros-rolling-tf2 \
    ros-rolling-tf2-ros \
    ros-rolling-tf2-geometry-msgs
```

---

## CUDA Configuration

The package is currently configured for:

* CUDA Toolkit
* NVCC
* GCC 12 host compiler
* C++17
* CUDA C++17

The relevant configuration in CMake is:

```cmake
set(CMAKE_CUDA_HOST_COMPILER /usr/bin/gcc-12)
set(CMAKE_CUDA_COMPILER "/usr/local/cuda/bin/nvcc")
enable_language(CUDA)
```

If your CUDA installation is located elsewhere, update these paths accordingly.

---

## Build

Clone the package inside a ROS2 workspace:

```bash
cd ~/ros2_ws/src

git clone git@github.com:jcevasco2003/ros2_cuda_depth_register.git
```

Build with colcon:

```bash
cd ~/ros2_ws

colcon build --symlink-install
```

Source the workspace:

```bash
source install/setup.bash
```

---

## Running

Launch the node:

```bash
ros2 run cuda_depth_register cuda_depth_register_node
```

---

## Inputs

The node expects:

* RGB image stream
* Depth image stream
* Camera calibration parameters
* TF transform between depth and RGB optical frames

---

## Output

The node publishes:

* Depth image registered to the RGB camera frame

This output can be directly consumed by RGB-D SLAM, visual-inertial SLAM, dense mapping, or 3D reconstruction systems.

---

## Use Case

This package was originally developed to support:

* VIGS-Fusion LC
---

## License

MIT License
