#include "ggml-opencl.h"

#include <array>
#include <atomic>
#include <sstream>
#include <vector>
#include <limits>

#define CL_TARGET_OPENCL_VERSION 120
// clCreateCommandQueue
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <clblast.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "ggml.h"

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

#define CL_DMMV_BLOCK_SIZE 32

#ifndef K_QUANTS_PER_ITERATION
#define K_QUANTS_PER_ITERATION 1
#else
static_assert(K_QUANTS_PER_ITERATION == 1 || K_QUANTS_PER_ITERATION == 2, "K_QUANTS_PER_ITERATION must be 1 or 2");
#endif

#define MULTILINE_QUOTE(...) #__VA_ARGS__
namespace {
std::string program_source = MULTILINE_QUOTE(

typedef char int8_t;
typedef uchar uint8_t;
typedef short int16_t;
typedef ushort uint16_t;
typedef int int32_t;
typedef uint uint32_t;

struct __attribute__ ((packed)) block_q4_0
{
    half d;
    uint8_t qs[QK4_0 / 2];
};

struct __attribute__ ((packed)) block_q4_1
{
    half d;
    half m;
    uint8_t qs[QK4_1 / 2];
};

struct __attribute__ ((packed)) block_q5_0
{
    half d;
    uint32_t qh;
    uint8_t qs[QK5_0 / 2];
};

struct __attribute__ ((packed)) block_q5_1
{
    half d;
    half m;
    uint32_t qh;
    uint8_t qs[QK5_1 / 2];
};

struct __attribute__ ((packed)) block_q8_0
{
    half d;
    int8_t qs[QK8_0];
};

struct __attribute__((packed)) block_q2_K
{
    uint8_t scales[16];
    uint8_t qs[64];
    half d;
    half dmin;
};

struct __attribute__((packed)) block_q3_K
{
    uint8_t hmask[32];
    uint8_t qs[64];
    uint8_t scales[12];
    half d;
};

struct __attribute__((packed)) block_q4_K
{
    half d;
    half dmin;
    uint8_t scales[12];
    uint8_t qs[128];
};

struct __attribute__((packed)) block_q5_K
{
    half d;
    half dmin;
    uint8_t scales[12];
    uint8_t qh[32];
    uint8_t qs[128];
};

struct __attribute__((packed)) block_q6_K
{
    uint8_t ql[128];
    uint8_t qh[64];
    int8_t scales[16];
    half d;
};

void dequantize_q4_0(__global const struct block_q4_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);

    const uint8_t vui = x[ib].qs[iqs];

    const int8_t vi0 = vui & 0xF;
    const int8_t vi1 = vui >> 4;

    *v0 = (vi0 - 8)*d;
    *v1 = (vi1 - 8)*d;
}
void dequantize_q4_1(__global const struct block_q4_1* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);
    const float m = vload_half(0, &x[ib].m);

    const uint8_t vui = x[ib].qs[iqs];

    const int8_t vi0 = vui & 0xF;
    const int8_t vi1 = vui >> 4;

    *v0 = vi0*d + m;
    *v1 = vi1*d + m;
}
void dequantize_q5_0(__global const struct block_q5_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);

    uint32_t qh = x[ib].qh;

    const uint8_t xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const uint8_t xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    const int32_t x0 = ((x[ib].qs[iqs] & 0xf) | xh_0) - 16;
    const int32_t x1 = ((x[ib].qs[iqs] >>  4) | xh_1) - 16;

    *v0 = x0*d;
    *v1 = x1*d;
}
void dequantize_q5_1(__global const struct block_q5_1* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);
    const float m = vload_half(0, &x[ib].m);

    uint32_t qh = x[ib].qh;

    const uint8_t xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const uint8_t xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    const int32_t x0 = ((x[ib].qs[iqs] & 0xf) | xh_0);
    const int32_t x1 = ((x[ib].qs[iqs] >>  4) | xh_1);

    *v0 = x0*d + m;
    *v1 = x1*d + m;
}
void dequantize_q8_0(__global const struct block_q8_0* x, const int ib, const int iqs, float* v0, float* v1) {
    const float d = vload_half(0, &x[ib].d);

    const int8_t vi0 = x[ib].qs[iqs + 0];
    const int8_t vi1 = x[ib].qs[iqs + 1];

    *v0 = vi0*d;
    *v1 = vi1*d;
}
void convert_f16(__global const half* x, const int ib, const int iqs, float* v0, float* v1){
    *v0 = vload_half(0, &x[ib + 0]);
    *v1 = vload_half(0, &x[ib + 1]);
}
);

