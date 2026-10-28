//******************************************************************************
///
/// @file core/shape/gaussiansplat_vk.h
///
/// Optional Vulkan compute backend for samples==2 / khrEllipse projected path:
/// project -> tile -> EWA blend into a primary-ray framebuffer (CPU fallback).
///
//******************************************************************************

#ifndef POVRAY_CORE_GAUSSIANSPLAT_VK_H
#define POVRAY_CORE_GAUSSIANSPLAT_VK_H

#include "core/shape/gaussiansplat.h"

#include <cstdint>
#include <vector>

namespace pov
{

/// True if compiled with POV_HAVE_VULKAN and a device can be created.
bool GaussianSplatVkAvailable();

/// Last Vulkan frame timing (updated by GaussianSplatVkRenderFrame).
struct GaussianSplatVkTiming
{
    bool valid;
    bool timestampsOk;     ///< True if VK timestamp queries succeeded.
    int width, height;
    unsigned splatCount;
    double hostGpuMs;      ///< Wall time: upload + dispatch + wait + readback.
    double gpuTimestampMs; ///< Device timestamps (compute only), or -1 if N/A.
    char deviceName[256];
    char deviceType[32];   ///< e.g. discrete / integrated / cpu / virtual

    GaussianSplatVkTiming() :
        valid(false), timestampsOk(false), width(0), height(0), splatCount(0),
        hostGpuMs(0), gpuTimestampMs(-1.0)
    {
        deviceName[0] = '\0';
        deviceType[0] = '\0';
    }
};

const GaussianSplatVkTiming& GaussianSplatVkLastTiming();

/// Packed world-space splat for GPU project (std430 layout, 5x vec4).
struct GaussianSplatGpuSplat
{
    float posAlpha[4]; // xyz, opacity
    float sig0[4];     // S00,S01,S02,S11
    float sig1[4];     // S12,S22,_,_
    float rgbNx[4];    // r,g,b,nx
    float nyNz[4];     // ny,nz,_,_
};

/// GPU framebuffer readback for primary rays.
struct GaussianSplatVkFramebuffer
{
    bool valid;
    int width;
    int height;
    Vector3d origin, right, up, forward;
    double fx, fy;
    std::vector<float> rgba;   // premul rgb + alpha, size 4*W*H
    std::vector<float> depth;  // view-forward depth, size W*H
    std::vector<float> normal; // xyz + pad, size 4*W*H

    GaussianSplatVkFramebuffer() :
        valid(false), width(0), height(0), fx(0), fy(0) {}
};

/// Render one camera frame via Vulkan compute. Returns false -> caller uses CPU.
bool GaussianSplatVkRenderFrame(const GaussianSplatCloud& cloud,
                                const GaussianSplatCamBasis& cam,
                                const std::vector<GaussianSplatGpuSplat>& splats,
                                GaussianSplatVkFramebuffer& out);

/// Sample framebuffer at POV screenU/screenV (pixels from centre). false = miss.
bool GaussianSplatVkSamplePixel(const GaussianSplatVkFramebuffer& fb,
                                double screenU, double screenV,
                                GaussianSplatSegmentResult& out);

} // namespace pov

#endif // POVRAY_CORE_GAUSSIANSPLAT_VK_H
