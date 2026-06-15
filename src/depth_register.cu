#include <limits>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "cuda_depth_register/depth_register.hpp"
#include <cuda_runtime.h>

__constant__ Intrinsics d_depth_K;
__constant__ Intrinsics d_color_K;
__constant__ Extrinsics d_T_color_depth;

__global__ void initZBuffer(
    unsigned int* buffer,
    unsigned int value,
    int width,
    int height)
{
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    int v = blockIdx.y * blockDim.y + threadIdx.y;

    if (u >= width || v >= height) return;

    int idx = v * width + u;
    buffer[idx] = value;
}

__global__ void registerKernel(
    const uint16_t* __restrict__ depth_in,
    unsigned int* __restrict__ z_buffer,
    int depth_w,
    int depth_h,
    int color_w,
    int color_h)
{
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    int v = blockIdx.y * blockDim.y + threadIdx.y;

    if (u >= depth_w || v >= depth_h) return;

    int idx = v * depth_w + u;

    uint16_t d = depth_in[idx];
    if (d == 0) return;

    float z = d * 0.001f;
    if (!isfinite(z) || z <= 0.0f) return;

    // Backprojection para obtener coordenadas 3D en el sistema de la cámara de profundidad
    float x = (u - d_depth_K.cx) * z * d_depth_K.inv_fx;
    float y = (v - d_depth_K.cy) * z * d_depth_K.inv_fy;

    // Transform para llevar al sistema de la cámara de color
    float x_c = fmaf(d_T_color_depth.R[0], x,
                fmaf(d_T_color_depth.R[1], y,
                fmaf(d_T_color_depth.R[2], z,
                     d_T_color_depth.t[0])));

    float y_c = fmaf(d_T_color_depth.R[3], x,
                fmaf(d_T_color_depth.R[4], y,
                fmaf(d_T_color_depth.R[5], z,
                     d_T_color_depth.t[1])));

    float z_c = fmaf(d_T_color_depth.R[6], x,
                fmaf(d_T_color_depth.R[7], y,
                fmaf(d_T_color_depth.R[8], z,
                     d_T_color_depth.t[2])));

    if (z_c <= 0.0f) return;

    // Projection para obtener coordenadas de píxel en la imagen de color
    float inv_z = 1.0f / z_c;

    int u_c = __float2int_rn(x_c * inv_z * d_color_K.fx + d_color_K.cx);
    int v_c = __float2int_rn(y_c * inv_z * d_color_K.fy + d_color_K.cy);

    if (u_c < 0 || u_c >= color_w || v_c < 0 || v_c >= color_h) return;

    int out_idx = v_c * color_w + u_c;

    unsigned int z_uint = __float_as_uint(z_c);
    atomicMin(&z_buffer[out_idx], z_uint);
}

__global__ void finalizeKernel(
    const unsigned int* z_buffer,
    uint16_t* depth_out,
    uint8_t* valid_in,
    uint8_t* valid_out,
    int width,
    int height)
{
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    int v = blockIdx.y * blockDim.y + threadIdx.y;

    if (u >= width || v >= height) return;

    int idx = v * width + u;

    float z = __uint_as_float(z_buffer[idx]);

    if (isfinite(z)) {
        depth_out[idx] = (uint16_t)(z * 1000.0f);
    } else {
        depth_out[idx] = 0;
    }

    valid_out[idx] = valid_in[idx] = (depth_out[idx] != 0) ? 1 : 0;
}

__global__ void holeFillingKernel(
    uint16_t* depth,
    uint8_t* valid_in,
    uint8_t* valid_out,
    int r,
    int width,
    int height)
{
    int u = blockIdx.x * blockDim.x + threadIdx.x;
    int v = blockIdx.y * blockDim.y + threadIdx.y;

    if (u >= width || v >= height) return;

    int idx = v * width + u;

    if (valid_in[idx]) return;

    uint16_t best_depth = 0;
    
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx == 0 && dy == 0) continue;
            
            int nu = u + dx;
            int nv = v + dy;

            int neighbor_idx = nv * width + nu;

            if (nu < 0 || nu >= width || nv < 0 || nv >= height) continue;

            if (valid_in[neighbor_idx] == 0) continue; // Solo consideramos vecinos válidos

            uint16_t neighbor_depth = depth[neighbor_idx];
            if (neighbor_depth != 0) {
                if (best_depth == 0 || neighbor_depth < best_depth) {
                    best_depth = neighbor_depth;
                }
            }
        }
    }
    if (best_depth != 0) {
        depth[idx] = best_depth;
        valid_out[idx] = 1; // Marcamos como válido el píxel rellenado
    }
}

