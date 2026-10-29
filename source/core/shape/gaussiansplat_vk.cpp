//******************************************************************************
///
/// @file core/shape/gaussiansplat_vk.cpp
///
/// Vulkan compute path for Gaussian splat project / tile / blend.
/// Built only when POV_HAVE_VULKAN is defined; otherwise stubs return false.
///
//******************************************************************************

#include "core/shape/gaussiansplat_vk.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace pov
{

namespace
{
thread_local GaussianSplatVkTiming gLastTiming;
} // namespace

#ifndef POV_HAVE_VULKAN

bool GaussianSplatVkAvailable()
{
    return false;
}

const GaussianSplatVkTiming& GaussianSplatVkLastTiming()
{
    return gLastTiming;
}

bool GaussianSplatVkRenderFrame(const GaussianSplatCloud&,
                                const GaussianSplatCamBasis&,
                                const std::vector<GaussianSplatGpuSplat>&,
                                GaussianSplatVkFramebuffer& out)
{
    out.valid = false;
    return false;
}

bool GaussianSplatVkSamplePixel(const GaussianSplatVkFramebuffer&,
                                double, double, GaussianSplatSegmentResult&)
{
    return false;
}

#else // POV_HAVE_VULKAN

#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk.h"

#include "core/shape/shaders/gsplat_project_spv.inc"
#include "core/shape/shaders/gsplat_tile_count_spv.inc"
#include "core/shape/shaders/gsplat_tile_write_spv.inc"
#include "core/shape/shaders/gsplat_blend_spv.inc"

namespace
{

constexpr int kTileSize = 16;
constexpr uint32_t kMaxHitsPowerCullLegacy = 8; // SuperSplat mahalanobis^2

struct DeviceContext
{
    bool ok;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    VkCommandPool cmdPool;
    VkDescriptorPool descPool;
    VkPipelineCache pipeCache;

    VkShaderModule shProject;
    VkShaderModule shTileCount;
    VkShaderModule shTileWrite;
    VkShaderModule shBlend;

    VkDescriptorSetLayout dslProject;
    VkDescriptorSetLayout dslTileCount;
    VkDescriptorSetLayout dslTileWrite;
    VkDescriptorSetLayout dslBlend;

    VkPipelineLayout plProject;
    VkPipelineLayout plTileCount;
    VkPipelineLayout plTileWrite;
    VkPipelineLayout plBlend;

    VkPipeline pipeProject;
    VkPipeline pipeTileCount;
    VkPipeline pipeTileWrite;
    VkPipeline pipeBlend;

    VkQueryPool queryPool;
    float timestampPeriodNs;
    uint32_t timestampBits;
    bool loggedInit;
    char deviceName[256];
    char deviceType[32];

    DeviceContext() :
        ok(false), instance(VK_NULL_HANDLE), phys(VK_NULL_HANDLE), device(VK_NULL_HANDLE),
        queue(VK_NULL_HANDLE), queueFamily(0), cmdPool(VK_NULL_HANDLE),
        descPool(VK_NULL_HANDLE), pipeCache(VK_NULL_HANDLE),
        shProject(VK_NULL_HANDLE), shTileCount(VK_NULL_HANDLE),
        shTileWrite(VK_NULL_HANDLE), shBlend(VK_NULL_HANDLE),
        dslProject(VK_NULL_HANDLE), dslTileCount(VK_NULL_HANDLE),
        dslTileWrite(VK_NULL_HANDLE), dslBlend(VK_NULL_HANDLE),
        plProject(VK_NULL_HANDLE), plTileCount(VK_NULL_HANDLE),
        plTileWrite(VK_NULL_HANDLE), plBlend(VK_NULL_HANDLE),
        pipeProject(VK_NULL_HANDLE), pipeTileCount(VK_NULL_HANDLE),
        pipeTileWrite(VK_NULL_HANDLE), pipeBlend(VK_NULL_HANDLE),
        queryPool(VK_NULL_HANDLE), timestampPeriodNs(0), timestampBits(0),
        loggedInit(false)
    {
        deviceName[0] = '\0';
        deviceType[0] = '\0';
    }
};

std::mutex gDevMutex;
std::mutex gSubmitMutex;
DeviceContext gDev;

const char *DeviceTypeName(VkPhysicalDeviceType t)
{
    switch (t)
    {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
        default:                                    return "other";
    }
}

bool VkCheck(VkResult r)
{
    return r == VK_SUCCESS;
}

VkShaderModule MakeShader(VkDevice dev, const uint32_t *words, size_t bytes)
{
    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = words;
    VkShaderModule m = VK_NULL_HANDLE;
    if (!VkCheck(vkCreateShaderModule(dev, &ci, nullptr, &m)))
        return VK_NULL_HANDLE;
    return m;
}

VkDescriptorSetLayout MakeDSL(VkDevice dev, const std::vector<VkDescriptorType> &types)
{
    std::vector<VkDescriptorSetLayoutBinding> binds(types.size());
    for (size_t i = 0; i < types.size(); ++i)
    {
        binds[i] = {};
        binds[i].binding = static_cast<uint32_t>(i);
        binds[i].descriptorType = types[i];
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<uint32_t>(binds.size());
    ci.pBindings = binds.data();
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (!VkCheck(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &dsl)))
        return VK_NULL_HANDLE;
    return dsl;
}

VkPipelineLayout MakePL(VkDevice dev, VkDescriptorSetLayout dsl)
{
    VkPipelineLayoutCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &dsl;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (!VkCheck(vkCreatePipelineLayout(dev, &ci, nullptr, &pl)))
        return VK_NULL_HANDLE;
    return pl;
}

VkPipeline MakeComputePipe(VkDevice dev, VkPipelineCache cache, VkShaderModule sh,
                           VkPipelineLayout pl)
{
    VkComputePipelineCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = sh;
    ci.stage.pName = "main";
    ci.layout = pl;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!VkCheck(vkCreateComputePipelines(dev, cache, 1, &ci, nullptr, &pipe)))
        return VK_NULL_HANDLE;
    return pipe;
}

struct Buffer
{
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *mapped;
    Buffer() : buf(VK_NULL_HANDLE), mem(VK_NULL_HANDLE), size(0), mapped(nullptr) {}
};

uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

bool CreateBuffer(DeviceContext &d, VkDeviceSize size, VkBufferUsageFlags usage, Buffer &out)
{
    out = Buffer();
    out.size = size;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!VkCheck(vkCreateBuffer(d.device, &bi, nullptr, &out.buf)))
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device, out.buf, &req);
    uint32_t mt = FindMemoryType(d.phys, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    if (!VkCheck(vkAllocateMemory(d.device, &ai, nullptr, &out.mem)))
        return false;
    if (!VkCheck(vkBindBufferMemory(d.device, out.buf, out.mem, 0)))
        return false;
    if (!VkCheck(vkMapMemory(d.device, out.mem, 0, size, 0, &out.mapped)))
        return false;
    return true;
}

