#pragma once

#include <cstdint>
#include "common_types.hpp"

void registerDepthCUDA(
    const uint16_t* d_depth_in,
    unsigned int* d_z_buffer,
    uint16_t* d_depth_out,
    uint8_t* d_valid_in,
    uint8_t* d_valid_out,
    int depth_w,
    int depth_h,
    int color_w,
    int color_h
);

void updateConstants(
    Intrinsics depth_K,
    Intrinsics color_K,
    Extrinsics T_color_depth
);