void updateConstants(
    Intrinsics depth_K,
    Intrinsics color_K,
    Extrinsics T_color_depth)
{
    cudaMemcpyToSymbol(d_depth_K, &depth_K, sizeof(Intrinsics));
    cudaMemcpyToSymbol(d_color_K, &color_K, sizeof(Intrinsics));
    cudaMemcpyToSymbol(d_T_color_depth, &T_color_depth, sizeof(Extrinsics));
}

bool measureTime = false;

void registerDepthCUDA(
    const uint16_t* d_depth_in,
    unsigned int* d_z_buffer,
    uint16_t* d_depth_out,
    uint8_t* d_valid_in,
    uint8_t* d_valid_out,
    int depth_w,
    int depth_h,
    int color_w,
    int color_h)
{
    float inf = std::numeric_limits<float>::infinity();
    unsigned int inf_uint;
    memcpy(&inf_uint, &inf, sizeof(float));

    dim3 block(16, 16);

    dim3 grid_color(
        (color_w + block.x - 1) / block.x,
        (color_h + block.y - 1) / block.y
    );

    dim3 grid_depth(
        (depth_w + block.x - 1) / block.x,
        (depth_h + block.y - 1) / block.y
    );

    // =========================
    // CUDA EVENTS
    // =========================
    cudaEvent_t start_total, stop_total;
    cudaEvent_t start, stop;

    float ms = 0.0f;
    if (measureTime) {
        cudaEventCreate(&start_total);
        cudaEventCreate(&stop_total);
        cudaEventCreate(&start);
        cudaEventCreate(&stop);

        cudaEventRecord(start_total);
    }
    
    // =========================
    // initZBuffer
    // =========================
    if (measureTime) {cudaEventRecord(start);}


    initZBuffer<<<grid_color, block>>>(
        d_z_buffer,
        inf_uint,
        color_w,
        color_h
    );

    if (measureTime) {
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        cudaEventElapsedTime(&ms, start, stop);
        printf("[CUDA] initZBuffer: %.3f ms\n", ms);
    }

    // =========================
    // registerKernel
    // =========================
    if (measureTime) {cudaEventRecord(start);}

    registerKernel<<<grid_depth, block>>>(
        d_depth_in,
        d_z_buffer,
        depth_w,
        depth_h,
        color_w,
        color_h
    );

    if (measureTime) {
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        cudaEventElapsedTime(&ms, start, stop);
        printf("[CUDA] registerKernel: %.3f ms\n", ms);
    }

    // =========================
    // finalizeKernel
    // =========================
    if (measureTime) {cudaEventRecord(start);}

    finalizeKernel<<<grid_color, block>>>(
        d_z_buffer,
        d_depth_out,
        d_valid_in,
        d_valid_out,
        color_w,
        color_h
    );

    if (measureTime) {
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);
        cudaEventElapsedTime(&ms, start, stop);
        printf("[CUDA] finalizeKernel: %.3f ms\n", ms);
    }

    // =========================
    // holeFillingKernel
    // =========================
    for (int i = 0; i < 2; ++i) {

        if (measureTime) {cudaEventRecord(start);}

        holeFillingKernel<<<grid_color, block>>>(
            d_depth_out,
            d_valid_in,
            d_valid_out,
            3,
            color_w,
            color_h
        );

        if (measureTime) {
            cudaEventRecord(stop);
            cudaEventSynchronize(stop);
            cudaEventElapsedTime(&ms, start, stop);
            printf("[CUDA] holeFillingKernel iter %d: %.3f ms\n", i, ms);
        }

        uint8_t* temp = d_valid_in;
        d_valid_in = d_valid_out;
        d_valid_out = temp;
    }

    // =========================
    // TOTAL
    // =========================
    if (measureTime) {
        cudaEventRecord(stop_total);
        cudaEventSynchronize(stop_total);

        cudaEventElapsedTime(&ms, start_total, stop_total);
        printf("[CUDA] TOTAL: %.3f ms\n", ms);
    
        // =========================
        // CLEANUP
        // =========================
        cudaEventDestroy(start_total);
        cudaEventDestroy(stop_total);
        cudaEventDestroy(start);
        cudaEventDestroy(stop);
    }
}