void DestroyBuffer(DeviceContext &d, Buffer &b)
{
    if (b.mapped && b.mem)
        vkUnmapMemory(d.device, b.mem);
    if (b.buf)
        vkDestroyBuffer(d.device, b.buf, nullptr);
    if (b.mem)
        vkFreeMemory(d.device, b.mem, nullptr);
    b = Buffer();
}

void WriteDescBuffer(VkDevice dev, VkDescriptorSet set, uint32_t binding,
                     VkDescriptorType type, VkBuffer buf, VkDeviceSize size)
{
    VkDescriptorBufferInfo bi = {};
    bi.buffer = buf;
    bi.offset = 0;
    bi.range = size;
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}

bool InitDevice()
{
    std::lock_guard<std::mutex> lock(gDevMutex);
    if (gDev.ok)
        return true;
    if (volkInitialize() != VK_SUCCESS)
        return false;

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "povray-gsplat";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "povray";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!VkCheck(vkCreateInstance(&ici, nullptr, &gDev.instance)))
        return false;
    volkLoadInstance(gDev.instance);

    uint32_t nPhys = 0;
    vkEnumeratePhysicalDevices(gDev.instance, &nPhys, nullptr);
    if (nPhys == 0)
        return false;
    std::vector<VkPhysicalDevice> phys(nPhys);
    vkEnumeratePhysicalDevices(gDev.instance, &nPhys, phys.data());
    gDev.phys = phys[0];
    // Prefer discrete, else first with compute queue.
    for (VkPhysicalDevice p : phys)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            gDev.phys = p;
            break;
        }
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gDev.phys, &props);
    std::snprintf(gDev.deviceName, sizeof(gDev.deviceName), "%s", props.deviceName);
    std::snprintf(gDev.deviceType, sizeof(gDev.deviceType), "%s", DeviceTypeName(props.deviceType));
    gDev.timestampPeriodNs = props.limits.timestampPeriod;

    uint32_t nQ = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gDev.phys, &nQ, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(nQ);
    vkGetPhysicalDeviceQueueFamilyProperties(gDev.phys, &nQ, qfp.data());
    gDev.queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < nQ; ++i)
    {
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            gDev.queueFamily = i;
            gDev.timestampBits = qfp[i].timestampValidBits;
            break;
        }
    }
    if (gDev.queueFamily == UINT32_MAX)
        return false;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gDev.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (!VkCheck(vkCreateDevice(gDev.phys, &dci, nullptr, &gDev.device)))
        return false;
    volkLoadDevice(gDev.device);
    vkGetDeviceQueue(gDev.device, gDev.queueFamily, 0, &gDev.queue);

    VkCommandPoolCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = gDev.queueFamily;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (!VkCheck(vkCreateCommandPool(gDev.device, &cpci, nullptr, &gDev.cmdPool)))
        return false;

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16 }
    };
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 16;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = poolSizes;
    if (!VkCheck(vkCreateDescriptorPool(gDev.device, &dpci, nullptr, &gDev.descPool)))
        return false;

    VkPipelineCacheCreateInfo pcci = {};
    pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (!VkCheck(vkCreatePipelineCache(gDev.device, &pcci, nullptr, &gDev.pipeCache)))
        return false;

    gDev.shProject = MakeShader(gDev.device, gsplat_project_spv, gsplat_project_spv_size);
    gDev.shTileCount = MakeShader(gDev.device, gsplat_tile_count_spv, gsplat_tile_count_spv_size);
    gDev.shTileWrite = MakeShader(gDev.device, gsplat_tile_write_spv, gsplat_tile_write_spv_size);
    gDev.shBlend = MakeShader(gDev.device, gsplat_blend_spv, gsplat_blend_spv_size);
    if (!gDev.shProject || !gDev.shTileCount || !gDev.shTileWrite || !gDev.shBlend)
        return false;

    gDev.dslProject = MakeDSL(gDev.device, {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    });
    gDev.dslTileCount = MakeDSL(gDev.device, {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    });
    gDev.dslTileWrite = MakeDSL(gDev.device, {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    });
    gDev.dslBlend = MakeDSL(gDev.device, {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    });
    if (!gDev.dslProject || !gDev.dslTileCount || !gDev.dslTileWrite || !gDev.dslBlend)
        return false;

    gDev.plProject = MakePL(gDev.device, gDev.dslProject);
    gDev.plTileCount = MakePL(gDev.device, gDev.dslTileCount);
    gDev.plTileWrite = MakePL(gDev.device, gDev.dslTileWrite);
    gDev.plBlend = MakePL(gDev.device, gDev.dslBlend);

    gDev.pipeProject = MakeComputePipe(gDev.device, gDev.pipeCache, gDev.shProject, gDev.plProject);
    gDev.pipeTileCount = MakeComputePipe(gDev.device, gDev.pipeCache, gDev.shTileCount, gDev.plTileCount);
    gDev.pipeTileWrite = MakeComputePipe(gDev.device, gDev.pipeCache, gDev.shTileWrite, gDev.plTileWrite);
    gDev.pipeBlend = MakeComputePipe(gDev.device, gDev.pipeCache, gDev.shBlend, gDev.plBlend);
    if (!gDev.pipeProject || !gDev.pipeTileCount || !gDev.pipeTileWrite || !gDev.pipeBlend)
        return false;

    // 4 timestamps: pass0 begin/end, pass1 begin/end.
    if (gDev.timestampBits > 0)
    {
        VkQueryPoolCreateInfo qpci = {};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = 4;
        if (!VkCheck(vkCreateQueryPool(gDev.device, &qpci, nullptr, &gDev.queryPool)))
            gDev.queryPool = VK_NULL_HANDLE;
    }

    gDev.ok = true;
    if (!gDev.loggedInit)
    {
        std::fprintf(stderr,
            "gaussian_splat vulkan: libvulkan loaded, device='%s' type=%s "
            "timestamps=%s period=%.3fns\n",
            gDev.deviceName, gDev.deviceType,
            (gDev.queryPool != VK_NULL_HANDLE) ? "yes" : "no",
            static_cast<double>(gDev.timestampPeriodNs));
        std::fflush(stderr);
        gDev.loggedInit = true;
    }
    return true;
}

