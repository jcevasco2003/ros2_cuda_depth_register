#pragma once

struct alignas(16) Intrinsics
{
    float fx, fy;
    float cx, cy;
    float inv_fx, inv_fy;
};

struct alignas(16) Extrinsics
{
    float R[9]; // row-major
    float t[3];
};