static std::string k_quants_source = MULTILINE_QUOTE(
inline void get_scale_min_k4(int j, const __global uint8_t *q, uint8_t *d, uint8_t *m)
{
    if (j < 4)
    {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    }
    else
    {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

__kernel void dequantize_block_q2_K(__global const struct block_q2_K *x, __global float *yy)
{
    const int i = get_group_id(0);
    const int tid = get_local_id(0);
    const int n = tid / 32;
    const int l = tid - 32 * n;
    const int is = 8 * n + l / 16;

    const uint8_t q = x[i].qs[32 * n + l];
    __global float *y = yy + i * QK_K + 128 * n;

    const float dall = vload_half(0, &x[i].d);
    const float dmin = vload_half(0, &x[i].dmin);

    y[l + 0] = dall * (x[i].scales[is + 0] & 0xF) * ((q >> 0) & 3) - dmin * (x[i].scales[is + 0] >> 4);
    y[l + 32] = dall * (x[i].scales[is + 2] & 0xF) * ((q >> 2) & 3) - dmin * (x[i].scales[is + 2] >> 4);
    y[l + 64] = dall * (x[i].scales[is + 4] & 0xF) * ((q >> 4) & 3) - dmin * (x[i].scales[is + 4] >> 4);
    y[l + 96] = dall * (x[i].scales[is + 6] & 0xF) * ((q >> 6) & 3) - dmin * (x[i].scales[is + 6] >> 4);
}

__kernel void dequantize_block_q3_K(__global const struct block_q3_K *x, __global float *yy)
{
    int r = get_local_id(0) / 4;
    int i = get_group_id(0);
    int tid = r / 2;
    int is0 = r % 2;
    int l0 = 16 * is0 + 4 * (get_local_id(0) % 4);
    int n = tid / 4;
    int j = tid - 4 * n;

    uint8_t m = 1 << (4 * n + j);
    int is = 8 * n + 2 * j + is0;
    int shift = 2 * j;

    int8_t us = is < 4 ? (x[i].scales[is - 0] & 0xF) | (((x[i].scales[is + 8] >> 0) & 3) << 4)
              : is < 8 ? (x[i].scales[is - 0] & 0xF) | (((x[i].scales[is + 4] >> 2) & 3) << 4)
              : is < 12  ? (x[i].scales[is - 8] >> 4) | (((x[i].scales[is + 0] >> 4) & 3) << 4)
              : (x[i].scales[is - 8] >> 4) | (((x[i].scales[is - 4] >> 6) & 3) << 4);
    float d_all = vload_half(0, &x[i].d);
    float dl = d_all * (us - 32);

    __global float *y = yy + i * QK_K + 128 * n + 32 * j;
    const __global uint8_t *q = x[i].qs + 32 * n;
    const __global uint8_t *hm = x[i].hmask;

    for (int l = l0; l < l0 + 4; ++l)
        y[l] = dl * ((int8_t)((q[l] >> shift) & 3) - ((hm[l] & m) ? 0 : 4));
}

__kernel void dequantize_block_q4_K(__global const struct block_q4_K *x, __global float *yy)
{
    const int i = get_group_id(0);
    const int tid = get_local_id(0);
    const int il = tid / 8;
    const int ir = tid % 8;
    const int is = 2 * il;
    const int n = 4;

    __global float *y = yy + i * QK_K + 64 * il + n * ir;

    const float dall = vload_half(0, &x[i].d);
    const float dmin = vload_half(0, &x[i].dmin);

    __global const uint8_t *q = x[i].qs + 32 * il + n * ir;

    uint8_t sc, m;
    get_scale_min_k4(is + 0, x[i].scales, &sc, &m);
    float d1 = dall * sc;
    float m1 = dmin * m;
    get_scale_min_k4(is + 1, x[i].scales, &sc, &m);
    float d2 = dall * sc;
    float m2 = dmin * m;
    for (int l = 0; l < n; ++l)
    {
        y[l + 0] = d1 * (q[l] & 0xF) - m1;
        y[l + 32] = d2 * (q[l] >> 4) - m2;
    }
}

__kernel void dequantize_block_q5_K(__global const struct block_q5_K *x, __global float *yy)
{
    const int i = get_group_id(0);
    const int tid = get_local_id(0);
    const int il = tid / 16;
    const int ir = tid % 16;
    const int is = 2 * il;

    __global float *y = yy + i * QK_K + 64 * il + 2 * ir;

    const float dall = vload_half(0, &x[i].d);
    const float dmin = vload_half(0, &x[i].dmin);

    __global const uint8_t *ql = x[i].qs + 32 * il + 2 * ir;
    __global const uint8_t *qh = x[i].qh + 2 * ir;

    uint8_t sc, m;
    get_scale_min_k4(is + 0, x[i].scales, &sc, &m);
    const float d1 = dall * sc;
    const float m1 = dmin * m;
    get_scale_min_k4(is + 1, x[i].scales, &sc, &m);
    const float d2 = dall * sc;
    const float m2 = dmin * m;

    uint8_t hm = 1 << (2 * il);
    y[0] = d1 * ((ql[0] & 0xF) + (qh[0] & hm ? 16 : 0)) - m1;
    y[1] = d1 * ((ql[1] & 0xF) + (qh[1] & hm ? 16 : 0)) - m1;
    hm <<= 1;
    y[32] = d2 * ((ql[0] >> 4) + (qh[0] & hm ? 16 : 0)) - m2;
    y[33] = d2 * ((ql[1] >> 4) + (qh[1] & hm ? 16 : 0)) - m2;
}

__kernel void dequantize_block_q6_K(__global const struct block_q6_K *x, __global float *yy)
{
    const int i = get_group_id(0);
    const int tid = get_local_id(0);
    const int ip = tid / 32;
    const int il = tid - 32 * ip;
    const int is = 8 * ip + il / 16;

    __global float *y = yy + i * QK_K + 128 * ip + il;

    const float d = vload_half(0, &x[i].d);

    __global const uint8_t *ql = x[i].ql + 64 * ip + il;
    const uint8_t qh = x[i].qh[32 * ip + il];
    __global const int8_t *sc = x[i].scales + is;

    y[0] = d * sc[0] * ((int8_t)((ql[0] & 0xF) | (((qh >> 0) & 3) << 4)) - 32);
    y[32] = d * sc[2] * ((int8_t)((ql[32] & 0xF) | (((qh >> 2) & 3) << 4)) - 32);
    y[64] = d * sc[4] * ((int8_t)((ql[0] >> 4) | (((qh >> 4) & 3) << 4)) - 32);
    y[96] = d * sc[6] * ((int8_t)((ql[32] >> 4) | (((qh >> 6) & 3) << 4)) - 32);
}

__kernel void dequantize_mul_mat_vec_q2_K(__global const struct block_q2_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {

    const int row = get_group_id(0);

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    __global const struct block_q2_K * x = xx + ib0;

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...31 or 0...15
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;  // 0 or 0,1

    const int step = 16/K_QUANTS_PER_ITERATION;

    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0...15 or 0...7

    const int l0 = K_QUANTS_PER_ITERATION*in;            // 0...15 or 0...14 in steps of 2
    const int q_offset = 32*im + l0;
    const int s_offset = 8*im;
    const int y_offset = 128*im + l0;

    tmp[16 * ix + tid] = 0;

    uint32_t aux[4];
    const uint8_t * d = (const uint8_t *)aux;
    const uint8_t * m = (const uint8_t *)(aux + 2);

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const float   * y = yy + i * QK_K + y_offset;
        __global const uint8_t * q = x[i].qs + q_offset;

        const float dall = vload_half(0, &x[i].d);
        const float dmin = vload_half(0, &x[i].dmin);

        __global const uint32_t * a = (__global const uint32_t *)(x[i].scales + s_offset);
        aux[0] = a[0] & 0x0f0f0f0f;
        aux[1] = a[1] & 0x0f0f0f0f;
        aux[2] = (a[0] >> 4) & 0x0f0f0f0f;
        aux[3] = (a[1] >> 4) & 0x0f0f0f0f;

        float sum1 = 0, sum2 = 0;
        for (int l = 0; l < K_QUANTS_PER_ITERATION; ++l) {
            sum1 += y[l+ 0] * d[0] * ((q[l+ 0] >> 0) & 3)
                  + y[l+32] * d[2] * ((q[l+ 0] >> 2) & 3)
                  + y[l+64] * d[4] * ((q[l+ 0] >> 4) & 3)
                  + y[l+96] * d[6] * ((q[l+ 0] >> 6) & 3)
                  + y[l+16] * d[1] * ((q[l+16] >> 0) & 3)
                  + y[l+48] * d[3] * ((q[l+16] >> 2) & 3)
                  + y[l+80] * d[5] * ((q[l+16] >> 4) & 3)
                  +y[l+112] * d[7] * ((q[l+16] >> 6) & 3);
            sum2 += y[l+ 0] * m[0] + y[l+32] * m[2] + y[l+64] * m[4] + y[ l+96] * m[6]
                  + y[l+16] * m[1] + y[l+48] * m[3] + y[l+80] * m[5] + y[l+112] * m[7];

        }
        tmp[16 * ix + tid] += dall * sum1 - dmin * sum2;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q3_K(__global const struct block_q3_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {
    const uint16_t kmask1 = 0x0303;
    const uint16_t kmask2 = 0x0f0f;

    const int row = get_group_id(0);

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    __global const struct block_q3_K * x = xx + ib0;

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...31 or 0...16
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;  // 0 or 0,1

    const int n  = K_QUANTS_PER_ITERATION;               // iterations in the inner loop
    const int step = 16/K_QUANTS_PER_ITERATION;
    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0....15 or 0...7

    const uint8_t m = 1 << (4*im);

    const int l0 = n*in;                                 // 0...15 or 0...14 in steps of 2
    const int q_offset =  32*im + l0;
    const int y_offset = 128*im + l0;

    uint16_t utmp[4];
    const int8_t * s = (const int8_t *)utmp;

    const uint16_t s_shift = 4*im;

    tmp[16 * ix + tid] = 0;

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const float   * y  = yy + i * QK_K + y_offset;
        __global const uint8_t * q = x[i].qs + q_offset;
        __global const uint8_t * h = x[i].hmask + l0;

        __global const uint16_t * a = (__global const uint16_t *)x[i].scales;
        utmp[0] = ((a[0] >> s_shift) & kmask2) | (((a[4] >> (s_shift + 0)) & kmask1) << 4);
        utmp[1] = ((a[1] >> s_shift) & kmask2) | (((a[5] >> (s_shift + 0)) & kmask1) << 4);
        utmp[2] = ((a[2] >> s_shift) & kmask2) | (((a[4] >> (s_shift + 2)) & kmask1) << 4);
        utmp[3] = ((a[3] >> s_shift) & kmask2) | (((a[5] >> (s_shift + 2)) & kmask1) << 4);

        const float d = vload_half(0, &x[i].d);

        float sum = 0;
        for (int l = 0; l < n; ++l) {
            sum += y[l+ 0] * (s[0] - 32) * (((q[l] >> 0) & 3) - (h[l] & (m << 0) ? 0 : 4))
                 + y[l+32] * (s[2] - 32) * (((q[l] >> 2) & 3) - (h[l] & (m << 1) ? 0 : 4))
                 + y[l+64] * (s[4] - 32) * (((q[l] >> 4) & 3) - (h[l] & (m << 2) ? 0 : 4))
                 + y[l+96] * (s[6] - 32) * (((q[l] >> 6) & 3) - (h[l] & (m << 3) ? 0 : 4));
            sum += y[l+16] * (s[1] - 32) * (((q[l+16] >> 0) & 3) - (h[l+16] & (m << 0) ? 0 : 4))
                 + y[l+48] * (s[3] - 32) * (((q[l+16] >> 2) & 3) - (h[l+16] & (m << 1) ? 0 : 4))
                 + y[l+80] * (s[5] - 32) * (((q[l+16] >> 4) & 3) - (h[l+16] & (m << 2) ? 0 : 4))
                + y[l+112] * (s[7] - 32) * (((q[l+16] >> 6) & 3) - (h[l+16] & (m << 3) ? 0 : 4));
        }
        tmp[16 * ix + tid] += d * sum;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q4_K(__global const struct block_q4_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {

    //to rename it later, just to test now
    const uint16_t kmask1 = 0x3f3f;
    const uint16_t kmask2 = 0x0f0f;
    const uint16_t kmask3 = 0xc0c0;

    const int row = get_group_id(0);
    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...15
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;

    const int step = 8/K_QUANTS_PER_ITERATION;

    const int il  = tid/step;     // 0...3
    const int ir  = tid - step*il;// 0...3
    const int n   = 2*K_QUANTS_PER_ITERATION;

    const int im = il/2;  // 0 or 1. 0 computes 0,32 + 128,160, 1 computes 64,96 + 192,224
    const int in = il%2;

    const int l0 = n*(2*ir + in);
    const int q_offset = 32*im + l0;
    const int y_offset = 64*im + l0;

    uint16_t aux[4];
    const uint8_t * sc = (const uint8_t *)aux;

    __global const struct block_q4_K * x = xx + ib0;

    tmp[16 * ix + tid] = 0;

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const uint8_t * q1 = x[i].qs + q_offset;
        __global const uint8_t * q2 = q1 + 64;
        __global const float   * y1 = yy + i*QK_K + y_offset;
        __global const float   * y2 = y1 + 128;

        const float dall = vload_half(0, &x[i].d);
        const float dmin = vload_half(0, &x[i].dmin);

        __global const uint16_t * a = (__global const uint16_t *)x[i].scales;
        aux[0] = a[im+0] & kmask1;
        aux[1] = a[im+2] & kmask1;
        aux[2] = ((a[im+4] >> 0) & kmask2) | ((a[im+0] & kmask3) >> 2);
        aux[3] = ((a[im+4] >> 4) & kmask2) | ((a[im+2] & kmask3) >> 2);

        float4 s = (float4)(0.f);
        float smin = 0;
        for (int l = 0; l < n; ++l) {
            s.x += y1[l] * (q1[l] & 0xF); s.y += y1[l+32] * (q1[l] >> 4);
            s.z += y2[l] * (q2[l] & 0xF); s.w += y2[l+32] * (q2[l] >> 4);
            smin += y1[l] * sc[2] + y1[l+32] * sc[3] + y2[l] * sc[6] + y2[l+32] * sc[7];
        }
        tmp[16 * ix + tid] += dall * (s.x * sc[0] + s.y * sc[1] + s.z * sc[4] + s.w * sc[5]) - dmin * smin;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q5_K(__global const struct block_q5_K * xx, __local float* tmp, __global float* yy, __global float* dst, const int ncols) {

    const uint16_t kmask1 = 0x3f3f;
    const uint16_t kmask2 = 0x0f0f;
    const uint16_t kmask3 = 0xc0c0;

    const int row = get_group_id(0);
    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    const int tid = get_local_id(0)/2;  // 0...15
    const int ix  = get_local_id(0)%2;

    const int il  = tid/4;     // 0...3
    const int ir  = tid - 4*il;// 0...3
    const int n   = 2;

    const int im = il/2;  // 0 or 1. 0 computes 0,32 + 128,160, 1 computes 64,96 + 192,224
    const int in = il%2;

    const int l0 = n*(2*ir + in);
    const int q_offset = 32*im + l0;
    const int y_offset = 64*im + l0;

    const uint8_t hm1  = 1 << (2*im);
    const uint8_t hm2  = hm1 << 4;

    uint16_t aux[4];
    const uint8_t * sc = (const uint8_t *)aux;

    __global const struct block_q5_K * x = xx + ib0;

    tmp[16 * ix + tid] = 0;

    for (int i = ix; i < num_blocks_per_row; i += 2) {

        __global const uint8_t * ql1 = x[i].qs + q_offset;
        __global const uint8_t * ql2 = ql1 + 64;
        __global const uint8_t * qh  = x[i].qh + l0;
        __global const float   * y1  = yy + i*QK_K + y_offset;
        __global const float   * y2  = y1 + 128;

        const float dall = vload_half(0, &x[i].d);
        const float dmin = vload_half(0, &x[i].dmin);

        __global const uint16_t * a = (__global const uint16_t *)x[i].scales;
        aux[0] = a[im+0] & kmask1;
        aux[1] = a[im+2] & kmask1;
        aux[2] = ((a[im+4] >> 0) & kmask2) | ((a[im+0] & kmask3) >> 2);
        aux[3] = ((a[im+4] >> 4) & kmask2) | ((a[im+2] & kmask3) >> 2);

        float4 sum = (float4)(0.f);
        float smin = 0;
        for (int l = 0; l < n; ++l) {
            sum.x += y1[l+ 0] * ((ql1[l+ 0] & 0xF) + (qh[l+ 0] & (hm1 << 0) ? 16 : 0))
                   + y1[l+16] * ((ql1[l+16] & 0xF) + (qh[l+16] & (hm1 << 0) ? 16 : 0));
            sum.y += y1[l+32] * ((ql1[l+ 0] >>  4) + (qh[l+ 0] & (hm1 << 1) ? 16 : 0))
                   + y1[l+48] * ((ql1[l+16] >>  4) + (qh[l+16] & (hm1 << 1) ? 16 : 0));
            sum.z += y2[l+ 0] * ((ql2[l+ 0] & 0xF) + (qh[l+ 0] & (hm2 << 0) ? 16 : 0))
                   + y2[l+16] * ((ql2[l+16] & 0xF) + (qh[l+16] & (hm2 << 0) ? 16 : 0));
            sum.w += y2[l+32] * ((ql2[l+ 0] >>  4) + (qh[l+ 0] & (hm2 << 1) ? 16 : 0))
                   + y2[l+48] * ((ql2[l+16] >>  4) + (qh[l+16] & (hm2 << 1) ? 16 : 0));
            smin += (y1[l] + y1[l+16]) * sc[2] + (y1[l+32] + y1[l+48]) * sc[3]
                  + (y2[l] + y2[l+16]) * sc[6] + (y2[l+32] + y2[l+48]) * sc[7];
        }
        tmp[16 * ix + tid] += dall * (sum.x * sc[0] + sum.y * sc[1] + sum.z * sc[4] + sum.w * sc[5]) - dmin * smin;

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

__kernel void dequantize_mul_mat_vec_q6_K(__global const struct block_q6_K * xx, __local float* tmp, __global const float * yy, __global float * dst, const int ncols) {

    const int row = get_group_id(0);

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    __global const struct block_q6_K * x = xx + ib0;

    const int tid = get_local_id(0)/K_QUANTS_PER_ITERATION;  // 0...31 or 0...16
    const int ix  = get_local_id(0)%K_QUANTS_PER_ITERATION;  // 0 or 0, 1

    const int step = 16/K_QUANTS_PER_ITERATION;          // 16 or 8

    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0...15 or 0...7

\n#if K_QUANTS_PER_ITERATION == 1\n
    const int l0 = K_QUANTS_PER_ITERATION*in;            // 0...15
    const int is = 0;

\n#else\n

    const int l0 = 4 * in;                               // 0, 4, 8, ..., 28
    const int is = in / 4;

\n#endif\n

    const int ql_offset = 64*im + l0;
    const int qh_offset = 32*im + l0;
    const int s_offset  =  8*im + is;
    const int y_offset = 128*im + l0;

    tmp[16 * ix + tid] = 0; // partial sum for thread in warp

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        __global const float   * y  = yy + i * QK_K + y_offset;
        __global const uint8_t * ql = x[i].ql + ql_offset;
        __global const uint8_t * qh = x[i].qh + qh_offset;
        __global const int8_t  * s  = x[i].scales + s_offset;

        const float d = vload_half(0, &x[i].d);

\n#if K_QUANTS_PER_ITERATION == 1\n
        float sum = y[ 0] * s[0] * d * ((int8_t)((ql[ 0] & 0xF) | ((qh[ 0] & 0x03) << 4)) - 32)
                  + y[16] * s[1] * d * ((int8_t)((ql[16] & 0xF) | ((qh[16] & 0x03) << 4)) - 32)
                  + y[32] * s[2] * d * ((int8_t)((ql[32] & 0xF) | ((qh[ 0] & 0x0c) << 2)) - 32)
                  + y[48] * s[3] * d * ((int8_t)((ql[48] & 0xF) | ((qh[16] & 0x0c) << 2)) - 32)
                  + y[64] * s[4] * d * ((int8_t)((ql[ 0]  >> 4) | ((qh[ 0] & 0x30) >> 0)) - 32)
                  + y[80] * s[5] * d * ((int8_t)((ql[16]  >> 4) | ((qh[16] & 0x30) >> 0)) - 32)
                  + y[96] * s[6] * d * ((int8_t)((ql[32]  >> 4) | ((qh[ 0] & 0xc0) >> 2)) - 32)
                  +y[112] * s[7] * d * ((int8_t)((ql[48]  >> 4) | ((qh[16] & 0xc0) >> 2)) - 32);
        tmp[16 * ix + tid] += sum;
\n#else\n
        float sum = 0;
        for (int l = 0; l < 4; ++l) {
            sum += y[l+ 0] * s[0] * d * ((int8_t)((ql[l+ 0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32)
                 + y[l+32] * s[2] * d * ((int8_t)((ql[l+32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32)
                 + y[l+64] * s[4] * d * ((int8_t)((ql[l+ 0]  >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32)
                 + y[l+96] * s[6] * d * ((int8_t)((ql[l+32]  >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32);
        }
        tmp[16 * ix + tid] += sum;
\n#endif\n

    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=16; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}

);


std::string dequant_template = MULTILINE_QUOTE(
__kernel void KERNEL_NAME(__global const X_TYPE* x, const int x_offset, __global float* dst, const int dst_offset) {
    const int i = get_group_id(0)*get_local_size(0) + get_local_id(0)*2;

    if (i >= get_global_size(0)) {
        return;
    }

    const uint qk = QUANT_K;
    const uint qr = QUANT_R;

    const int ib = i/qk; // block index
    const int iqs = (i%qk)/qr; // quant index
    const int iybs = i - i%qk; // y block start index
    const int q_offset = qr == 1 ? 1 : qk/2;
    dst += dst_offset;

    // dequantize
    float v0, v1;
    DEQUANT_FUNC(x + x_offset, ib, iqs, &v0, &v1);
    dst[iybs + iqs + 0] = v0;
    dst[iybs + iqs + q_offset] = v1;
}
);

std::string dequant_mul_mat_vec_template = MULTILINE_QUOTE(
__kernel void KERNEL_NAME(__global const X_TYPE* x, const int x_offset, __local float* tmp, __global const float* y, const int y_offset, __global float* dst, const int dst_offset, const int ncols) {
    const int block_size = get_local_size(0);
    const int row = get_group_id(0);
    const int tid = get_local_id(0);

    const uint qk = QUANT_K;
    const uint qr = QUANT_R;

    const int q_offset = qr == 1 ? 1 : qk/2;
    x += x_offset;
    y += y_offset;
    dst += dst_offset;

    tmp[tid] = 0;

    for (int i = 0; i < ncols/block_size; i += 2) {
        const int col = i*block_size + 2*tid;
        const int ib = (row*ncols + col)/qk; // block index
        const int iqs = (col%qk)/qr; // quant index
        const int iybs = col - col%qk; // y block start index

        // dequantize
        float v0, v1;
        DEQUANT_FUNC(x, ib, iqs, &v0, &v1);

        // matrix multiplication
        tmp[tid] += v0 * y[iybs + iqs + 0];
        tmp[tid] += v1 * y[iybs + iqs + q_offset];
    }

    // sum up partial sums and write back result
    barrier(CLK_LOCAL_MEM_FENCE);
    for (int s=block_size/2; s>0; s>>=1) {
        if (tid < s) {
            tmp[tid] += tmp[tid + s];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (tid == 0) {
        dst[row] = tmp[0];
    }
}
);


std::string mul_template = MULTILINE_QUOTE(
__kernel void KERNEL_NAME(__global const TYPE* x, const int x_offset, __global const TYPE* y, const int y_offset, __global TYPE* dst, const int dst_offset, const int ky) {
    const int i = get_group_id(0)*get_local_size(0) + get_local_id(0);

    if (i >= get_global_size(0)) {
        return;
    }

    dst[dst_offset + i] = x[x_offset + i] * y[y_offset + i%ky];
}
);

#define CL_CHECK(err)                                               \
    do {                                                            \
        cl_int err_ = (err);                                        \
        if (err_ != CL_SUCCESS) {                                   \
            fprintf(stderr, "ggml_opencl: %s error %d at %s:%d\n",  \
                #err, err_, __FILE__, __LINE__);                    \
            exit(1);                                                \
        }                                                           \
    } while (0)

std::array<std::string, 5> dequant_str_keys = {
    "KERNEL_NAME", "X_TYPE", "QUANT_K", "QUANT_R", "DEQUANT_FUNC"
};

std::array<std::string, 30> dequant_str_values = {
    "dequantize_row_q4_0", "struct block_q4_0", "QK4_0", "QR4_0", "dequantize_q4_0",
    "dequantize_row_q4_1", "struct block_q4_1", "QK4_1", "QR4_1", "dequantize_q4_1",
    "dequantize_row_q5_0", "struct block_q5_0", "QK5_0", "QR5_0", "dequantize_q5_0",
    "dequantize_row_q5_1", "struct block_q5_1", "QK5_1", "QR5_1", "dequantize_q5_1",
    "dequantize_row_q8_0", "struct block_q8_0", "QK8_0", "QR8_0", "dequantize_q8_0",
    "convert_row_f16", "half", "1", "1", "convert_f16"
};

std::array<std::string, 30> dequant_mul_mat_vec_str_values = {
    "dequantize_mul_mat_vec_q4_0", "struct block_q4_0", "QK4_0", "QR4_0", "dequantize_q4_0",
    "dequantize_mul_mat_vec_q4_1", "struct block_q4_1", "QK4_1", "QR4_1", "dequantize_q4_1",
    "dequantize_mul_mat_vec_q5_0", "struct block_q5_0", "QK5_0", "QR5_0", "dequantize_q5_0",
    "dequantize_mul_mat_vec_q5_1", "struct block_q5_1", "QK5_1", "QR5_1", "dequantize_q5_1",
    "dequantize_mul_mat_vec_q8_0", "struct block_q8_0", "QK8_0", "QR8_0", "dequantize_q8_0",
    "convert_mul_mat_vec_f16", "half", "1", "1", "convert_f16"
};

std::array<std::string, 2> mul_str_keys = {
    "KERNEL_NAME", "TYPE"
};
std::array<std::string, 2> mul_str_values = {
    "mul_f32", "float"
};

std::string& replace(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
         s.replace(pos, from.length(), to);
         pos += to.length();
    }
    return s;
}

std::string generate_kernels() {
    std::stringstream src;
    src << program_source << '\n';
    src << k_quants_source << '\n';
    for (size_t i = 0; i < dequant_str_values.size(); i += dequant_str_keys.size()) {
        std::string dequant_kernel = dequant_template;
        std::string dmmv_kernel = dequant_mul_mat_vec_template;
        for (size_t j = 0; j < dequant_str_keys.size(); j++) {
            replace(dequant_kernel, dequant_str_keys[j], dequant_str_values[i + j]);
            replace(dmmv_kernel, dequant_str_keys[j], dequant_mul_mat_vec_str_values[i + j]);
        }
        src << dequant_kernel << '\n';
        src << dmmv_kernel << '\n';
    }
    for (size_t i = 0; i < mul_str_values.size(); i += mul_str_keys.size()) {
        std::string mul_kernel = mul_template;
        for (size_t j = 0; j < mul_str_keys.size(); j++) {
            replace(mul_kernel, mul_str_keys[j], mul_str_values[i + j]);
        }
        src << mul_kernel << '\n';
    }

    return src.str();
}

cl_platform_id platform;
cl_device_id device;
cl_context context;
cl_command_queue queue;
cl_program program;
cl_kernel convert_row_f16_cl;
cl_kernel dequantize_row_q4_0_cl, dequantize_row_q4_1_cl, dequantize_row_q5_0_cl, dequantize_row_q5_1_cl, dequantize_row_q8_0_cl;
cl_kernel dequantize_mul_mat_vec_q4_0_cl, dequantize_mul_mat_vec_q4_1_cl, dequantize_mul_mat_vec_q5_0_cl, dequantize_mul_mat_vec_q5_1_cl, dequantize_mul_mat_vec_q8_0_cl, convert_mul_mat_vec_f16_cl;
cl_kernel dequantize_block_q2_k_cl, dequantize_block_q3_k_cl, dequantize_block_q4_k_cl, dequantize_block_q5_k_cl, dequantize_block_q6_k_cl;
cl_kernel dequantize_mul_mat_vec_q2_K_cl, dequantize_mul_mat_vec_q3_K_cl, dequantize_mul_mat_vec_q4_K_cl, dequantize_mul_mat_vec_q5_K_cl, dequantize_mul_mat_vec_q6_K_cl;
cl_kernel mul_f32_cl;
bool fp16_support;

cl_program build_program_from_source(cl_context ctx, cl_device_id dev, const char* program_buffer) {
    cl_program p;
    char *program_log;
    size_t program_size;
    size_t log_size;
    int err;

    program_size = strlen(program_buffer);

    p = clCreateProgramWithSource(ctx, 1, (const char**)&program_buffer, &program_size, &err);
    if(err < 0) {
        fprintf(stderr, "OpenCL error creating program");
        exit(1);
    }

    std::string compile_opts = "-cl-mad-enable -cl-unsafe-math-optimizations -cl-finite-math-only -cl-fast-relaxed-math "
                               "-DQK4_0=32 -DQR4_0=2 -DQK4_1=32 -DQR4_1=2 -DQK5_0=32 -DQR5_0=2 -DQK5_1=32 -DQR5_1=2 -DQK8_0=32 -DQR8_0=1 "
                               "-DQK_K=256 -DK_QUANTS_PER_ITERATION=" + std::to_string(K_QUANTS_PER_ITERATION);

    err = clBuildProgram(p, 0, nullptr, compile_opts.c_str(), nullptr, nullptr);
    if(err < 0) {

        clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        program_log = (char*) malloc(log_size + 1);
        program_log[log_size] = '\0';
        clGetProgramBuildInfo(p, dev, CL_PROGRAM_BUILD_LOG, log_size + 1, program_log, nullptr);
        fprintf(stderr, "ggml_opencl: kernel compile error:\n\n%s\n", program_log);
        free(program_log);
        exit(1);
    }

    return p;
}
} // namespace

void ggml_cl_init(void) {
    cl_int err;

    struct cl_device;
    struct cl_platform {
        cl_platform_id id;
        unsigned number;
        char name[128];
        char vendor[128];
        struct cl_device * devices;
        unsigned n_devices;
        struct cl_device * default_device;
    };

    struct cl_device {
        struct cl_platform * platform;
        cl_device_id id;
        unsigned number;
        cl_device_type type;
        char name[128];
    };

    enum { NPLAT = 16, NDEV = 16 };

    struct cl_platform platforms[NPLAT];
    unsigned n_platforms = 0;
    struct cl_device devices[NDEV];
    unsigned n_devices = 0;
    struct cl_device * default_device = nullptr;

    platform = nullptr;
    device = nullptr;

    cl_platform_id platform_ids[NPLAT];
    CL_CHECK(clGetPlatformIDs(NPLAT, platform_ids, &n_platforms));

    for (unsigned i = 0; i < n_platforms; i++) {
        struct cl_platform * p = &platforms[i];
        p->number = i;
        p->id = platform_ids[i];
        CL_CHECK(clGetPlatformInfo(p->id, CL_PLATFORM_NAME, sizeof(p->name), &p->name, nullptr));
        CL_CHECK(clGetPlatformInfo(p->id, CL_PLATFORM_VENDOR, sizeof(p->vendor), &p->vendor, nullptr));

        cl_device_id device_ids[NDEV];
        cl_int clGetDeviceIDsError = clGetDeviceIDs(p->id, CL_DEVICE_TYPE_ALL, NDEV, device_ids, &p->n_devices);
        if (clGetDeviceIDsError == CL_DEVICE_NOT_FOUND) {
            p->n_devices = 0;
        } else {
            CL_CHECK(clGetDeviceIDsError);
        }
        p->devices = p->n_devices > 0 ? &devices[n_devices] : nullptr;
        p->default_device = nullptr;

        for (unsigned j = 0; j < p->n_devices; j++) {
            struct cl_device * d = &devices[n_devices];
            d->number = n_devices++;
            d->id = device_ids[j];
            d->platform = p;
            CL_CHECK(clGetDeviceInfo(d->id, CL_DEVICE_NAME, sizeof(d->name), &d->name, nullptr));
            CL_CHECK(clGetDeviceInfo(d->id, CL_DEVICE_TYPE, sizeof(d->type), &d->type, nullptr));

            if (p->default_device == nullptr && d->type == CL_DEVICE_TYPE_GPU) {
                p->default_device = d;
            }
        }

        if (default_device == nullptr && p->default_device != nullptr) {
            default_device = p->default_device;
        }
    }

    if (n_devices == 0) {
        fprintf(stderr, "ggml_opencl: could find any OpenCL devices.\n");
        exit(1);
    }

    char * user_platform_string = getenv("GGML_OPENCL_PLATFORM");
    char * user_device_string = getenv("GGML_OPENCL_DEVICE");
    unsigned long user_platform_number = ~0UL;
    unsigned long user_device_number = ~0UL;

    if (user_platform_string != nullptr) {
        user_platform_number = std::stoul(user_platform_string);
    }
    if (user_device_string != nullptr) {
        user_device_number = std::stoul(user_device_string);
    }
    if (~user_platform_number && ~user_device_number) {
        if (user_platform_number >= n_devices) {
            fprintf(stderr, "ggml_opencl: invalid platform number %lu\n", user_platform_number);
            exit(1);
        }
        cl_platform* user_platform = &platforms[user_platform_number];
        if (user_device_number >= user_platform->n_devices) {
            fprintf(stderr, "ggml_opencl: invalid device number %lu\n", user_device_number);
            exit(1);
        }
        default_device = &user_platform->devices[user_device_number];
    } else {

        struct cl_device * selected_devices = devices;
        unsigned n_selected_devices = n_devices;

        if (!~user_platform_number && user_platform_string != nullptr && user_platform_string[0] != 0) {
            for (unsigned i = 0; i < n_platforms; i++) {
                struct cl_platform * p = &platforms[i];
                if (strstr(p->name, user_platform_string) != nullptr ||
                    strstr(p->vendor, user_platform_string) != nullptr) {
                    user_platform_number = (int)i;
                    break;
                }
            }
            if (!~user_platform_number) {
                fprintf(stderr, "ggml_opencl: no platform matching '%s' was found.\n", user_platform_string);
                exit(1);
            }
        }
        if (~user_platform_number) {
            struct cl_platform * p = &platforms[user_platform_number];
            selected_devices = p->devices;
            n_selected_devices = p->n_devices;
            default_device = p->default_device;
            if (n_selected_devices == 0) {
                fprintf(stderr, "ggml_opencl: selected platform '%s' does not have any devices.\n", p->name);
                exit(1);
            }
        }

        if (!~user_device_number && user_device_string != nullptr && user_device_string[0] != 0) {
            for (unsigned i = 0; i < n_selected_devices; i++) {
                struct cl_device * d = &selected_devices[i];
                if (strstr(d->name, user_device_string) != nullptr) {
                    user_device_number = d->number;
                    break;
                }
            }
            if (!~user_device_number) {
                fprintf(stderr, "ggml_opencl: no device matching '%s' was found.\n", user_device_string);
                exit(1);
            }
        }
        if (~user_device_number) {
            selected_devices = &devices[user_device_number];
            n_selected_devices = 1;
            default_device = &selected_devices[0];
        }

        GGML_ASSERT(n_selected_devices > 0);

        if (default_device == nullptr) {
            default_device = &selected_devices[0];
        }
    }

    fprintf(stderr, "ggml_opencl: selecting platform: '%s'\n", default_device->platform->name);
    fprintf(stderr, "ggml_opencl: selecting device: '%s'\n", default_device->name);
    if (default_device->type != CL_DEVICE_TYPE_GPU) {
        fprintf(stderr, "ggml_opencl: warning, not a GPU: '%s'.\n", default_device->name);
    }

    platform = default_device->platform->id;
    device = default_device->id;

    size_t ext_str_size;
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, 0, nullptr, &ext_str_size);
    char *ext_buffer = (char *)alloca(ext_str_size + 1);
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, ext_str_size, ext_buffer, nullptr);
    ext_buffer[ext_str_size] = '\0'; // ensure it is null terminated
    // Check if ext_buffer contains cl_khr_fp16
    fp16_support = strstr(ext_buffer, "cl_khr_fp16") != nullptr;
    fprintf(stderr, "ggml_opencl: device FP16 support: %s\n", fp16_support ? "true" : "false");

    cl_context_properties properties[] = {
        (intptr_t)CL_CONTEXT_PLATFORM, (intptr_t)platform, 0
    };

    CL_CHECK((context = clCreateContext(properties, 1, &device, nullptr, nullptr, &err), err));

    CL_CHECK((queue = clCreateCommandQueue(context, device, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &err),
        (err != CL_INVALID_QUEUE_PROPERTIES && err != CL_INVALID_VALUE ? err :
        (queue = clCreateCommandQueue(context, device, 0, &err), err)
    )));

    const std::string kernel_src = generate_kernels();

    program = build_program_from_source(context, device, kernel_src.c_str());

    // FP16 to FP32 kernel
    CL_CHECK((convert_row_f16_cl = clCreateKernel(program, "convert_row_f16", &err), err));

    // Dequantize kernels
    CL_CHECK((dequantize_row_q4_0_cl = clCreateKernel(program, "dequantize_row_q4_0", &err), err));
    CL_CHECK((dequantize_row_q4_1_cl = clCreateKernel(program, "dequantize_row_q4_1", &err), err));
    CL_CHECK((dequantize_row_q5_0_cl = clCreateKernel(program, "dequantize_row_q5_0", &err), err));
    CL_CHECK((dequantize_row_q5_1_cl = clCreateKernel(program, "dequantize_row_q5_1", &err), err));
    CL_CHECK((dequantize_row_q8_0_cl = clCreateKernel(program, "dequantize_row_q8_0", &err), err));
    CL_CHECK((dequantize_row_q8_0_cl = clCreateKernel(program, "dequantize_row_q8_0", &err), err));
    CL_CHECK((dequantize_block_q2_k_cl = clCreateKernel(program, "dequantize_block_q2_K", &err), err));
    CL_CHECK((dequantize_block_q3_k_cl = clCreateKernel(program, "dequantize_block_q3_K", &err), err));
    CL_CHECK((dequantize_block_q4_k_cl = clCreateKernel(program, "dequantize_block_q4_K", &err), err));
    CL_CHECK((dequantize_block_q5_k_cl = clCreateKernel(program, "dequantize_block_q5_K", &err), err));
    CL_CHECK((dequantize_block_q6_k_cl = clCreateKernel(program, "dequantize_block_q6_K", &err), err));

    // dequant mul mat kernel
    CL_CHECK((dequantize_mul_mat_vec_q4_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_0", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q4_1_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_1", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q5_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_0", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q5_1_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_1", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q8_0_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q8_0", &err), err));
    CL_CHECK((convert_mul_mat_vec_f16_cl = clCreateKernel(program, "convert_mul_mat_vec_f16", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q2_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q2_K", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q3_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q3_K", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q4_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q4_K", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q5_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q5_K", &err), err));
    CL_CHECK((dequantize_mul_mat_vec_q6_K_cl = clCreateKernel(program, "dequantize_mul_mat_vec_q6_K", &err), err));

    // mul kernel
    CL_CHECK((mul_f32_cl = clCreateKernel(program, "mul_f32", &err), err));
}

namespace {
cl_kernel* ggml_get_to_fp32_cl(ggml_type type) {
    switch (type) {
        case GGML_TYPE_Q4_0:
            return &dequantize_row_q4_0_cl;
        case GGML_TYPE_Q4_1:
            return &dequantize_row_q4_1_cl;
        case GGML_TYPE_Q5_0:
            return &dequantize_row_q5_0_cl;
        case GGML_TYPE_Q5_1:
            return &dequantize_row_q5_1_cl;
        case GGML_TYPE_Q8_0:
            return &dequantize_row_q8_0_cl;
        case GGML_TYPE_Q2_K:
            return &dequantize_block_q2_k_cl;
        case GGML_TYPE_Q3_K:
            return &dequantize_block_q3_k_cl;
        case GGML_TYPE_Q4_K:
            return &dequantize_block_q4_k_cl;
        case GGML_TYPE_Q5_K:
            return &dequantize_block_q5_k_cl;
        case GGML_TYPE_Q6_K:
            return &dequantize_block_q6_k_cl;
        case GGML_TYPE_F16:
            return &convert_row_f16_cl;
        default:
            return nullptr;
    }
}

size_t ggml_cl_global_denom(ggml_type type) {
    switch (type) {
        case GGML_TYPE_Q4_0:
        case GGML_TYPE_Q4_1:
        case GGML_TYPE_Q5_0:
        case GGML_TYPE_Q5_1:
        case GGML_TYPE_Q8_0:
            return 1;
        case GGML_TYPE_Q2_K:
        case GGML_TYPE_Q3_K:
            return 4;
        case GGML_TYPE_Q4_K:
            return 8;
        case GGML_TYPE_Q5_K:
        case GGML_TYPE_Q6_K:
            return 4;
        case GGML_TYPE_F16:
        default:
            return 1;
    }
}

size_t ggml_cl_local_size(ggml_type type) {
    switch (type) {
        case GGML_TYPE_Q4_0:
        case GGML_TYPE_Q4_1:
        case GGML_TYPE_Q5_0:
        case GGML_TYPE_Q5_1:
        case GGML_TYPE_Q8_0:
            return 0;
        case GGML_TYPE_Q2_K:
        case GGML_TYPE_Q3_K:
            return 64;
        case GGML_TYPE_Q4_K:
            return 32;
        case GGML_TYPE_Q5_K:
        case GGML_TYPE_Q6_K:
            return 64;
        case GGML_TYPE_F16:
        default:
            return 0;
    }
}

cl_kernel* ggml_get_dequantize_mul_mat_vec_cl(ggml_type type) {
    switch (type) {
        case GGML_TYPE_Q4_0:
            return &dequantize_mul_mat_vec_q4_0_cl;
        case GGML_TYPE_Q4_1:
            return &dequantize_mul_mat_vec_q4_1_cl;
        case GGML_TYPE_Q5_0:
            return &dequantize_mul_mat_vec_q5_0_cl;
        case GGML_TYPE_Q5_1:
            return &dequantize_mul_mat_vec_q5_1_cl;
        case GGML_TYPE_Q8_0:
            return &dequantize_mul_mat_vec_q8_0_cl;
        case GGML_TYPE_F16:
            return &convert_mul_mat_vec_f16_cl;
        case GGML_TYPE_Q2_K:
            return &dequantize_mul_mat_vec_q2_K_cl;
        case GGML_TYPE_Q3_K:
            return &dequantize_mul_mat_vec_q3_K_cl;
        case GGML_TYPE_Q4_K:
            return &dequantize_mul_mat_vec_q4_K_cl;
        case GGML_TYPE_Q5_K:
            return &dequantize_mul_mat_vec_q5_K_cl;
        case GGML_TYPE_Q6_K:
            return &dequantize_mul_mat_vec_q6_K_cl;
        default:
            return nullptr;
    }
}

// buffer pool for cl
#define MAX_CL_BUFFERS 128

struct scoped_spin_lock {
    std::atomic_flag& lock;
    explicit scoped_spin_lock(std::atomic_flag& lock) : lock(lock) {
        while (lock.test_and_set(std::memory_order_acquire)) {
            // spin
        }
    }
    ~scoped_spin_lock() {
        lock.clear(std::memory_order_release);
    }
    scoped_spin_lock(const scoped_spin_lock&) = delete;
    scoped_spin_lock& operator=(const scoped_spin_lock&) = delete;
};

struct cl_buffer {
    cl_mem mem = nullptr;
    size_t size = 0;
};

class ggml_cl_pool {
    const cl_mem_flags flags;
    cl_buffer g_cl_buffer_pool[MAX_CL_BUFFERS];
    std::atomic_flag g_cl_pool_lock = ATOMIC_FLAG_INIT;
public:
    explicit ggml_cl_pool(cl_mem_flags _flags) : flags{_flags} {};

    cl_mem get(size_t size, size_t * actual_size) {
        scoped_spin_lock lock(g_cl_pool_lock);
        cl_int err;

        int best_i = -1;
        size_t best_size = std::numeric_limits<size_t>::max(); //smallest unused buffer that fits our needs
        int worst_i = -1;
        size_t worst_size = 0; //largest unused buffer seen so far
        for (int i = 0; i < MAX_CL_BUFFERS; ++i) {
            cl_buffer &b = g_cl_buffer_pool[i];
            if (b.size > 0 && b.size >= size && b.size < best_size)
            {
                best_i = i;
                best_size = b.size;
            }
            if (b.size > 0 && b.size > worst_size)
            {
                worst_i = i;
                worst_size = b.size;
            }
        }
        if(best_i!=-1) //found the smallest buffer that fits our needs
        {
            cl_buffer& b = g_cl_buffer_pool[best_i];
            cl_mem mem = b.mem;
            *actual_size = b.size;
            b.size = 0;
            return mem;
        }
        if(worst_i!=-1) //no buffer that fits our needs, resize largest one to save memory
        {
             cl_buffer& b = g_cl_buffer_pool[worst_i];
             cl_mem mem = b.mem;
             b.size = 0;
             clReleaseMemObject(mem);
        }
        cl_mem mem;
        CL_CHECK((mem = clCreateBuffer(context, flags, size, nullptr, &err), err));
        *actual_size = size;
        return mem;
    }

    void put(cl_mem mem, size_t size) {
        scoped_spin_lock lock(g_cl_pool_lock);

        for (auto & b : g_cl_buffer_pool) {
            if (b.size == 0) {
                b.mem = mem;
                b.size = size;
                return;
            }
        }
        fprintf(stderr, "WARNING: cl buffer pool full, increase MAX_CL_BUFFERS\n");
        clReleaseMemObject(mem);
    }
};
ggml_cl_pool r_pool{CL_MEM_READ_ONLY | CL_MEM_HOST_WRITE_ONLY};
ggml_cl_pool w_pool{CL_MEM_WRITE_ONLY | CL_MEM_HOST_READ_ONLY};
ggml_cl_pool rw_pool{CL_MEM_READ_WRITE | CL_MEM_HOST_NO_ACCESS};
} // namespace

void ggml_cl_free_data(const struct ggml_tensor* tensor) {
    if (tensor->backend != GGML_BACKEND_GPU) {
        return;
    }

    clReleaseMemObject(static_cast<cl_mem>(tensor->extra));
}

namespace {
cl_int ggml_cl_h2d_tensor_2d(cl_mem dst, size_t offset, const struct ggml_tensor * src, cl_int i3, cl_int i2, cl_event* ev) {
    cl_int err;
    const size_t ne0 = src->ne[0];
    const size_t ne1 = src->ne[1];
    const size_t nb0 = src->nb[0];
    const size_t nb1 = src->nb[1];
    const size_t nb2 = src->nb[2];
    const size_t nb3 = src->nb[3];
    const enum ggml_type type = src->type;
    const size_t ts = ggml_type_size(type);
    const size_t bs = ggml_blck_size(type);

    const void * x = (const void *) ((const char *) src->data + i2*nb2 + i3*nb3);
    if (nb0 == ts && nb1 == ts*ne0/bs) {
        err = clEnqueueWriteBuffer(queue, dst, CL_FALSE, offset, ne1*nb1, x, 0, nullptr, ev);
        return err;
    }
    if (nb0 == ts) {
        const size_t buffer_origin[3] = { offset, 0, 0 };
        const size_t host_origin[3] = { 0, 0, 0 };
        const size_t region[3] = { ts*ne0/bs, ne1, 1 };
        err = clEnqueueWriteBufferRect(queue, dst, CL_FALSE, buffer_origin, host_origin, region, ts*ne0/bs, 0, nb1, 0, x, 0, nullptr, ev);
        return err;
    }
    for (size_t i1 = 0; i1 < ne1; i1++) {
        // pretend the row is a matrix with cols=1
        const size_t buffer_origin[3] = { offset, i1, 0 };
        const size_t host_origin[3] = { 0, 0, 0 };
        const size_t region[3] = { ts/bs, ne0, 1 };
        err = clEnqueueWriteBufferRect(queue, dst, CL_FALSE, buffer_origin, host_origin, region, 0, 0, nb0, 0, ((const char *)x) + i1*nb0, 0, nullptr, ev);
        if (err != CL_SUCCESS) {
            break;
        }
    }
    return err;
}

void ggml_cl_mul_f32(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_ASSERT(src1->backend == GGML_BACKEND_GPU);
    const auto ne00 = static_cast<cl_int>(src0->ne[0]);
    const auto ne01 = static_cast<cl_int>(src0->ne[1]);
    const auto ne02 = static_cast<cl_int>(src0->ne[2]);
    const auto ne03 = static_cast<cl_int>(src0->ne[3]);
    const cl_int ne023 = ne02 * ne03;
    const cl_int ne001 = ne00 * ne01;
    const cl_int ne0 = ne001 * ne023;
    const auto ne10 = static_cast<cl_int>(src1->ne[0]);
    const auto ne11 = static_cast<cl_int>(src1->ne[1]);
    const auto ne12 = static_cast<cl_int>(src1->ne[2]);
    const auto ne13 = static_cast<cl_int>(src1->ne[3]);
    const auto nb10 = static_cast<cl_int>(src1->nb[0]);
    const auto nb2  = static_cast<cl_int>(dst->nb[2]);
    const auto nb3  = static_cast<cl_int>(dst->nb[3]);
    size_t x_size;
    size_t d_size;

    cl_mem d_X = r_pool.get(ne0 * sizeof(float), &x_size); // src0
    auto* d_Y = static_cast<cl_mem>(src1->extra); // src1 is already on device, broadcasted.
    cl_mem d_D = w_pool.get(ne0 * sizeof(float), &d_size); // dst

    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            // copy src0 to device
            // stream in with same loop not a separate one, so that the kernel can run as soon as each upload finishes
            cl_event h2d_ev;
            const cl_int i0 = i03*ne02 + i02;
            const size_t size001 = sizeof(float) * ne001;
            CL_CHECK(ggml_cl_h2d_tensor_2d(d_X, i0*size001, src0, i03, i02, &h2d_ev));

            cl_event kernel_ev;
            auto * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);
            const cl_int ky = ne10;
            if (nb10 == sizeof(float)) {
                // Contiguous, avoid overhead from queueing many kernel runs
                const cl_int i13 = i03%ne13;
                const cl_int i12 = i02%ne12;
                const cl_int i1 = i13*ne12*ne11 + i12*ne11;

                cl_int x_offset = i0*ne001;
                cl_int y_offset = i1*ne10;
                cl_int d_offset = x_offset;

                const size_t global = ne00 * ne01;
                CL_CHECK(clSetKernelArg(mul_f32_cl, 0, sizeof(cl_mem), &d_X));
                CL_CHECK(clSetKernelArg(mul_f32_cl, 1, sizeof(cl_int), &x_offset));
                CL_CHECK(clSetKernelArg(mul_f32_cl, 2, sizeof(cl_mem), &d_Y));
                CL_CHECK(clSetKernelArg(mul_f32_cl, 3, sizeof(cl_int), &y_offset));
                CL_CHECK(clSetKernelArg(mul_f32_cl, 4, sizeof(cl_mem), &d_D));
                CL_CHECK(clSetKernelArg(mul_f32_cl, 5, sizeof(cl_int), &d_offset));
                CL_CHECK(clSetKernelArg(mul_f32_cl, 6, sizeof(cl_int), &ky));
                CL_CHECK(clEnqueueNDRangeKernel(queue, mul_f32_cl, 1, nullptr, &global, nullptr, 1, &h2d_ev, &kernel_ev));

                // copy dst to host
                CL_CHECK(clEnqueueReadBuffer(queue, d_D, false, i0*size001, size001, d, 1, &kernel_ev, nullptr));
                CL_CHECK(clReleaseEvent(kernel_ev));
            } else {
                const size_t global = ne00;
                for (cl_int i01 = 0; i01 < ne01; i01++) {
                    const cl_int i13 = i03%ne13;
                    const cl_int i12 = i02%ne12;
                    const cl_int i11 = i01%ne11;
                    const cl_int i1 = i13*ne12*ne11 + i12*ne11 + i11;

                    cl_int x_offset = i01*ne00 + i0*ne001;
                    cl_int y_offset = i1*ne10;
                    cl_int d_offset = x_offset;

                    // compute
                    CL_CHECK(clSetKernelArg(mul_f32_cl, 0, sizeof(cl_mem), &d_X));
                    CL_CHECK(clSetKernelArg(mul_f32_cl, 1, sizeof(cl_int), &x_offset));
                    CL_CHECK(clSetKernelArg(mul_f32_cl, 2, sizeof(cl_mem), &d_Y));
                    CL_CHECK(clSetKernelArg(mul_f32_cl, 3, sizeof(cl_int), &y_offset));
                    CL_CHECK(clSetKernelArg(mul_f32_cl, 4, sizeof(cl_mem), &d_D));
                    CL_CHECK(clSetKernelArg(mul_f32_cl, 5, sizeof(cl_int), &d_offset));
                    CL_CHECK(clSetKernelArg(mul_f32_cl, 6, sizeof(cl_int), &ky));
                    CL_CHECK(clEnqueueNDRangeKernel(queue, mul_f32_cl, 1, nullptr, &global, nullptr, 1, &h2d_ev, &kernel_ev));

                    // copy dst to host
                    const size_t size = global * sizeof(float);
                    CL_CHECK(clEnqueueReadBuffer(queue, d_D, false, i0*size001 + i01*size, size, d, 1, &kernel_ev, nullptr));
                    CL_CHECK(clReleaseEvent(kernel_ev));
                }
            }
            CL_CHECK(clReleaseEvent(h2d_ev));
        }
    }
    CL_CHECK(clFinish(queue)); // block after starting all downloads instead of after each
    r_pool.put(d_X, x_size);
    w_pool.put(d_D, d_size);
}
} // namespace

void ggml_cl_mul(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst) {
    GGML_ASSERT(src0->type == GGML_TYPE_F32 && src1->type == GGML_TYPE_F32 && dst->type == GGML_TYPE_F32);
    ggml_cl_mul_f32(src0, src1, dst);
}

namespace {
void ggml_cl_mul_mat_f32(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    const auto ne00 = static_cast<cl_int>(src0->ne[0]);
    const auto ne01 = static_cast<cl_int>(src0->ne[1]);
    const auto ne02 = static_cast<cl_int>(src0->ne[2]);
    const auto ne03 = static_cast<cl_int>(src0->ne[3]);
    const cl_int ne023 = ne02 * ne03;
    const cl_int ne001 = ne00 * ne01;
    const cl_int ne0 = ne001 * ne023;

    const auto ne10 = static_cast<cl_int>(src1->ne[0]);
    const auto ne11 = static_cast<cl_int>(src1->ne[1]);
    const cl_int ne101 = ne10 * ne11;
    const cl_int ne1 = ne101 * ne023;

    const auto nb2  = static_cast<cl_int>(dst->nb[2]);
    const auto nb3  = static_cast<cl_int>(dst->nb[3]);

    const float alpha = 1.0f;
    const float beta = 0.0f;
    const cl_int ne201 = ne11 * ne01;
    const cl_int ne2 = ne201 * ne023;

    size_t x_size{};
    size_t y_size{};
    size_t d_size{};
    cl_mem d_X;
    const bool src0h2d = src0->backend != GGML_BACKEND_GPU;
    if (!src0h2d) { // NOLINT
        d_X = (cl_mem) src0->extra;
    } else {
        d_X = r_pool.get(ne0 * sizeof(float), &x_size);
    }
    cl_mem d_Y = r_pool.get(ne1 * sizeof(float), &y_size);
    cl_mem d_D = w_pool.get(ne2 * sizeof(float), &d_size);

    // To avoid clFinish, use a separate loop to work around clblast not having event_wait_list
    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            // copy data to device
            const cl_int i0 = i03*ne02 + i02;
            if (src0h2d) {
                CL_CHECK(ggml_cl_h2d_tensor_2d(d_X, i0*ne001, src0, i03, i02, nullptr));
            }
            CL_CHECK(ggml_cl_h2d_tensor_2d(d_Y, i0*ne101, src1, i03, i02, nullptr));
        }
    }
    // Sequence the kernels after the upload, but do not block the CPU
    // Increasing precision to try to launch each kernel after each upload is unproductive because
    // the amount to upload is small and llama.cpp is CPU bound
    CL_CHECK(clEnqueueBarrierWithWaitList(queue, 0, nullptr, nullptr));

    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            // compute
            const cl_int i0 = i03*ne02 + i02;
            cl_event ev_sgemm;
            clblast::StatusCode status = clblast::Gemm<cl_float>(clblast::Layout::kColMajor,
                                                       clblast::Transpose::kYes, clblast::Transpose::kNo,
                                                       ne01, ne11, ne10,
                                                       alpha,
                                                       d_X, i0*ne001, ne00,
                                                       d_Y, i0*ne101, ne10,
                                                       beta,
                                                       d_D, i0*ne201, ne01,
                                                       &queue, &ev_sgemm);

            if (status != clblast::StatusCode::kSuccess) {
                GGML_ASSERT(false);
            }

            // copy dst to host
            auto * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);
            const size_t size = sizeof(float) * ne201;
            CL_CHECK(clEnqueueReadBuffer(queue, d_D, false, i0*size, size, d, 1, &ev_sgemm, nullptr));
            CL_CHECK(clReleaseEvent(ev_sgemm));
        }
    }
    CL_CHECK(clFinish(queue)); // block after starting all downloads instead of after each

    if (src0->backend != GGML_BACKEND_GPU) {
        r_pool.put(d_X, x_size);
    }
    r_pool.put(d_Y, y_size);
    w_pool.put(d_D, d_size);
}

void ggml_cl_mul_mat_f16(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst, void * wdata, size_t /* wsize */) {
    GGML_ASSERT(fp16_support);

    const auto ne00 = static_cast<cl_int>(src0->ne[0]);
    const auto ne01 = static_cast<cl_int>(src0->ne[1]);
    const auto ne02 = static_cast<cl_int>(src0->ne[2]);
    const auto ne03 = static_cast<cl_int>(src0->ne[3]);
    const cl_int ne023 = ne02 * ne03;
    const cl_int ne001 = ne00 * ne01;
    const cl_int ne0 = ne001 * ne023;

    const auto ne10 = static_cast<cl_int>(src1->ne[0]);
    const auto ne11 = static_cast<cl_int>(src1->ne[1]);
    const cl_int ne101 = ne10 * ne11;
    const cl_int ne1 = ne101 * ne023;

    const auto nb10 = static_cast<cl_int>(src1->nb[0]);
    const auto nb11 = static_cast<cl_int>(src1->nb[1]);
    const auto nb12 = static_cast<cl_int>(src1->nb[2]);
    const auto nb13 = static_cast<cl_int>(src1->nb[3]);

    const auto nb2 = static_cast<cl_int>(dst->nb[2]);
    const auto nb3 = static_cast<cl_int>(dst->nb[3]);
    const cl_int ne201 = ne11 * ne01;
    const cl_int ne2 = ne201 * ne023;

    const ggml_fp16_t alpha = ggml_fp32_to_fp16(1.0f);
    const ggml_fp16_t beta = ggml_fp32_to_fp16(0.0f);

    size_t x_size{};
    size_t y_size{};
    size_t d_size{};
    cl_mem d_X;
    const bool src0h2d = src0->backend != GGML_BACKEND_GPU;
    if (!src0h2d) { // NOLINT
        d_X = (cl_mem) src0->extra;
    } else {
        d_X = r_pool.get(ne0 * sizeof(ggml_fp16_t), &x_size);
    }
    cl_mem d_Y = r_pool.get(ne1 * sizeof(ggml_fp16_t), &y_size);
    cl_mem d_D = w_pool.get(ne2 * sizeof(ggml_fp16_t), &d_size);

    bool src1_cont_rows = nb10 == sizeof(float);
    bool src1_cont_cols = (size_t)nb11 == ne11*sizeof(float);

    // Async copy in background while CPU does vector stuff
    if (src0h2d) {
        for (cl_int i03 = 0; i03 < ne03; i03++) {
            for (cl_int i02 = 0; i02 < ne02; i02++) {
                // copy src0 to device
                const cl_int i0 = i03*ne02 + i02;
                CL_CHECK(ggml_cl_h2d_tensor_2d(d_X, i0*ne001, src0, i03, i02, nullptr));
            }
        }
    }

    // Do all the math before any syscalls like clEnqueueWriteBuffer to allow the compiler to vectorize
    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            // convert src1 to fp16
            // TODO: use multiple threads
            ggml_fp16_t * const tmp = (ggml_fp16_t *) wdata + ne101 * (i03 * ne02 + i02);
            char * src1i = (char *) src1->data + i03*nb13 + i02*nb12;
            if (src1_cont_rows) {
                if (src1_cont_cols) {
                    ggml_fp32_to_fp16_row((float *) src1i, tmp, ne101);
                }
                else {
                    for (cl_int i01 = 0; i01 < ne11; i01++) {
                        ggml_fp32_to_fp16_row((float *) (src1i + i01*nb11), tmp + i01*ne10, ne10);
                    }
                }
            }
            else {
                for (cl_int i01 = 0; i01 < ne11; i01++) {
                    for (cl_int i00 = 0; i00 < ne10; i00++) {
                        // very slow due to no inlining
                        tmp[i01*ne10 + i00] = ggml_fp32_to_fp16(*(float *) (src1i + i01*nb11 + i00*nb10));
                    }
                }
            }
        }
    }

    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            ggml_fp16_t * const tmp = (ggml_fp16_t *) wdata + ne101 * (i03 * ne02 + i02);
            // copy src1 to device
            const cl_int i0 = i03*ne02 + i02;
            const size_t size = sizeof(ggml_fp16_t) * ne101;
            CL_CHECK(clEnqueueWriteBuffer(queue, d_Y, false, i0*size, size, tmp, 0, nullptr, nullptr));
        }
    }
    // Sequence the kernels after the uploads, but do not block the CPU, to work around clblast lacking event_wait_list
    CL_CHECK(clEnqueueBarrierWithWaitList(queue, 0, nullptr, nullptr));

    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            // compute
            const cl_int i0 = i03*ne02 + i02;
            cl_event ev_sgemm;
            clblast::StatusCode status = clblast::Gemm<cl_half>(clblast::Layout::kColMajor,
                                                       clblast::Transpose::kYes, clblast::Transpose::kNo,
                                                       ne01, ne11, ne10,
                                                       alpha,
                                                       d_X, i0*ne001, ne00,
                                                       d_Y, i0*ne101, ne10,
                                                       beta,
                                                       d_D, i0*ne201, ne01,
                                                       &queue, &ev_sgemm);

            if (status != clblast::StatusCode::kSuccess) {
                GGML_ASSERT(false);
            }

            // copy dst to host
            ggml_fp16_t * const tmp = (ggml_fp16_t *) wdata + ne101 * (i03 * ne02 + i02);
            const size_t size = sizeof(ggml_fp16_t) * ne201;
            CL_CHECK(clEnqueueReadBuffer(queue, d_D, false, i0*size, size, tmp, 1, &ev_sgemm, nullptr));
            CL_CHECK(clReleaseEvent(ev_sgemm));
        }
    }

    // Wait for download, then convert everything at once to allow vectorization and avoid context switching
    CL_CHECK(clFinish(queue));
    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            // convert to float
            ggml_fp16_t * const tmp = (ggml_fp16_t *) wdata + ne101 * (i03 * ne02 + i02);
            auto * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);
            ggml_fp16_to_fp32_row(tmp, d, ne201);
        }
    }

    if (src0->backend != GGML_BACKEND_GPU) {
        r_pool.put(d_X, x_size);
    }
    r_pool.put(d_Y, y_size);
    w_pool.put(d_D, d_size);
}

void ggml_cl_mul_mat_q_f32(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    const auto ne00 = static_cast<cl_int>(src0->ne[0]);
    const auto ne01 = static_cast<cl_int>(src0->ne[1]);
    const auto ne02 = static_cast<cl_int>(src0->ne[2]);
    const auto ne03 = static_cast<cl_int>(src0->ne[3]);
    const cl_int ne023 = ne02 * ne03;
    const cl_int ne001 = ne00 * ne01;
    const cl_int ne0 = ne001 * ne023;

    const auto ne10 = static_cast<cl_int>(src1->ne[0]);
    const auto ne11 = static_cast<cl_int>(src1->ne[1]);
    const cl_int ne101 = ne10 * ne11;
    const cl_int ne1 = ne101 * ne023;

    const auto nb2 = static_cast<cl_int>(dst->nb[2]);
    const auto nb3 = static_cast<cl_int>(dst->nb[3]);
    const cl_int ne201 = ne11 * ne01;
    const cl_int ne2 = ne201 * ne023;
    const ggml_type type = src0->type;
    const bool mul_mat_vec = ne11 == 1;

    const float alpha = 1.0f;
    const float beta = 0.0f;
    const size_t ne3 = ggml_type_size(type) * ne0 / ggml_blck_size(type);

    size_t x_size{};
    size_t y_size{};
    size_t d_size{};
    size_t q_size{};
    cl_mem d_X;
    if (!mul_mat_vec) {
        d_X = rw_pool.get(ne0 * sizeof(float), &x_size);
    }
    cl_mem d_Y = r_pool.get(ne1 * sizeof(float), &y_size);
    cl_mem d_D = w_pool.get(ne2 * sizeof(float), &d_size);
    cl_mem d_Q;
    const bool src0h2d = src0->backend == GGML_BACKEND_CPU;
    if (src0h2d) {
        d_Q = r_pool.get(ne3, &q_size);
    } else if (src0->backend == GGML_BACKEND_GPU) {
        d_Q = (cl_mem) src0->extra;
    } else {
        GGML_ASSERT(false);
    }

    cl_kernel* to_fp32_cl = ggml_get_to_fp32_cl(type);
    cl_kernel* dmmv = ggml_get_dequantize_mul_mat_vec_cl(type);
    GGML_ASSERT(to_fp32_cl != nullptr);

    const size_t global_denom = ggml_cl_global_denom(type);
    const size_t generalized_local = ggml_cl_local_size(type);

    // Part 1: Enqueue everything that doesn't need a barrier
    for (cl_int i03 = 0; i03 < ne03; i03++) {
        for (cl_int i02 = 0; i02 < ne02; i02++) {
            cl_event h2d_ev[2];
            const cl_int i0 = i03*ne02 + i02;
            if (src0h2d) {
                // copy src0 to device if necessary
                CL_CHECK(ggml_cl_h2d_tensor_2d(d_Q, i0*ne001, src0, i03, i02, &h2d_ev[1]));
            }
            // copy src1 to device
            CL_CHECK(ggml_cl_h2d_tensor_2d(d_Y, i0*ne101, src1, i03, i02, &h2d_ev[0]));

            cl_uint num_events_in_wait_list = src0h2d ? 2 : 1;
            const cl_int x_offset = i0*ne001;
            const cl_int dst_offset = i0*ne201;
            if (mul_mat_vec) { // specialized dequantize_mul_mat_vec kernel
                // compute
                const cl_int y_offset = i0*ne101;
                const size_t specialized_local = CL_DMMV_BLOCK_SIZE;
                const size_t global = ne01 * specialized_local;
                const cl_int ncols = ne00;
                CL_CHECK(clSetKernelArg(*dmmv, 0, sizeof(cl_mem), &d_Q));
                CL_CHECK(clSetKernelArg(*dmmv, 1, sizeof(cl_int), &x_offset));
                CL_CHECK(clSetKernelArg(*dmmv, 2, sizeof(float) * specialized_local, nullptr));
                CL_CHECK(clSetKernelArg(*dmmv, 3, sizeof(cl_mem), &d_Y));
                CL_CHECK(clSetKernelArg(*dmmv, 4, sizeof(cl_int), &y_offset));
                CL_CHECK(clSetKernelArg(*dmmv, 5, sizeof(cl_mem), &d_D));
                CL_CHECK(clSetKernelArg(*dmmv, 6, sizeof(cl_int), &dst_offset));
                CL_CHECK(clSetKernelArg(*dmmv, 7, sizeof(cl_int), &ncols));
                cl_event kernel_ev;
                CL_CHECK(clEnqueueNDRangeKernel(queue, *dmmv, 1, nullptr, &global, &specialized_local, num_events_in_wait_list, h2d_ev, &kernel_ev));

                // copy dst to host
                auto * d = (float *) ((char *) dst->data + i02*nb2 + i03*nb3);
                const size_t size = sizeof(float) * ne201;
                CL_CHECK(clEnqueueReadBuffer(queue, d_D, false, i0*size, size, d, 1, &kernel_ev, nullptr));
                CL_CHECK(clReleaseEvent(kernel_ev));
            } else { // general dequantization kernel + CLBlast matrix matrix multiplication
                // convert src0 to fp32 on device
                const size_t global = ne001 / global_denom;
                CL_CHECK(clSetKernelArg(*to_fp32_cl, 0, sizeof(cl_mem), &d_Q));
                CL_CHECK(clSetKernelArg(*to_fp32_cl, 1, sizeof(cl_int), &x_offset));
                CL_CHECK(clSetKernelArg(*to_fp32_cl, 2, sizeof(cl_mem), &d_X));
                CL_CHECK(clSetKernelArg(*to_fp32_cl, 3, sizeof(cl_int), &dst_offset));
                CL_CHECK(clEnqueueNDRangeKernel(queue, *to_fp32_cl, 1, nullptr, &global, generalized_local > 0 ? &generalized_local : nullptr, num_events_in_wait_list, h2d_ev, nullptr));
            }
            for (cl_uint i = 0; i < num_events_in_wait_list; i++) {
                CL_CHECK(clReleaseEvent(h2d_ev[i]));
            }
        }
    }

    if (!mul_mat_vec) { // general dequantization kernel + CLBlast matrix matrix multiplication, part 2
        // Sequence the kernels after the conversion without making the CPU wait.
        // This barrier is harmless because the GPU is busy executing the previous kernel
        CL_CHECK(clEnqueueBarrierWithWaitList(queue, 0, nullptr, nullptr));
        for (cl_int i03 = 0; i03 < ne03; i03++) {
            for (cl_int i02 = 0; i02 < ne02; i02++) {
                // compute
                const cl_int i0 = i03*ne02 + i02;
                cl_event kernel_ev;
                clblast::StatusCode status = clblast::Gemm<cl_float>(clblast::Layout::kColMajor,
                                                                     clblast::Transpose::kYes, clblast::Transpose::kNo,
                                                                     ne01, ne11, ne10,
                                                                     alpha,
                                                                     d_X, i0*ne001, ne00,
                                                                     d_Y, i0*ne101, ne10,
                                                                     beta,
                                                                     d_D, i0*ne201, ne01,
                                                                     &queue, &kernel_ev);

                if (status != clblast::StatusCode::kSuccess) {
                    GGML_ASSERT(false);
                }

                // copy dst to host
                auto *d = (float *) ((char *) dst->data + i02 * nb2 + i03 * nb3);
                const size_t size = sizeof(float) * ne201;
                CL_CHECK(clEnqueueReadBuffer(queue, d_D, false, i0*size, size, d, 1, &kernel_ev, nullptr));
                CL_CHECK(clReleaseEvent(kernel_ev));
            }
        }
    }
    CL_CHECK(clFinish(queue));

    if (!mul_mat_vec) {
        rw_pool.put(d_X, x_size);
    }
    r_pool.put(d_Y, y_size);
    w_pool.put(d_D, d_size);
    if (src0h2d) {
        r_pool.put(d_Q, q_size);
    }
}
} // namespace

bool ggml_cl_can_mul_mat(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst) {
    const int64_t ne10 = src1->ne[0];

    const int64_t ne0 = dst->ne[0];
    const int64_t ne1 = dst->ne[1];

    // TODO: find the optimal values for these
    return (src0->type == GGML_TYPE_F32 || src0->type == GGML_TYPE_F16 || ggml_is_quantized(src0->type)) &&
        src1->type == GGML_TYPE_F32 &&
        dst->type == GGML_TYPE_F32 &&
        ((ne0 >= 32 && ne1 >= 32 && ne10 >= 32) || src0->backend == GGML_BACKEND_GPU);
}

namespace {
bool ggml_cl_mul_mat_use_f16(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * /* dst */) {
    // If device doesn't support FP16
    if (!fp16_support) {
        return false;
    }

    size_t src0_sz = ggml_nbytes(src0);
    size_t src1_sz = ggml_nbytes(src1);

    // mul_mat_q: src0 is converted to fp32 on device
    size_t mul_mat_q_transfer = src0_sz + src1_sz;

    // mul_mat_f16: src1 is converted to fp16 on cpu
    size_t mul_mat_f16_transfer = src0_sz + sizeof(ggml_fp16_t) * ggml_nelements(src1);

    // choose the smaller one to transfer to the device
    // TODO: this is not always the best choice due to the overhead of converting to fp16
    return mul_mat_f16_transfer < mul_mat_q_transfer;
}
} // namespace

void ggml_cl_mul_mat(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst, void * wdata, size_t wsize) {
    GGML_ASSERT(ggml_cl_can_mul_mat(src0, src1, dst));

    if (src0->type == GGML_TYPE_F32) {
        ggml_cl_mul_mat_f32(src0, src1, dst);
    }
    else if (src0->type == GGML_TYPE_F16) {
        if (ggml_cl_mul_mat_use_f16(src0, src1, dst)) {
            ggml_cl_mul_mat_f16(src0, src1, dst, wdata, wsize);
        }
        else {
            ggml_cl_mul_mat_q_f32(src0, src1, dst);
        }
    }
    else if (ggml_is_quantized(src0->type)) {
        ggml_cl_mul_mat_q_f32(src0, src1, dst);
    }
    else {
        GGML_ASSERT(false);
    }
}

size_t ggml_cl_mul_mat_get_wsize(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst) {
    if (ggml_cl_mul_mat_use_f16(src0, src1, dst)) {
        return ggml_nelements(src1) * sizeof(ggml_fp16_t);
    }
    return 0;
}

void ggml_cl_transform_tensor(void * data, ggml_tensor * tensor) {
    const auto ne0 = static_cast<cl_int>(tensor->ne[0]);
    const auto ne1 = static_cast<cl_int>(tensor->ne[1]);
    const auto ne2 = static_cast<cl_int>(tensor->ne[2]);
    const auto ne3 = static_cast<cl_int>(tensor->ne[3]);

    const ggml_type type = tensor->type;
    const size_t q_sz = ggml_type_size(type) * ne0 * ne1 * ne2 * ne3 / ggml_blck_size(type);

    size_t q_size;
    cl_mem dst = r_pool.get(q_sz, &q_size);

    tensor->data = data;
    // copy tensor to device
    for (cl_int i3 = 0; i3 < ne3; i3++) {
        for (cl_int i2 = 0; i2 < ne2; i2++) {
            cl_int i = i3 * ne2 + i2;
            CL_CHECK(ggml_cl_h2d_tensor_2d(dst, i*ne0*ne1, tensor, i3, i2, nullptr));
        }
    }

    CL_CHECK(clFinish(queue));

    tensor->extra = dst;
    GGML_ASSERT(tensor->backend == GGML_BACKEND_GPU);
}