bool AllocSet(DeviceContext &d, VkDescriptorSetLayout dsl, VkDescriptorSet &set)
{
    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = d.descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &dsl;
    return VkCheck(vkAllocateDescriptorSets(d.device, &ai, &set));
}

void MemoryBarrierSSBO(VkCommandBuffer cmd)
{
    VkMemoryBarrier mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb, 0, nullptr, 0, nullptr);
}

// std140 vec4 arrays for uniforms
struct FrameUBO
{
    float origin[4];
    float right[4];
    float up[4];
    float forward[4];
    float screen[4];
    float cull[4];
};

struct TileUBO
{
    float grid[4];
    float dims[4];
};

struct BlendUBO
{
    float grid[4];
    float dims[4];
    float params[4];
    float flags[4];
};

} // namespace

bool GaussianSplatVkAvailable()
{
    return InitDevice();
}

const GaussianSplatVkTiming& GaussianSplatVkLastTiming()
{
    return gLastTiming;
}

bool GaussianSplatVkSamplePixel(const GaussianSplatVkFramebuffer& fb,
                                double screenU, double screenV,
                                GaussianSplatSegmentResult& out)
{
    out.colour = Vector3d(0.0, 0.0, 0.0);
    out.normal = Vector3d(0.0, 0.0, 0.0);
    out.transmittance = 1.0;
    out.depth = 0.0;
    out.valid = false;
    if (!fb.valid || fb.width <= 0 || fb.height <= 0)
        return false;

    const int px = static_cast<int>(std::floor(screenU + 0.5 * fb.width));
    const int py = static_cast<int>(std::floor(0.5 * fb.height - screenV));
    if (px < 0 || py < 0 || px >= fb.width || py >= fb.height)
        return false;

    const size_t pix = static_cast<size_t>(py) * static_cast<size_t>(fb.width) + static_cast<size_t>(px);
    const float *rgba = &fb.rgba[pix * 4];
    const float alpha = rgba[3];
    if (alpha <= 0.0f)
        return false;

    out.colour = Vector3d(rgba[0], rgba[1], rgba[2]);
    out.transmittance = 1.0 - static_cast<double>(alpha);
    out.depth = fb.depth[pix];
    if (!fb.normal.empty())
    {
        const float *n = &fb.normal[pix * 4];
        out.normal = Vector3d(n[0], n[1], n[2]);
    }
    out.valid = true;
    return true;
}

bool GaussianSplatVkRenderFrame(const GaussianSplatCloud& cloud,
                                const GaussianSplatCamBasis& cam,
                                const std::vector<GaussianSplatGpuSplat>& splats,
                                GaussianSplatVkFramebuffer& out)
{
    out.valid = false;
    gLastTiming = GaussianSplatVkTiming();
    if (splats.empty() || !cam.camValid)
        return false;
    if (!InitDevice())
        return false;

    std::lock_guard<std::mutex> lock(gSubmitMutex);
    DeviceContext &d = gDev;
    const auto hostT0 = std::chrono::steady_clock::now();
    const bool useTs = (d.queryPool != VK_NULL_HANDLE);
    const uint32_t n = static_cast<uint32_t>(splats.size());
    const int imgW = std::max(1, static_cast<int>(std::lround(cam.screenW)));
    const int imgH = std::max(1, static_cast<int>(std::lround(cam.screenH)));
    if (imgW <= 0 || imgH <= 0)
        return false;

    const float pad = static_cast<float>(kTileSize);
    const float tileOriginU = -0.5f * static_cast<float>(imgW) - pad;
    const float tileOriginV = -0.5f * static_cast<float>(imgH) - pad;
    const float spanU = static_cast<float>(imgW) + 2.0f * pad;
    const float spanV = static_cast<float>(imgH) + 2.0f * pad;
    const int tilesX = std::max(1, static_cast<int>(std::ceil(spanU / kTileSize)));
    const int tilesY = std::max(1, static_cast<int>(std::ceil(spanV / kTileSize)));
    const int nTiles = tilesX * tilesY;

    // ProjEntry size: 14 floats + 4 ints = 14*4+16 = 72
    const VkDeviceSize splatBytes = sizeof(GaussianSplatGpuSplat) * n;
    const VkDeviceSize projBytes = 72ull * n;
    const VkDeviceSize countBytes = sizeof(uint32_t) * static_cast<VkDeviceSize>(nTiles);
    const VkDeviceSize metaBytes = sizeof(uint32_t) * 4;
    const VkDeviceSize offBytes = sizeof(int32_t) * static_cast<VkDeviceSize>(nTiles + 1);
    const VkDeviceSize pix = static_cast<VkDeviceSize>(imgW) * static_cast<VkDeviceSize>(imgH);
    const VkDeviceSize rgbaBytes = pix * sizeof(float) * 4;
    const VkDeviceSize depthBytes = pix * sizeof(float);
    const VkDeviceSize normalBytes = pix * sizeof(float) * 4;

    Buffer bSplats, bProjs, bCounts, bMeta, bOffsets, bCursor, bIndices;
    Buffer bRGBA, bDepth, bNormal, bFrameUBO, bTileUBO, bBlendUBO;

    auto cleanup = [&]() {
        DestroyBuffer(d, bSplats); DestroyBuffer(d, bProjs);
        DestroyBuffer(d, bCounts); DestroyBuffer(d, bMeta);
        DestroyBuffer(d, bOffsets); DestroyBuffer(d, bCursor); DestroyBuffer(d, bIndices);
        DestroyBuffer(d, bRGBA); DestroyBuffer(d, bDepth); DestroyBuffer(d, bNormal);
        DestroyBuffer(d, bFrameUBO); DestroyBuffer(d, bTileUBO); DestroyBuffer(d, bBlendUBO);
        vkResetDescriptorPool(d.device, d.descPool, 0);
    };

    const VkBufferUsageFlags SSBO = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkBufferUsageFlags UBO = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    if (!CreateBuffer(d, splatBytes, SSBO, bSplats) ||
        !CreateBuffer(d, projBytes, SSBO, bProjs) ||
        !CreateBuffer(d, countBytes, SSBO, bCounts) ||
        !CreateBuffer(d, metaBytes, SSBO, bMeta) ||
        !CreateBuffer(d, offBytes, SSBO, bOffsets) ||
        !CreateBuffer(d, countBytes, SSBO, bCursor) ||
        !CreateBuffer(d, rgbaBytes, SSBO, bRGBA) ||
        !CreateBuffer(d, depthBytes, SSBO, bDepth) ||
        !CreateBuffer(d, normalBytes, SSBO, bNormal) ||
        !CreateBuffer(d, sizeof(FrameUBO), UBO, bFrameUBO) ||
        !CreateBuffer(d, sizeof(TileUBO), UBO, bTileUBO) ||
        !CreateBuffer(d, sizeof(BlendUBO), UBO, bBlendUBO))
    {
        cleanup();
        return false;
    }

    std::memcpy(bSplats.mapped, splats.data(), static_cast<size_t>(splatBytes));
    std::memset(bCounts.mapped, 0, static_cast<size_t>(countBytes));
    std::memset(bMeta.mapped, 0, static_cast<size_t>(metaBytes));

    FrameUBO fubo = {};
    fubo.origin[0] = static_cast<float>(cam.origin[X]);
    fubo.origin[1] = static_cast<float>(cam.origin[Y]);
    fubo.origin[2] = static_cast<float>(cam.origin[Z]);
    fubo.right[0] = static_cast<float>(cam.right[X]);
    fubo.right[1] = static_cast<float>(cam.right[Y]);
    fubo.right[2] = static_cast<float>(cam.right[Z]);
    fubo.right[3] = static_cast<float>(cam.fx);
    fubo.up[0] = static_cast<float>(cam.up[X]);
    fubo.up[1] = static_cast<float>(cam.up[Y]);
    fubo.up[2] = static_cast<float>(cam.up[Z]);
    fubo.up[3] = static_cast<float>(cam.fy);
    fubo.forward[0] = static_cast<float>(cam.forward[X]);
    fubo.forward[1] = static_cast<float>(cam.forward[Y]);
    fubo.forward[2] = static_cast<float>(cam.forward[Z]);
    fubo.forward[3] = static_cast<float>(n);
    fubo.screen[0] = static_cast<float>(imgW);
    fubo.screen[1] = static_cast<float>(imgH);
    fubo.screen[2] = static_cast<float>(cloud.sigmaExtent);
    fubo.screen[3] = static_cast<float>(cloud.ewaFilter);
    fubo.cull[0] = static_cast<float>(cloud.opacityCutoff);
    fubo.cull[1] = static_cast<float>(cloud.maxQuadFrac);
    fubo.cull[2] = cloud.khrEllipse ? 1.0f : 0.0f;
    std::memcpy(bFrameUBO.mapped, &fubo, sizeof(fubo));

    TileUBO tubo = {};
    tubo.grid[0] = tileOriginU;
    tubo.grid[1] = tileOriginV;
    tubo.grid[2] = static_cast<float>(kTileSize);
    tubo.grid[3] = static_cast<float>(n);
    tubo.dims[0] = static_cast<float>(tilesX);
    tubo.dims[1] = static_cast<float>(tilesY);
    tubo.dims[2] = static_cast<float>(cloud.sigmaExtent);
    // maxKeys filled after count
    std::memcpy(bTileUBO.mapped, &tubo, sizeof(tubo));

    VkDescriptorSet setProject = VK_NULL_HANDLE, setCount = VK_NULL_HANDLE;
    if (!AllocSet(d, d.dslProject, setProject) || !AllocSet(d, d.dslTileCount, setCount))
    {
        cleanup();
        return false;
    }
    WriteDescBuffer(d.device, setProject, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bSplats.buf, splatBytes);
    WriteDescBuffer(d.device, setProject, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bProjs.buf, projBytes);
    WriteDescBuffer(d.device, setProject, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bFrameUBO.buf, sizeof(FrameUBO));

    WriteDescBuffer(d.device, setCount, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bProjs.buf, projBytes);
    WriteDescBuffer(d.device, setCount, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bCounts.buf, countBytes);
    WriteDescBuffer(d.device, setCount, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bMeta.buf, metaBytes);
    WriteDescBuffer(d.device, setCount, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bTileUBO.buf, sizeof(TileUBO));

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = d.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!VkCheck(vkAllocateCommandBuffers(d.device, &cbai, &cmd)))
    {
        cleanup();
        return false;
    }

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    if (useTs)
    {
        vkCmdResetQueryPool(cmd, d.queryPool, 0, 4);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, d.queryPool, 0);
    }

    const uint32_t groupsN = (n + 255u) / 256u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeProject);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.plProject, 0, 1, &setProject, 0, nullptr);
    vkCmdDispatch(cmd, groupsN, 1, 1);
    MemoryBarrierSSBO(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeTileCount);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.plTileCount, 0, 1, &setCount, 0, nullptr);
    vkCmdDispatch(cmd, groupsN, 1, 1);
    MemoryBarrierSSBO(cmd);
    if (useTs)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.queryPool, 1);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (!VkCheck(vkQueueSubmit(d.queue, 1, &si, VK_NULL_HANDLE)) ||
        !VkCheck(vkQueueWaitIdle(d.queue)))
    {
        vkFreeCommandBuffers(d.device, d.cmdPool, 1, &cmd);
        cleanup();
        return false;
    }

    // Prefix sum on CPU from tile counts.
    const uint32_t *counts = static_cast<const uint32_t *>(bCounts.mapped);
    std::vector<int32_t> offsets(static_cast<size_t>(nTiles + 1));
    int64_t total = 0;
    for (int t = 0; t < nTiles; ++t)
    {
        offsets[static_cast<size_t>(t)] = static_cast<int32_t>(total);
        total += counts[t];
    }
    offsets[static_cast<size_t>(nTiles)] = static_cast<int32_t>(total);
    if (total <= 0)
    {
        // No coverage - empty frame is valid miss.
        out.width = imgW;
        out.height = imgH;
        out.origin = cam.origin;
        out.right = cam.right;
        out.up = cam.up;
        out.forward = cam.forward;
        out.fx = cam.fx;
        out.fy = cam.fy;
        out.rgba.assign(static_cast<size_t>(pix) * 4, 0.0f);
        out.depth.assign(static_cast<size_t>(pix), 0.0f);
        out.normal.assign(static_cast<size_t>(pix) * 4, 0.0f);
        out.valid = true;
        vkFreeCommandBuffers(d.device, d.cmdPool, 1, &cmd);
        cleanup();
        return true;
    }

    const VkDeviceSize idxBytes = sizeof(int32_t) * static_cast<VkDeviceSize>(total);
    if (!CreateBuffer(d, idxBytes, SSBO, bIndices))
    {
        vkFreeCommandBuffers(d.device, d.cmdPool, 1, &cmd);
        cleanup();
        return false;
    }

    std::memcpy(bOffsets.mapped, offsets.data(), static_cast<size_t>(offBytes));
    // cursor starts at offsets[t]
    std::memcpy(bCursor.mapped, offsets.data(), static_cast<size_t>(countBytes));
    tubo.dims[3] = static_cast<float>(total);
    std::memcpy(bTileUBO.mapped, &tubo, sizeof(tubo));

    BlendUBO bubo = {};
    bubo.grid[0] = tileOriginU;
    bubo.grid[1] = tileOriginV;
    bubo.grid[2] = static_cast<float>(kTileSize);
    bubo.grid[3] = static_cast<float>(tilesX);
    bubo.dims[0] = static_cast<float>(imgW);
    bubo.dims[1] = static_cast<float>(imgH);
    bubo.dims[2] = static_cast<float>(tilesY);
    bubo.dims[3] = static_cast<float>(cloud.sigmaExtent);
    bubo.params[0] = static_cast<float>(cloud.opacityCutoff);
    bubo.params[1] = static_cast<float>(cloud.alphaClamp);
    bubo.params[2] = static_cast<float>(cloud.alphaStop);
    bubo.params[3] = static_cast<float>(kMaxHitsPowerCullLegacy);
    bubo.flags[0] = cloud.khrEllipse ? 1.0f : 0.0f;
    bubo.flags[1] = cloud.khrSortCameraDistance ? 1.0f : 0.0f;
    bubo.flags[2] = cloud.cullSingletonHits ? 1.0f : 0.0f;
    bubo.flags[3] = (cloud.lightingMode == GaussianSplatLightingMode::Relight) ? 1.0f : 0.0f;
    std::memcpy(bBlendUBO.mapped, &bubo, sizeof(bubo));

    VkDescriptorSet setWrite = VK_NULL_HANDLE, setBlend = VK_NULL_HANDLE;
    if (!AllocSet(d, d.dslTileWrite, setWrite) || !AllocSet(d, d.dslBlend, setBlend))
    {
        vkFreeCommandBuffers(d.device, d.cmdPool, 1, &cmd);
        cleanup();
        return false;
    }
    WriteDescBuffer(d.device, setWrite, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bProjs.buf, projBytes);
    WriteDescBuffer(d.device, setWrite, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bOffsets.buf, offBytes);
    WriteDescBuffer(d.device, setWrite, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bCursor.buf, countBytes);
    WriteDescBuffer(d.device, setWrite, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bIndices.buf, idxBytes);
    WriteDescBuffer(d.device, setWrite, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bTileUBO.buf, sizeof(TileUBO));

    WriteDescBuffer(d.device, setBlend, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bProjs.buf, projBytes);
    WriteDescBuffer(d.device, setBlend, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bOffsets.buf, offBytes);
    WriteDescBuffer(d.device, setBlend, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bIndices.buf, idxBytes);
    WriteDescBuffer(d.device, setBlend, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bRGBA.buf, rgbaBytes);
    WriteDescBuffer(d.device, setBlend, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bDepth.buf, depthBytes);
    WriteDescBuffer(d.device, setBlend, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bNormal.buf, normalBytes);
    WriteDescBuffer(d.device, setBlend, 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bBlendUBO.buf, sizeof(BlendUBO));

    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    if (useTs)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, d.queryPool, 2);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeTileWrite);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.plTileWrite, 0, 1, &setWrite, 0, nullptr);
    vkCmdDispatch(cmd, groupsN, 1, 1);
    MemoryBarrierSSBO(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeBlend);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.plBlend, 0, 1, &setBlend, 0, nullptr);
    const uint32_t gx = (static_cast<uint32_t>(imgW) + 7u) / 8u;
    const uint32_t gy = (static_cast<uint32_t>(imgH) + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
    MemoryBarrierSSBO(cmd);
    if (useTs)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.queryPool, 3);
    vkEndCommandBuffer(cmd);

    if (!VkCheck(vkQueueSubmit(d.queue, 1, &si, VK_NULL_HANDLE)) ||
        !VkCheck(vkQueueWaitIdle(d.queue)))
    {
        vkFreeCommandBuffers(d.device, d.cmdPool, 1, &cmd);
        cleanup();
        return false;
    }

    out.width = imgW;
    out.height = imgH;
    out.origin = cam.origin;
    out.right = cam.right;
    out.up = cam.up;
    out.forward = cam.forward;
    out.fx = cam.fx;
    out.fy = cam.fy;
    out.rgba.resize(static_cast<size_t>(pix) * 4);
    out.depth.resize(static_cast<size_t>(pix));
    out.normal.resize(static_cast<size_t>(pix) * 4);
    std::memcpy(out.rgba.data(), bRGBA.mapped, static_cast<size_t>(rgbaBytes));
    std::memcpy(out.depth.data(), bDepth.mapped, static_cast<size_t>(depthBytes));
    std::memcpy(out.normal.data(), bNormal.mapped, static_cast<size_t>(normalBytes));
    out.valid = true;

    double gpuTsMs = -1.0;
    bool tsOk = false;
    if (useTs)
    {
        uint64_t ts[4] = {};
        if (VkCheck(vkGetQueryPoolResults(d.device, d.queryPool, 0, 4, sizeof(ts), ts,
                                          sizeof(uint64_t), VK_QUERY_RESULT_64_BIT)))
        {
            const double nsPerTick = static_cast<double>(d.timestampPeriodNs);
            const double pass0 = (static_cast<double>(ts[1] - ts[0]) * nsPerTick) * 1.0e-6;
            const double pass1 = (static_cast<double>(ts[3] - ts[2]) * nsPerTick) * 1.0e-6;
            gpuTsMs = pass0 + pass1;
            tsOk = true;
        }
    }
    const auto hostT1 = std::chrono::steady_clock::now();
    const double hostMs =
        std::chrono::duration<double, std::milli>(hostT1 - hostT0).count();

    gLastTiming.valid = true;
    gLastTiming.timestampsOk = tsOk;
    gLastTiming.width = imgW;
    gLastTiming.height = imgH;
    gLastTiming.splatCount = n;
    gLastTiming.hostGpuMs = hostMs;
    gLastTiming.gpuTimestampMs = gpuTsMs;
    std::snprintf(gLastTiming.deviceName, sizeof(gLastTiming.deviceName), "%s", d.deviceName);
    std::snprintf(gLastTiming.deviceType, sizeof(gLastTiming.deviceType), "%s", d.deviceType);

    if (tsOk)
    {
        std::fprintf(stderr,
            "gaussian_splat vulkan frame: %dx%d splats=%u host_gpu=%.2fms gpu_ts=%.2fms "
            "device='%s' (%s)\n",
            imgW, imgH, n, hostMs, gpuTsMs, d.deviceName, d.deviceType);
    }
    else
    {
        std::fprintf(stderr,
            "gaussian_splat vulkan frame: %dx%d splats=%u host_gpu=%.2fms gpu_ts=n/a "
            "device='%s' (%s)\n",
            imgW, imgH, n, hostMs, d.deviceName, d.deviceType);
    }
    std::fflush(stderr);

    vkFreeCommandBuffers(d.device, d.cmdPool, 1, &cmd);
    cleanup();
    return true;
}

#endif // POV_HAVE_VULKAN

} // namespace pov
