//******************************************************************************
///
/// @file core/shape/gaussiansplat.cpp
///
/// Experimental 3D Gaussian Splatting cloud.
///
/// samples==2: SuperSplat/Kerbl project / tile / depth-sort / 2D EWA (CPU),
///   including the training-rasterizer +0.3px cov low-pass.
/// khrEllipse: KHR_gaussian_splatting ellipse kernel (ratified):
///   Sigma = M C C^T M^T, Sigma' = J W Sigma W^T J^T (no +0.3), 3-sigma,
///   cameraDistance sort, float blend then clamp, SH via R^{-1}*viewDir,
///   no volume refill on primary miss; secondary = orthographic ray-plane
///   Sigma projection (spec forward pass is camera-primary).
/// POV finish / interior IOR = optional relighting (allowed by the spec).
/// samples<=1 peak and samples>=3 Vol3DGS remain along-ray (GI / hybrid).
///
//******************************************************************************

#include "core/shape/gaussiansplat.h"
#include "core/shape/gaussiansplat_vk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "core/bounding/boundingbox.h"
#include "core/material/pigment.h"
#include "core/material/texture.h"
#include "core/math/matrix.h"
#include "core/render/ray.h"
#include "core/scene/tracethreaddata.h"

#include "base/povdebug.h"

namespace pov
{

namespace
{

const DBL kShC0 = 0.28209479177387814;
const DBL kShC1 = 0.4886025119029199;
const DBL kShC2[] = {
    1.0925484305920792,
    -1.0925484305920792,
    0.31539156525252005,
    -1.0925484305920792,
    0.5462742152960396
};
const DBL kShC3[] = {
    -0.5900435899266435,
    2.890611442640554,
    -0.4570457994644658,
    0.3731763325901154,
    -0.4570457994644658,
    1.445305721320277,
    -0.5900435899266435
};

const DBL kDepthTolerance = 1.0e-6;
const DBL kWeightEps = 1.0e-4;
const int kDefaultMaxHits = 65536;
const int kHardMaxHits = 262144;
/// SuperSplat quad edge at UV=1 => mahalanobis^2 = 8 (2*sqrt(2) sigma); soft cull only.
const DBL kProjPowerCullSuperSplat = 8.0;
const int kProjTileSize = 16;
/// Veiled-baseline: no min/max pixel / floater culls (those came after the
/// "verschleiert" feedback). Extent still used for tile coverage.
const DBL kSqrtTwoPi = 2.5066282746310002; // sqrt(2*pi)

inline DBL Clamp01(DBL v)
{
    return (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
}

/// Plain Gaussian falloff (pre-normExp / pre-size-cull veiled baseline).
inline DBL GaussPower(DBL power)
{
    return std::exp(-0.5 * power);
}

/// Display encode for PNG/pigment: keep [0,1] linear, soft-knee HDR above 1.
inline Vector3d TonemapSplatRgb(const Vector3d& c)
{
    return Vector3d(
        (c[X] <= 1.0) ? Clamp01(c[X]) : (1.0 - 1.0 / (1.0 + c[X])),
        (c[Y] <= 1.0) ? Clamp01(c[Y]) : (1.0 - 1.0 / (1.0 + c[Y])),
        (c[Z] <= 1.0) ? Clamp01(c[Z]) : (1.0 - 1.0 / (1.0 + c[Z])));
}

inline DBL SrgbToLinear(DBL c)
{
    c = Clamp01(c);
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

void QuatToMatrix(DBL w, DBL x, DBL y, DBL z, MATRIX out)
{
    DBL n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n < EPSILON)
    {
        MIdentity(out);
        return;
    }
    w /= n; x /= n; y /= n; z /= n;
    MIdentity(out);
    out[0][0] = 1 - 2 * (y * y + z * z);
    out[0][1] = 2 * (x * y - z * w);
    out[0][2] = 2 * (x * z + y * w);
    out[1][0] = 2 * (x * y + z * w);
    out[1][1] = 1 - 2 * (x * x + z * z);
    out[1][2] = 2 * (y * z - x * w);
    out[2][0] = 2 * (x * z - y * w);
    out[2][1] = 2 * (y * z + x * w);
    out[2][2] = 1 - 2 * (x * x + y * y);
}

/// Build Sigma = R * diag(s^2) * R^T in object space (KHR C C^T with C = R S).
void BuildSigmaObject(const GaussianSplatPoint& sp, DBL Sigma[3][3], bool preserveZeroScale)
{
    MATRIX Rmat;
    QuatToMatrix(sp.qw, sp.qx, sp.qy, sp.qz, Rmat);
    const DBL minScale = preserveZeroScale ? 0.0 : EPSILON;
    const DBL sx = std::max(sp.scale[X], minScale);
    const DBL sy = std::max(sp.scale[Y], minScale);
    const DBL sz = std::max(sp.scale[Z], minScale);
    const DBL s2x = sx * sx, s2y = sy * sy, s2z = sz * sz;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            Sigma[i][j] = Rmat[i][0] * s2x * Rmat[j][0]
                        + Rmat[i][1] * s2y * Rmat[j][1]
                        + Rmat[i][2] * s2z * Rmat[j][2];
        }
    }
}

/// Sigma_world = M * Sigma * M^T using the same linear map as MTransDirection
/// (KHR Sigma = M C C^T M^T with M = node upper-left 3x3).
void TransformCovariance(DBL Sigma[3][3], const TRANSFORM *Trans)
{
    if (Trans == nullptr)
        return;

    // M3[i][j]: (M v)[i] = sum_j M3[i][j] v[j]  (matches MTransDirection).
    DBL M3[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            M3[i][j] = Trans->matrix[j][i];

    DBL tmp[3][3];
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            DBL s = 0.0;
            for (int k = 0; k < 3; ++k)
                s += Sigma[i][k] * M3[j][k]; // Sigma * M^T
            tmp[i][j] = s;
        }
    }
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            DBL s = 0.0;
            for (int k = 0; k < 3; ++k)
                s += M3[i][k] * tmp[k][j]; // M * tmp
            Sigma[i][j] = s;
        }
    }
}

Vector3d SplatNormalObject(const GaussianSplatPoint& sp, const TRANSFORM *sourceTransform)
{
    MATRIX Rmat;
    QuatToMatrix(sp.qw, sp.qx, sp.qy, sp.qz, Rmat);
    int axis = 0;
    if (sp.scale[Y] < sp.scale[axis])
        axis = 1;
    if (sp.scale[Z] < sp.scale[axis])
        axis = 2;

    Vector3d normal(Rmat[0][axis], Rmat[1][axis], Rmat[2][axis]);
    if (sourceTransform != nullptr)
        MTransNormal(normal, normal, sourceTransform);
    if (normal.lengthSqr() > EPSILON)
        normal.normalize();
    return normal;
}

void InverseRotationDirection(Vector3d& result, const Vector3d& direction, const TRANSFORM *transform)
{
    if (transform == nullptr)
    {
        result = direction;
        return;
    }

    Vector3d axisX(transform->matrix[0][0], transform->matrix[0][1], transform->matrix[0][2]);
    Vector3d axisY(transform->matrix[1][0], transform->matrix[1][1], transform->matrix[1][2]);
    Vector3d axisZ(transform->matrix[2][0], transform->matrix[2][1], transform->matrix[2][2]);
    if (axisX.lengthSqr() > EPSILON) axisX.normalize();
    if (axisY.lengthSqr() > EPSILON) axisY.normalize();
    if (axisZ.lengthSqr() > EPSILON) axisZ.normalize();
    result = Vector3d(dot(direction, axisX), dot(direction, axisY), dot(direction, axisZ));
}

void SplatWorldAABB(const GaussianSplatPoint& sp, const TRANSFORM *sourceTransform,
                    DBL sigmaExtent, Vector3d& bmin, Vector3d& bmax)
{
    Vector3d center = sp.position;
    if (sourceTransform != nullptr)
        MTransPoint(center, sp.position, sourceTransform);

    DBL sigma[3][3];
    BuildSigmaObject(sp, sigma, true);
    TransformCovariance(sigma, sourceTransform);
    const Vector3d ext(
        sigmaExtent * std::sqrt(std::max(0.0, sigma[0][0])),
        sigmaExtent * std::sqrt(std::max(0.0, sigma[1][1])),
        sigmaExtent * std::sqrt(std::max(0.0, sigma[2][2])));
    bmin = center - ext;
    bmax = center + ext;
}

TEXTURE *EnsureThreadSplatTexture(TraceThreadData *Thread)
{
    if (Thread->GaussianSplatTexture != nullptr)
        return Thread->GaussianSplatTexture;

    TEXTURE *tex = Create_Texture();
    Destroy_Pigment(tex->Pigment);
    tex->Pigment = Create_Pigment();
    if (tex->Finish == nullptr)
        tex->Finish = Create_Finish();
    tex->Finish->Ambient = MathColour(0.0);
    tex->Finish->Diffuse = 0.0f;
    tex->Finish->DiffuseBack = 0.0f;
    tex->Finish->Specular = 0.0f;
    tex->Finish->Phong = 0.0f;
    tex->Finish->Emission = MathColour(1.0);
    Post_Textures(tex);
    Thread->GaussianSplatTexture = tex;
    return tex;
}

} // namespace

GaussianSplatCloud::GaussianSplatCloud() :
    NonsolidObject(GAUSSIAN_SPLAT_OBJECT),
    restCount(0),
    restCoeffsInterleaved(false),
    shDegree(3),
    opacityCutoff(1.0 / 255.0),
    alphaStop(0.999),
    alphaClamp(0.99),
    sigmaExtent(3.0),
    ewaFilter(0.3),
    maxQuadFrac(0.0),
    samples(4),
    maxHits(0),
    bvhLeafSize(1),
    giWeight(1.0),
    opacityScale(1.0),
    lightingMode(GaussianSplatLightingMode::Automatic),
    materialShading(false),
    cullSingletonHits(true),
    khrEllipse(false),
    khrSortCameraDistance(true),
    renderBackend(GaussianSplatRenderBackend::Cpu),
    asSurface(false),
    inverseSurface(false),
    surfaceSmooth(false),
    filterFloaters(true),
    surfaceVoxelSize(0.0),
    surfaceVoxelOpacity(0.1),
    surfaceMinOpacity(1.0 / 255.0),
    floaterVoxelSize(0.05),
    floaterVoxelOpacity(0.1),
    floaterMinOpacity(0.004),
    vkFrame(nullptr)
{
    Type |= PATCH_OBJECT | TEXTURED_OBJECT;
    Set_Flag(this, HOLLOW_FLAG);
    Set_Flag(this, PH_IGNORE_PHOTONS_FLAG);
    // Required so Trace::ComputeTextureColour calls Determine_Textures (per-hit emission colour).
    Set_Flag(this, MULTITEXTURE_FLAG);
}

GaussianSplatCloud::~GaussianSplatCloud()
{
}

ObjectPtr GaussianSplatCloud::Copy()
{
    GaussianSplatCloud *New = new GaussianSplatCloud();
    Destroy_Transform(New->Trans);
    New->points = points;
    New->sourceTransforms = sourceTransforms;
    New->restCoeffs = restCoeffs;
    New->restCount = restCount;
    New->restCoeffsInterleaved = restCoeffsInterleaved;
    New->shDegree = shDegree;
    New->opacityCutoff = opacityCutoff;
    New->alphaStop = alphaStop;
    New->alphaClamp = alphaClamp;
    New->sigmaExtent = sigmaExtent;
    New->ewaFilter = ewaFilter;
    New->maxQuadFrac = maxQuadFrac;
    New->samples = samples;
    New->maxHits = maxHits;
    New->bvhLeafSize = bvhLeafSize;
    New->giWeight = giWeight;
    New->opacityScale = opacityScale;
    New->lightingMode = lightingMode;
    New->materialShading = materialShading;
    New->cullSingletonHits = cullSingletonHits;
    New->khrEllipse = khrEllipse;
    New->khrKernel = khrKernel;
    New->khrColorSpace = khrColorSpace;
    New->khrProjection = khrProjection;
    New->khrSortingMethod = khrSortingMethod;
    New->khrSortCameraDistance = khrSortCameraDistance;
    New->renderBackend = renderBackend;
    New->asSurface = asSurface;
    New->inverseSurface = inverseSurface;
    New->exportMeshPath = exportMeshPath;
    New->surfaceSmooth = surfaceSmooth;
    New->filterFloaters = filterFloaters;
    New->surfaceVoxelSize = surfaceVoxelSize;
    New->surfaceVoxelOpacity = surfaceVoxelOpacity;
    New->surfaceMinOpacity = surfaceMinOpacity;
    New->floaterVoxelSize = floaterVoxelSize;
    New->floaterVoxelOpacity = floaterVoxelOpacity;
    New->floaterMinOpacity = floaterMinOpacity;
    New->bvh = bvh;
    New->bvhOrder = bvhOrder;
    New->raySplatData = raySplatData;
    New->Trans = Copy_Transform(Trans);
    New->Texture = Copy_Textures(Texture);
    New->Interior_Texture = Copy_Textures(Interior_Texture);
    New->interior = interior;
    New->Type = Type;
    New->Flags = Flags;
    New->BBox = BBox;
    return New;
}

bool GaussianSplatCloud::IsOpaque() const
{
    return false;
}

bool GaussianSplatCloud::WantsMaterialShading() const
{
    if (lightingMode == GaussianSplatLightingMode::Relight)
        return true;
    if (lightingMode == GaussianSplatLightingMode::Khr)
        return false;
    if (materialShading)
        return true;
    if (interior != nullptr && interior->IOR > SNGL(1.0) + EPSILON)
        return true;
    if (Texture != nullptr && Texture->Finish != nullptr)
    {
        const FINISH *f = Texture->Finish;
        if (f->Diffuse > EPSILON || f->DiffuseBack > EPSILON ||
            f->Specular > EPSILON || f->Phong > EPSILON)
            return true;
    }
    return false;
}

bool GaussianSplatCloud::Inside(const Vector3d&, TraceThreadData *) const
{
    return false;
}

void GaussianSplatCloud::Normal(Vector3d& Result, Intersection *Inter, TraceThreadData *) const
{
    // Face the incoming ray approximately (emission billboard).
    if (Inter != nullptr && Inter->haveNormal)
        Result = Inter->INormal;
    else
        Result = Vector3d(0.0, 0.0, 1.0);
}

void GaussianSplatCloud::Translate(const Vector3d& Vector, const TRANSFORM *tr)
{
    Transform(tr);
}

void GaussianSplatCloud::Rotate(const Vector3d&, const TRANSFORM *tr)
{
    Transform(tr);
}

void GaussianSplatCloud::Scale(const Vector3d&, const TRANSFORM *tr)
{
    Transform(tr);
}

void GaussianSplatCloud::Transform(const TRANSFORM *tr)
{
    if (tr == nullptr)
        return;
    if (Trans == nullptr)
        Trans = Create_Transform();
    Compose_Transforms(Trans, tr);
    {
        std::lock_guard<std::mutex> lock(projCacheMutex);
        projCache.valid = false;
        projCache.entries.clear();
    }
    {
        std::lock_guard<std::mutex> lock(vkFrameMutex);
        vkFrame.reset();
    }
    Compute_BBox();
}

void GaussianSplatCloud::Compute_BBox()
{
    if (points.empty())
    {
        Make_BBox(BBox, 0, 0, 0, 0, 0, 0);
        return;
    }

    BuildAcceleration();

    Vector3d bmin = bvh[0].bmin;
    Vector3d bmax = bvh[0].bmax;
    if (Trans != nullptr)
    {
        Recompute_BBox(&BBox, Trans);
        // Recompute from transformed corners of root AABB.
        Make_BBox(BBox, bmin[X], bmin[Y], bmin[Z],
                  bmax[X] - bmin[X], bmax[Y] - bmin[Y], bmax[Z] - bmin[Z]);
        Recompute_BBox(&BBox, Trans);
    }
    else
    {
        Make_BBox(BBox, bmin[X], bmin[Y], bmin[Z],
                  bmax[X] - bmin[X], bmax[Y] - bmin[Y], bmax[Z] - bmin[Z]);
    }
}

void GaussianSplatCloud::BuildAcceleration()
{
    std::lock_guard<std::mutex> lock(accelMutex);
    if (!bvh.empty())
        return;

    bvhOrder.resize(points.size());
    raySplatData.resize(points.size());
    for (size_t i = 0; i < points.size(); ++i)
    {
        bvhOrder[i] = static_cast<int>(i);
        BuildSigmaObject(points[i], raySplatData[i].covariance, false);
        const TRANSFORM* sourceTransform =
            (points[i].transformIndex < sourceTransforms.size()) ? &sourceTransforms[points[i].transformIndex] : nullptr;
        raySplatData[i].normal = SplatNormalObject(points[i], sourceTransform);
    }
    if (points.empty())
        return;
    bvh.reserve(points.size() * 2);
    bvh.push_back(GaussianSplatBVHNode());
    BuildBVHRecursive(0, 0, static_cast<int>(points.size()), 0);
}

void GaussianSplatCloud::BuildBVHRecursive(int nodeIndex, int begin, int end, int depth)
{
    GaussianSplatBVHNode& node = bvh[static_cast<size_t>(nodeIndex)];
    Vector3d bmin(BOUND_HUGE, BOUND_HUGE, BOUND_HUGE);
    Vector3d bmax(-BOUND_HUGE, -BOUND_HUGE, -BOUND_HUGE);
    for (int i = begin; i < end; ++i)
    {
        Vector3d smin, smax;
        const GaussianSplatPoint& point = points[static_cast<size_t>(bvhOrder[static_cast<size_t>(i)])];
        const TRANSFORM *sourceTransform =
            (point.transformIndex < sourceTransforms.size()) ? &sourceTransforms[point.transformIndex] : nullptr;
        SplatWorldAABB(point, sourceTransform, sigmaExtent, smin, smax);
        bmin = Vector3d(std::min(bmin[X], smin[X]), std::min(bmin[Y], smin[Y]), std::min(bmin[Z], smin[Z]));
        bmax = Vector3d(std::max(bmax[X], smax[X]), std::max(bmax[Y], smax[Y]), std::max(bmax[Z], smax[Z]));
    }
    node.bmin = bmin;
    node.bmax = bmax;

    const int count = end - begin;
    const int leafSize = std::max(1, bvhLeafSize);
    if (count <= leafSize || depth > 64)
    {
        node.left = -1;
        node.right = -1;
        node.first = begin;
        node.count = count;
        return;
    }

    Vector3d ext = bmax - bmin;
    int axis = X;
    if (ext[Y] > ext[axis]) axis = Y;
    if (ext[Z] > ext[axis]) axis = Z;

    const int mid = begin + count / 2;
    std::nth_element(bvhOrder.begin() + begin, bvhOrder.begin() + mid, bvhOrder.begin() + end,
                     [&](int a, int b) {
                         return points[static_cast<size_t>(a)].position[axis] < points[static_cast<size_t>(b)].position[axis];
                     });

    const int leftIndex = static_cast<int>(bvh.size());
    bvh.push_back(GaussianSplatBVHNode());
    const int rightIndex = static_cast<int>(bvh.size());
    bvh.push_back(GaussianSplatBVHNode());
    // Re-fetch node after potential reallocation.
    bvh[static_cast<size_t>(nodeIndex)].left = leftIndex;
    bvh[static_cast<size_t>(nodeIndex)].right = rightIndex;
    bvh[static_cast<size_t>(nodeIndex)].first = 0;
    bvh[static_cast<size_t>(nodeIndex)].count = 0;

    BuildBVHRecursive(leftIndex, begin, mid, depth + 1);
    BuildBVHRecursive(rightIndex, mid, end, depth + 1);
}

bool GaussianSplatCloud::RayAABB(const Vector3d& origin, const Vector3d& invDir,
                                 const Vector3d& bmin, const Vector3d& bmax,
                                 DBL& tNear, DBL& tFar) const
{
    tNear = -BOUND_HUGE;
    tFar = BOUND_HUGE;
    for (int a = X; a <= Z; ++a)
    {
        DBL t0 = (bmin[a] - origin[a]) * invDir[a];
        DBL t1 = (bmax[a] - origin[a]) * invDir[a];
        if (t0 > t1)
            std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar)
            return false;
    }
    return tFar >= kDepthTolerance;
}

bool GaussianSplatCloud::SplatContribution(size_t pointIndex,
                                           const Vector3d& origin, const Vector3d& dir,
                                           DBL tSeg0, DBL tSeg1, DBL& tHit, DBL& weight,
                                           TraceThreadData *Thread, bool useKerbl, const Vector3d planeAxes[2]) const
{
    const GaussianSplatPoint& sp = points[pointIndex];
    const DBL opac = Clamp01(sp.opacity * opacityScale);
    if (opac < opacityCutoff)
        return false;

    MATRIX Rmat;
    if (samples != 2)
        QuatToMatrix(sp.qw, sp.qx, sp.qy, sp.qz, Rmat);

    const DBL sx = std::max(sp.scale[X], EPSILON);
    const DBL sy = std::max(sp.scale[Y], EPSILON);
    const DBL sz = std::max(sp.scale[Z], EPSILON);

    // Ray in Mahalanobis / unit-ellipsoid local space: x = o + t d → oL + t dL.
    auto toLocal = [&](const Vector3d& w, Vector3d& out) {
        out = Vector3d(
            Rmat[0][0] * w[X] + Rmat[1][0] * w[Y] + Rmat[2][0] * w[Z],
            Rmat[0][1] * w[X] + Rmat[1][1] * w[Y] + Rmat[2][1] * w[Z],
            Rmat[0][2] * w[X] + Rmat[1][2] * w[Y] + Rmat[2][2] * w[Z]);
        out[X] /= sx; out[Y] /= sy; out[Z] /= sz;
    };

    // --- samples <= 1: legacy 3D Mahalanobis peak (fast, underestimates opacity) ---
    if (samples <= 1)
    {
        Vector3d oL, dL;
        toLocal(origin - sp.position, oL);
        toLocal(dir, dL);
        const DBL dd = dL.lengthSqr();
        if (dd < EPSILON)
            return false;
        tHit = -dot(oL, dL) / dd;
        if (tHit < tSeg0 || tHit > tSeg1)
            return false;
        const Vector3d closest = oL + dL * tHit;
        const DBL g = std::exp(-0.5 * closest.lengthSqr());
        weight = Clamp01(opac * g);
        return weight >= opacityCutoff;
    }

    // --- samples == 2: Kerbl Jacobian EWA (primary) or plane-perp billboard fallback ---
    // khrEllipse secondary: never use primary-camera J (caller forces useKerbl=false).
    if (samples == 2)
    {
        const TraceThreadData::GaussianSplatProj *proj =
            (useKerbl && !khrEllipse && Thread != nullptr && Thread->GaussianSplatCam.camValid &&
             Thread->GaussianSplatCam.pixelValid) ? &Thread->GaussianSplatCam : nullptr;

        if (proj != nullptr)
        {
            // Camera space: X=right, Y=up, Z=forward.
            const Vector3d rel = sp.position - proj->origin;
            const DBL tx = dot(rel, proj->right);
            const DBL ty = dot(rel, proj->up);
            const DBL tz = dot(rel, proj->forward);
            if (tz <= EPSILON)
                return false;

            tHit = dot(sp.position - origin, dir);
            if (tHit < tSeg0 || tHit > tSeg1)
                return false;

            const auto& SigmaW = raySplatData[pointIndex].covariance;

            // W rows = camera axes → Σ_cam = W Σ_world Wᵀ
            const Vector3d axes[3] = { proj->right, proj->up, proj->forward };
            DBL SigmaC[3][3];
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    DBL s = 0.0;
                    for (int a = 0; a < 3; ++a)
                    {
                        DBL WaS = 0.0;
                        for (int b = 0; b < 3; ++b)
                            WaS += axes[i][b] * SigmaW[b][a];
                        s += WaS * axes[j][a];
                    }
                    SigmaC[i][j] = s;
                }
            }

            // J (2×3): match SuperSplat / PlayCanvas projector (positive perspective terms).
            const DBL fx = proj->fx, fy = proj->fy;
            const DBL invZ = 1.0 / tz;
            const DBL invZ2 = invZ * invZ;
            const DBL J00 = fx * invZ, J02 = -fx * tx * invZ2;
            const DBL J11 = fy * invZ, J12 = -fy * ty * invZ2;

            // cov2d = J * Σ_cam * Jᵀ  (2x2). Kerbl +0.3px low-pass is NOT in KHR.
            auto Sj = [&](int row, int colJ) -> DBL {
                // (Σ_cam * Jᵀ)[row][colJ]
                if (colJ == 0)
                    return SigmaC[row][0] * J00 + SigmaC[row][2] * J02;
                return SigmaC[row][1] * J11 + SigmaC[row][2] * J12;
            };
            DBL cov00 = J00 * Sj(0, 0) + J02 * Sj(2, 0);
            DBL cov01 = J00 * Sj(0, 1) + J02 * Sj(2, 1);
            DBL cov11 = J11 * Sj(1, 1) + J12 * Sj(2, 1);
            if (!khrEllipse)
            {
                cov00 += ewaFilter;
                cov11 += ewaFilter;
            }

            const DBL det = cov00 * cov11 - cov01 * cov01;
            if (det < EPSILON)
                return false;

            const DBL meanU = fx * tx * invZ;
            const DBL meanV = fy * ty * invZ;
            const DBL du = proj->screenU - meanU;
            const DBL dv = proj->screenV - meanV;
            const DBL power = (cov11 * du * du - 2.0 * cov01 * du * dv + cov00 * dv * dv) / det;
            const DBL powerCull = khrEllipse ? (sigmaExtent * sigmaExtent) : kProjPowerCullSuperSplat;
            if (power > powerCull)
                return false;

            weight = Clamp01(opac * std::exp(-0.5 * power));
            return weight >= opacityCutoff;
        }

        // Billboard fallback (KHR secondary policy): Σ projected into plane ⟂ ray (no perspective J).
        const Vector3d om = sp.position - origin;
        tHit = dot(om, dir);
        if (tHit < tSeg0 || tHit > tSeg1)
            return false;

        const Vector3d mid = origin + dir * tHit;
        const Vector3d delta = sp.position - mid;

        const auto& Sigma = raySplatData[pointIndex].covariance;
        const Vector3d& u = planeAxes[0];
        const Vector3d& v = planeAxes[1];

        auto quadForm = [&](const Vector3d& e1, const Vector3d& e2) -> DBL {
            const Vector3d Se2(
                Sigma[0][0] * e2[X] + Sigma[0][1] * e2[Y] + Sigma[0][2] * e2[Z],
                Sigma[1][0] * e2[X] + Sigma[1][1] * e2[Y] + Sigma[1][2] * e2[Z],
                Sigma[2][0] * e2[X] + Sigma[2][1] * e2[Y] + Sigma[2][2] * e2[Z]);
            return dot(e1, Se2);
        };

        const DBL eps2 = 1.0e-12;
        const DBL a00 = quadForm(u, u) + eps2;
        const DBL a01 = quadForm(u, v);
        const DBL a11 = quadForm(v, v) + eps2;
        const DBL det = a00 * a11 - a01 * a01;
        if (det < EPSILON)
            return false;

        const DBL du = dot(delta, u);
        const DBL dv = dot(delta, v);
        const DBL power = (a11 * du * du - 2.0 * a01 * du * dv + a00 * dv * dv) / det;
        const DBL powerCull = khrEllipse ? (sigmaExtent * sigmaExtent) : kProjPowerCullSuperSplat;
        if (power > powerCull)
            return false;

        weight = Clamp01(opac * std::exp(-0.5 * power));
        return weight >= opacityCutoff;
    }

    // --- samples >= 3: Vol3DGS analytic volume α (Eq. 19) ---
    // α = 1 − exp(−κ · G_peak · √(2π) · β), with erf segment clip (Eq. 18).
    // Density κ uses Vol3DGS Eq. 20 style reparam so small 3DGS-trained scales
    // stay opaque (raw opacity as κ underestimates when β ≪ 1).
    {
        Vector3d oL, dL;
        toLocal(origin - sp.position, oL);
        toLocal(dir, dL);
        const DBL a = dL.lengthSqr(); // d^T Σ^{-1} d
        if (a < EPSILON)
            return false;

        tHit = -dot(oL, dL) / a; // γ
        const DBL beta = 1.0 / std::sqrt(a);
        if (tHit < (tSeg0 - sigmaExtent * beta) || tHit > (tSeg1 + sigmaExtent * beta))
            return false;

        const Vector3d closest = oL + dL * tHit;
        const DBL gPeak = std::exp(-0.5 * closest.lengthSqr());
        if (gPeak < kWeightEps)
            return false;

        const DBL invSigma = 1.0 / (beta * std::sqrt(2.0));
        const DBL tLo = std::max(tSeg0, tHit - sigmaExtent * beta);
        const DBL tHi = std::min(tSeg1, tHit + sigmaExtent * beta);
        if (tHi <= tLo)
            return false;
        const DBL erfHi = std::erf((tHi - tHit) * invSigma);
        const DBL erfLo = std::erf((tLo - tHit) * invSigma);
        const DBL integralG = gPeak * beta * kSqrtTwoPi * 0.5 * (erfHi - erfLo);
        if (integralG <= 0.0)
            return false;

        // κ = −log(1 − 0.99 θ) · (1/3)(1/sx+1/sy+1/sz), θ := sigmoid opacity.
        const DBL theta = std::min(opac, 0.999);
        const DBL invScaleMean = (1.0 / sx + 1.0 / sy + 1.0 / sz) / 3.0;
        const DBL kappa = -std::log(1.0 - 0.99 * theta) * invScaleMean;
        weight = Clamp01(1.0 - std::exp(-kappa * integralG));
        return weight >= opacityCutoff;
    }
}

void GaussianSplatCloud::ViewDirForSH(const GaussianSplatPoint& sp,
                                      const Vector3d& camToSplatWorld, Vector3d& outLocal) const
{
    // KHR: SH coefficients SHOULD be transformed by the node rotation. Equivalent:
    // evaluate SH with R^{-1} * viewDir (authoring / pre-Trans frame).
    if (Trans != nullptr)
    {
        InverseRotationDirection(outLocal, camToSplatWorld, Trans);
    }
    else
    {
        outLocal = camToSplatWorld;
    }
    if (sp.transformIndex < sourceTransforms.size())
        InverseRotationDirection(outLocal, outLocal, &sourceTransforms[sp.transformIndex]);
    if (outLocal.lengthSqr() > EPSILON)
        outLocal.normalize();
}

void GaussianSplatCloud::EvalColour(const GaussianSplatPoint& sp, const Vector3d& viewDir, Vector3d& rgb) const
{
    // viewDir = camera → splat in the SH authoring frame (see ViewDirForSH).
    Vector3d dir = viewDir.normalized();
    const DBL x = dir[X], y = dir[Y], z = dir[Z];

    rgb = Vector3d(
        0.5 + kShC0 * sp.dc[X],
        0.5 + kShC0 * sp.dc[Y],
        0.5 + kShC0 * sp.dc[Z]);

    const int deg = std::min(shDegree, 3);
    if (deg < 1 || restCount < 9 || restCoeffs.empty())
    {
        rgb = Vector3d(std::max(0.0, rgb[X]), std::max(0.0, rgb[Y]), std::max(0.0, rgb[Z]));
        return;
    }

    const DBL *rest = &restCoeffs[sp.restOffset];
    // KHR sidecars are coefficient-major RGB; legacy graphdeco PLY sidecars are
    // channel-major (all R bands, then G, then B).
    const unsigned numBands = restCount / 3;
    auto band = [&](int b, int c) -> DBL {
        if (b < 0 || static_cast<unsigned>(b) >= numBands || c < 0 || c > 2)
            return 0.0;
        const unsigned idx = restCoeffsInterleaved
            ? static_cast<unsigned>(b) * 3u + static_cast<unsigned>(c)
            : static_cast<unsigned>(c) * numBands + static_cast<unsigned>(b);
        return (idx < restCount) ? rest[idx] : 0.0;
    };

    // Degree 1 (3 bands × RGB).
    rgb[X] += -kShC1 * y * band(0, 0) + kShC1 * z * band(1, 0) - kShC1 * x * band(2, 0);
    rgb[Y] += -kShC1 * y * band(0, 1) + kShC1 * z * band(1, 1) - kShC1 * x * band(2, 1);
    rgb[Z] += -kShC1 * y * band(0, 2) + kShC1 * z * band(1, 2) - kShC1 * x * band(2, 2);

    if (deg >= 2 && restCount >= 24)
    {
        const DBL xx = x * x, yy = y * y, zz = z * z;
        const DBL xy = x * y, yz = y * z, xz = x * z;
        auto add2 = [&](int base, DBL w) {
            rgb[X] += w * band(base, 0);
            rgb[Y] += w * band(base, 1);
            rgb[Z] += w * band(base, 2);
        };
        add2(3, kShC2[0] * xy);
        add2(4, kShC2[1] * yz);
        add2(5, kShC2[2] * (2.0 * zz - xx - yy));
        add2(6, kShC2[3] * xz);
        add2(7, kShC2[4] * (xx - yy));
    }

    if (deg >= 3 && restCount >= 45)
    {
        const DBL xx = x * x, yy = y * y, zz = z * z;
        const DBL xy = x * y, yz = y * z, xz = x * z;
        auto add3 = [&](int base, DBL w) {
            rgb[X] += w * band(base, 0);
            rgb[Y] += w * band(base, 1);
            rgb[Z] += w * band(base, 2);
        };
        add3(8, kShC3[0] * y * (3.0 * xx - yy));
        add3(9, kShC3[1] * xy * z);
        add3(10, kShC3[2] * y * (4.0 * zz - xx - yy));
        add3(11, kShC3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy));
        add3(12, kShC3[4] * x * (4.0 * zz - xx - yy));
        add3(13, kShC3[5] * z * (xx - yy));
        add3(14, kShC3[6] * x * (xx - 3.0 * yy));
    }

    // Keep HDR for compositing (SuperSplat allows up to ~8); only floor negatives.
    rgb = Vector3d(std::max(0.0, rgb[X]), std::max(0.0, rgb[Y]), std::max(0.0, rgb[Z]));
}

void GaussianSplatCloud::EvalDiffuseColour(const GaussianSplatPoint& sp, Vector3d& rgb) const
{
    rgb = Vector3d(
        std::max(0.0, 0.5 + kShC0 * sp.dc[X]),
        std::max(0.0, 0.5 + kShC0 * sp.dc[Y]),
        std::max(0.0, 0.5 + kShC0 * sp.dc[Z]));
}

bool GaussianSplatCloud::IntegrateAlongRay(const Vector3d& origin, const Vector3d& dir,
                                           DBL t0, DBL t1, const Vector3d& viewDir,
                                           GaussianSplatSegmentResult& out, TraceThreadData *Thread,
                                           bool useKerbl) const
{
    out.colour = Vector3d(0.0, 0.0, 0.0);
    out.normal = Vector3d(0.0, 0.0, 0.0);
    out.transmittance = 1.0;
    out.depth = 0.0;
    out.valid = false;

    if (points.empty() || Thread == nullptr || t1 <= t0)
        return false;

    // BuildAcceleration is non-const; callers ensure BVH exists before render.
    if (bvh.empty())
        return false;

    Vector3d invDir(
        (std::fabs(dir[X]) > EPSILON) ? (1.0 / dir[X]) : BOUND_HUGE,
        (std::fabs(dir[Y]) > EPSILON) ? (1.0 / dir[Y]) : BOUND_HUGE,
        (std::fabs(dir[Z]) > EPSILON) ? (1.0 / dir[Z]) : BOUND_HUGE);

    DBL rootNear = 0, rootFar = 0;
    if (!RayAABB(origin, invDir, bvh[0].bmin, bvh[0].bmax, rootNear, rootFar))
        return false;

    const DBL seg0 = std::max(t0, rootNear);
    const DBL seg1 = std::min(t1, rootFar);
    if (seg1 <= seg0)
        return false;

    Vector3d planeAxes[2];
    if (samples == 2)
    {
        const Vector3d axis = (std::fabs(dir[Z]) < 0.9) ? Vector3d(0.0, 0.0, 1.0) : Vector3d(0.0, 1.0, 0.0);
        planeAxes[0] = cross(dir, axis);
        const DBL length = planeAxes[0].length();
        if (length < EPSILON)
            return false;
        planeAxes[0] /= length;
        planeAxes[1] = cross(dir, planeAxes[0]);
    }

    const int hitCap = std::min(kHardMaxHits, (maxHits > 0) ? maxHits : kDefaultMaxHits);
    std::vector<TraceThreadData::GaussianSplatHitRec>& hits = Thread->GaussianSplatHits;
    hits.clear();
    if (static_cast<int>(hits.capacity()) < hitCap)
        hits.reserve(static_cast<size_t>(std::min(hitCap, 4096)));

    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0 && static_cast<int>(hits.size()) < hitCap)
    {
        const int ni = stack[--sp];
        const GaussianSplatBVHNode& node = bvh[static_cast<size_t>(ni)];
        if (node.left < 0)
        {
            for (int i = 0; i < node.count && static_cast<int>(hits.size()) < hitCap; ++i)
            {
                const int pi = bvhOrder[static_cast<size_t>(node.first + i)];
                DBL tHit = 0, w = 0;
                if (SplatContribution(static_cast<size_t>(pi), origin, dir, seg0, seg1, tHit, w,
                                      Thread, useKerbl, planeAxes))
                {
                    TraceThreadData::GaussianSplatHitRec rec;
                    rec.t = tHit;
                    rec.w = w;
                    rec.idx = pi;
                    hits.push_back(rec);
                }
            }
        }
        else
        {
            DBL l0, l1, r0, r1;
            const bool hitL = RayAABB(origin, invDir, bvh[static_cast<size_t>(node.left)].bmin,
                                      bvh[static_cast<size_t>(node.left)].bmax, l0, l1) &&
                              l1 >= seg0 && l0 <= seg1;
            const bool hitR = RayAABB(origin, invDir, bvh[static_cast<size_t>(node.right)].bmin,
                                      bvh[static_cast<size_t>(node.right)].bmax, r0, r1) &&
                              r1 >= seg0 && r0 <= seg1;
            if (hitL && hitR)
            {
                if (l0 > r0)
                {
                    if (sp < 63) stack[sp++] = node.left;
                    if (sp < 63) stack[sp++] = node.right;
                }
                else
                {
                    if (sp < 63) stack[sp++] = node.right;
                    if (sp < 63) stack[sp++] = node.left;
                }
            }
            else if (hitL && sp < 63)
                stack[sp++] = node.left;
            else if (hitR && sp < 63)
                stack[sp++] = node.right;
        }
    }

    if (hits.empty())
        return false;

    // Singleton view-ray cull: one splat alone is usually a soft edge / floater.
    if (cullSingletonHits && hits.size() < 2)
        return false;

    std::sort(hits.begin(), hits.end(),
              [](const TraceThreadData::GaussianSplatHitRec& a, const TraceThreadData::GaussianSplatHitRec& b) {
                  return a.t < b.t;
              });

    Vector3d acc(0.0, 0.0, 0.0);
    DBL T = 1.0;
    DBL depthSum = 0.0;
    DBL depthW = 0.0;
    Vector3d normalSum(0.0, 0.0, 0.0);

    for (size_t i = 0; i < hits.size() && T > (1.0 - alphaStop); ++i)
    {
        // SplatContribution already returns final per-splat α (peak/EWA/volume).
        const DBL alpha = hits[i].w;
        if (alpha < opacityCutoff)
            continue;
        const GaussianSplatPoint& spoint = points[static_cast<size_t>(hits[i].idx)];
        Vector3d rgb;
        if (lightingMode == GaussianSplatLightingMode::Relight)
            EvalDiffuseColour(spoint, rgb);
        else
        {
            // SH expects camera→splat in authoring frame; viewDir arg is toward camera.
            Vector3d camToSplat = spoint.position - origin;
            if (camToSplat.lengthSqr() > EPSILON)
                camToSplat.normalize();
            else
                camToSplat = -viewDir;
            Vector3d shDir;
            ViewDirForSH(spoint, camToSplat, shDir);
            EvalColour(spoint, shDir, rgb);
        }
        acc += rgb * (alpha * T);
        const DBL contrib = alpha * T;
        if (lightingMode == GaussianSplatLightingMode::Relight)
        {
            Vector3d normal = raySplatData[static_cast<size_t>(hits[i].idx)].normal;
            if (dot(normal, viewDir) < 0.0)
                normal = -normal;
            normalSum += normal * contrib;
        }
        depthSum += hits[i].t * contrib;
        depthW += contrib;
        T *= (1.0 - alpha);
    }

    const DBL alphaOut = Clamp01(1.0 - T);
    if (alphaOut < opacityCutoff)
        return false;

    out.colour = acc;
    out.normal = normalSum;
    if (out.normal.lengthSqr() > EPSILON)
        out.normal.normalize();
    out.transmittance = T;
    out.depth = (depthW > EPSILON) ? (depthSum / depthW) : hits[0].t;
    out.valid = true;
    return true;
}

std::shared_ptr<const GaussianSplatVkFramebuffer> GaussianSplatCloud::PrepareVulkanFramebuffer(const GaussianSplatCamBasis& cam) const
{
    if (!cam.camValid || renderBackend != GaussianSplatRenderBackend::Vulkan)
        return nullptr;
    if (cam.screenW <= EPSILON || cam.screenH <= EPSILON)
        return nullptr;

    std::lock_guard<std::mutex> lock(vkFrameMutex);
    auto sameVec = [](const Vector3d& a, const Vector3d& b) {
        return std::fabs(a[X] - b[X]) < 1.0e-9 &&
               std::fabs(a[Y] - b[Y]) < 1.0e-9 &&
               std::fabs(a[Z] - b[Z]) < 1.0e-9;
    };
    if (vkFrame != nullptr &&
        vkFrame->width == static_cast<int>(std::lround(cam.screenW)) &&
        vkFrame->height == static_cast<int>(std::lround(cam.screenH)) &&
        sameVec(vkFrame->origin, cam.origin) &&
        sameVec(vkFrame->right, cam.right) &&
        sameVec(vkFrame->up, cam.up) &&
        sameVec(vkFrame->forward, cam.forward) &&
        std::fabs(vkFrame->fx - cam.fx) < 1.0e-6 &&
        std::fabs(vkFrame->fy - cam.fy) < 1.0e-6)
    {
        return vkFrame;
    }

    auto frame = std::make_shared<GaussianSplatVkFramebuffer>();
    frame->width = static_cast<int>(std::lround(cam.screenW));
    frame->height = static_cast<int>(std::lround(cam.screenH));
    frame->origin = cam.origin;
    frame->right = cam.right;
    frame->up = cam.up;
    frame->forward = cam.forward;
    frame->fx = cam.fx;
    frame->fy = cam.fy;
    if (!GaussianSplatVkAvailable())
    {
        std::fprintf(stderr, "gaussian_splat vulkan: initialization unavailable; using CPU projection for this camera\n");
        vkFrame = frame;
        return vkFrame;
    }

    std::vector<GaussianSplatGpuSplat> gpuSplats;
    gpuSplats.reserve(points.size());
    for (size_t pi = 0; pi < points.size(); ++pi)
    {
        const GaussianSplatPoint& sp = points[pi];
        const DBL opac = Clamp01(sp.opacity * opacityScale);
        if (opac <= 0.0 || opac <= opacityCutoff)
            continue;

        Vector3d pos = sp.position;
        const TRANSFORM *sourceTransform =
            (sp.transformIndex < sourceTransforms.size()) ? &sourceTransforms[sp.transformIndex] : nullptr;
        if (sourceTransform != nullptr)
            MTransPoint(pos, pos, sourceTransform);
        if (Trans != nullptr)
            MTransPoint(pos, pos, Trans);

        DBL SigmaW[3][3];
        BuildSigmaObject(sp, SigmaW, khrEllipse);
        TransformCovariance(SigmaW, sourceTransform);
        TransformCovariance(SigmaW, Trans);

        Vector3d rgb;
        if (lightingMode == GaussianSplatLightingMode::Relight)
            EvalDiffuseColour(sp, rgb);
        else
        {
            Vector3d camToSplat = pos - cam.origin;
            if (camToSplat.lengthSqr() > EPSILON)
                camToSplat.normalize();
            Vector3d shDir;
            ViewDirForSH(sp, camToSplat, shDir);
            EvalColour(sp, shDir, rgb);
        }
        if (!khrEllipse)
            rgb = TonemapSplatRgb(rgb);

        Vector3d normal(0.0, 0.0, 0.0);
        if (lightingMode == GaussianSplatLightingMode::Relight)
        {
            normal = SplatNormalObject(sp, sourceTransform);
            if (Trans != nullptr)
                MTransNormal(normal, normal, Trans);
            if (normal.lengthSqr() > EPSILON)
                normal.normalize();
            const Vector3d rel = pos - cam.origin;
            if (dot(normal, rel) > 0.0)
                normal = -normal;
        }

        GaussianSplatGpuSplat g = {};
        g.posAlpha[0] = static_cast<float>(pos[X]);
        g.posAlpha[1] = static_cast<float>(pos[Y]);
        g.posAlpha[2] = static_cast<float>(pos[Z]);
        g.posAlpha[3] = static_cast<float>(opac);
        g.sig0[0] = static_cast<float>(SigmaW[0][0]);
        g.sig0[1] = static_cast<float>(SigmaW[0][1]);
        g.sig0[2] = static_cast<float>(SigmaW[0][2]);
        g.sig0[3] = static_cast<float>(SigmaW[1][1]);
        g.sig1[0] = static_cast<float>(SigmaW[1][2]);
        g.sig1[1] = static_cast<float>(SigmaW[2][2]);
        g.rgbNx[0] = static_cast<float>(rgb[X]);
        g.rgbNx[1] = static_cast<float>(rgb[Y]);
        g.rgbNx[2] = static_cast<float>(rgb[Z]);
        g.rgbNx[3] = static_cast<float>(normal[X]);
        g.nyNz[0] = static_cast<float>(normal[Y]);
        g.nyNz[1] = static_cast<float>(normal[Z]);
        gpuSplats.push_back(g);
    }

    if (gpuSplats.empty())
    {
        const size_t pixels = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
        frame->rgba.assign(pixels * 4, 0.0f);
        frame->depth.assign(pixels, 0.0f);
        frame->valid = true;
    }
    else if (!GaussianSplatVkRenderFrame(*this, cam, gpuSplats, *frame))
        std::fprintf(stderr, "gaussian_splat vulkan: frame dispatch failed; using CPU projection for this camera\n");
    vkFrame = frame;
    return vkFrame;
}

bool GaussianSplatCloud::EnsureProjectedCache(const GaussianSplatCamBasis& cam) const
{
    if (!cam.camValid)
        return false;

    std::lock_guard<std::mutex> lock(projCacheMutex);
    auto sameVec = [](const Vector3d& a, const Vector3d& b) {
        return std::fabs(a[X] - b[X]) < 1.0e-9 &&
               std::fabs(a[Y] - b[Y]) < 1.0e-9 &&
               std::fabs(a[Z] - b[Z]) < 1.0e-9;
    };
    if (projCache.valid &&
        sameVec(projCache.origin, cam.origin) &&
        sameVec(projCache.right, cam.right) &&
        sameVec(projCache.up, cam.up) &&
        sameVec(projCache.forward, cam.forward) &&
        std::fabs(projCache.fx - cam.fx) < 1.0e-6 &&
        std::fabs(projCache.fy - cam.fy) < 1.0e-6)
    {
        return !projCache.entries.empty();
    }

    BuildProjectedCache(cam);
    return projCache.valid && !projCache.entries.empty();
}

void GaussianSplatCloud::BuildProjectedCache(const GaussianSplatCamBasis& cam) const
{
    projCache.valid = false;
    projCache.entries.clear();
    projCache.tileOffsets.clear();
    projCache.tileIndices.clear();
    projCache.origin = cam.origin;
    projCache.right = cam.right;
    projCache.up = cam.up;
    projCache.forward = cam.forward;
    projCache.fx = cam.fx;
    projCache.fy = cam.fy;
    projCache.tileSize = kProjTileSize;

    if (points.empty() || cam.fx <= EPSILON || cam.fy <= EPSILON)
        return;

    // Veiled-baseline: project all opacity-passing Gaussians (no floater AABB /
    // neighbor / scale / screen-size culls from the later anti-fog pass).
    projCache.entries.reserve(points.size());

    DBL minU = BOUND_HUGE, maxU = -BOUND_HUGE, minV = BOUND_HUGE, maxV = -BOUND_HUGE;
    std::vector<DBL> projectedExtentsU;
    std::vector<DBL> projectedExtentsV;
    projectedExtentsU.reserve(points.size());
    projectedExtentsV.reserve(points.size());

    for (size_t pi = 0; pi < points.size(); ++pi)
    {
        const GaussianSplatPoint& sp = points[pi];
        const DBL opac = Clamp01(sp.opacity * opacityScale);
        if (opac <= 0.0 || opac <= opacityCutoff)
            continue;

        Vector3d pos = sp.position;
        const TRANSFORM *sourceTransform =
            (sp.transformIndex < sourceTransforms.size()) ? &sourceTransforms[sp.transformIndex] : nullptr;
        if (sourceTransform != nullptr)
            MTransPoint(pos, pos, sourceTransform);
        if (Trans != nullptr)
            MTransPoint(pos, pos, Trans);

        const Vector3d rel = pos - cam.origin;
        const DBL tx = dot(rel, cam.right);
        const DBL ty = dot(rel, cam.up);
        const DBL tz = dot(rel, cam.forward);
        if (tz <= EPSILON)
            continue;

        DBL SigmaW[3][3];
        BuildSigmaObject(sp, SigmaW, khrEllipse);
        TransformCovariance(SigmaW, sourceTransform);
        // KHR: Sigma = M C C^T M^T (also required whenever Trans rotates/scales).
        TransformCovariance(SigmaW, Trans);

        // Sigma_cam = W Sigma_world W^T  (rows of W = camera axes)
        const Vector3d axes[3] = { cam.right, cam.up, cam.forward };
        DBL SigmaC[3][3];
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                DBL s = 0.0;
                for (int a = 0; a < 3; ++a)
                {
                    DBL WaS = 0.0;
                    for (int b = 0; b < 3; ++b)
                        WaS += axes[i][b] * SigmaW[b][a];
                    s += WaS * axes[j][a];
                }
                SigmaC[i][j] = s;
            }
        }

        const DBL invZ = 1.0 / tz;
        const DBL invZ2 = invZ * invZ;
        const DBL jx0 = cam.fx * invZ, jx2 = -cam.fx * tx * invZ2;
        const DBL jy1 = cam.fy * invZ, jy2 = -cam.fy * ty * invZ2;

        auto Sj = [&](int row, int colJ) -> DBL {
            if (colJ == 0)
                return SigmaC[row][0] * jx0 + SigmaC[row][2] * jx2;
            return SigmaC[row][1] * jy1 + SigmaC[row][2] * jy2;
        };
        DBL cov00 = jx0 * Sj(0, 0) + jx2 * Sj(2, 0);
        DBL cov01 = jx0 * Sj(0, 1) + jx2 * Sj(2, 1);
        DBL cov11 = jy1 * Sj(1, 1) + jy2 * Sj(2, 1);
        // Khronos Sample Viewer EWA low-pass.
        cov00 += ewaFilter;
        cov11 += ewaFilter;
        const DBL det = cov00 * cov11 - cov01 * cov01;
        if (!std::isfinite(det) || det <= (khrEllipse ? 0.0 : EPSILON))
            continue;

        // Sample Viewer / khronos.vert: discard huge projected quads
        // (max(3*sqrt(a), 3*sqrt(c)) > max_quad_frac * min(W,H)). 0 = off.
        if (maxQuadFrac > EPSILON && cam.screenW > EPSILON && cam.screenH > EPSILON)
        {
            const DBL quadW = sigmaExtent * std::sqrt(std::max(0.0, cov00));
            const DBL quadH = sigmaExtent * std::sqrt(std::max(0.0, cov11));
            const DBL minScreen = std::min(cam.screenW, cam.screenH);
            if (std::max(quadW, quadH) > maxQuadFrac * minScreen)
                continue;
        }

        const DBL extentU = sigmaExtent * std::sqrt(std::max(0.0, cov00));
        const DBL extentV = sigmaExtent * std::sqrt(std::max(0.0, cov11));

        const DBL meanU = cam.fx * tx * invZ;
        const DBL meanV = cam.fy * ty * invZ;

        Vector3d rgb;
        if (lightingMode == GaussianSplatLightingMode::Relight)
            EvalDiffuseColour(sp, rgb);
        else
        {
            Vector3d camToSplat = rel;
            camToSplat.normalize();
            Vector3d shDir;
            ViewDirForSH(sp, camToSplat, shDir);
            EvalColour(sp, shDir, rgb);
        }
        // KHR: blend in float, tonemap/clamp after composition. Legacy path tonemaps early.
        if (!khrEllipse)
            rgb = TonemapSplatRgb(rgb);

        Vector3d normal(0.0, 0.0, 0.0);
        if (lightingMode == GaussianSplatLightingMode::Relight)
        {
            normal = SplatNormalObject(sp, sourceTransform);
            if (Trans != nullptr)
                MTransNormal(normal, normal, Trans);
            if (normal.lengthSqr() > EPSILON)
                normal.normalize();
            if (dot(normal, rel) > 0.0)
                normal = -normal;
        }

        ProjEntry e;
        e.meanU = meanU;
        e.meanV = meanV;
        e.cov00 = cov00;
        e.cov01 = cov01;
        e.cov11 = cov11;
        e.depth = tz;
        e.camDist = rel.length(); // KHR sortingMethod cameraDistance
        e.alpha = opac;
        e.r = rgb[X];
        e.g = rgb[Y];
        e.b = rgb[Z];
        e.nx = normal[X];
        e.ny = normal[Y];
        e.nz = normal[Z];
        projCache.entries.push_back(e);
        projectedExtentsU.push_back(extentU);
        projectedExtentsV.push_back(extentV);

        minU = std::min(minU, meanU - extentU);
        maxU = std::max(maxU, meanU + extentU);
        minV = std::min(minV, meanV - extentV);
        maxV = std::max(maxV, meanV + extentV);
    }

    if (projCache.entries.empty())
    {
        projCache.valid = true;
        return;
    }

    // Pad and build tile grid.
    const DBL pad = static_cast<DBL>(kProjTileSize);
    minU -= pad; maxU += pad; minV -= pad; maxV += pad;
    projCache.tileOriginU = minU;
    projCache.tileOriginV = minV;
    const DBL spanU = std::max(maxU - minU, static_cast<DBL>(kProjTileSize));
    const DBL spanV = std::max(maxV - minV, static_cast<DBL>(kProjTileSize));
    projCache.tilesX = std::max(1, static_cast<int>(std::ceil(spanU / kProjTileSize)));
    projCache.tilesY = std::max(1, static_cast<int>(std::ceil(spanV / kProjTileSize)));
    const int nTiles = projCache.tilesX * projCache.tilesY;

    std::vector<std::vector<int>> buckets(static_cast<size_t>(nTiles));
    for (size_t i = 0; i < projCache.entries.size(); ++i)
    {
        const ProjEntry& e = projCache.entries[i];
        const DBL extentU = projectedExtentsU[i];
        const DBL extentV = projectedExtentsV[i];
        const int x0 = std::max(0, static_cast<int>(std::floor((e.meanU - extentU - minU) / kProjTileSize)));
        const int x1 = std::min(projCache.tilesX - 1, static_cast<int>(std::floor((e.meanU + extentU - minU) / kProjTileSize)));
        const int y0 = std::max(0, static_cast<int>(std::floor((e.meanV - extentV - minV) / kProjTileSize)));
        const int y1 = std::min(projCache.tilesY - 1, static_cast<int>(std::floor((e.meanV + extentV - minV) / kProjTileSize)));
        for (int ty = y0; ty <= y1; ++ty)
            for (int tx = x0; tx <= x1; ++tx)
                buckets[static_cast<size_t>(ty * projCache.tilesX + tx)].push_back(static_cast<int>(i));
    }

    projCache.tileOffsets.resize(static_cast<size_t>(nTiles + 1));
    size_t total = 0;
    for (int t = 0; t < nTiles; ++t)
    {
        projCache.tileOffsets[static_cast<size_t>(t)] = static_cast<int>(total);
        total += buckets[static_cast<size_t>(t)].size();
    }
    projCache.tileOffsets[static_cast<size_t>(nTiles)] = static_cast<int>(total);
    projCache.tileIndices.resize(total);
    for (int t = 0; t < nTiles; ++t)
    {
        const int base = projCache.tileOffsets[static_cast<size_t>(t)];
        const auto& b = buckets[static_cast<size_t>(t)];
        for (size_t k = 0; k < b.size(); ++k)
            projCache.tileIndices[static_cast<size_t>(base) + k] = b[k];
    }

    projCache.valid = true;
}

bool GaussianSplatCloud::CompositeProjectedPixel(DBL screenU, DBL screenV,
                                                 GaussianSplatSegmentResult& out,
                                                 TraceThreadData *Thread) const
{
    out.colour = Vector3d(0.0, 0.0, 0.0);
    out.normal = Vector3d(0.0, 0.0, 0.0);
    out.transmittance = 1.0;
    out.depth = 0.0;
    out.valid = false;

    std::lock_guard<std::mutex> lock(projCacheMutex);
    if (!projCache.valid || projCache.entries.empty() || Thread == nullptr)
        return false;

    const int tx = static_cast<int>(std::floor((screenU - projCache.tileOriginU) / projCache.tileSize));
    const int ty = static_cast<int>(std::floor((screenV - projCache.tileOriginV) / projCache.tileSize));
    if (tx < 0 || ty < 0 || tx >= projCache.tilesX || ty >= projCache.tilesY)
        return false;

    const int tile = ty * projCache.tilesX + tx;
    const int begin = projCache.tileOffsets[static_cast<size_t>(tile)];
    const int end = projCache.tileOffsets[static_cast<size_t>(tile + 1)];
    if (begin >= end)
        return false;

    std::vector<TraceThreadData::GaussianSplatHitRec>& hits = Thread->GaussianSplatHits;
    hits.clear();
    const int hitCap = khrEllipse
        ? (end - begin)
        : std::min(kHardMaxHits, (maxHits > 0) ? maxHits : kDefaultMaxHits);
    hits.reserve(static_cast<size_t>(std::min(end - begin, hitCap)));

    for (int ii = begin; ii < end && static_cast<int>(hits.size()) < hitCap; ++ii)
    {
        const int ei = projCache.tileIndices[static_cast<size_t>(ii)];
        const ProjEntry& e = projCache.entries[static_cast<size_t>(ei)];
        const DBL du = screenU - e.meanU;
        const DBL dv = screenV - e.meanV;
        const DBL det = e.cov00 * e.cov11 - e.cov01 * e.cov01;
        if (det <= EPSILON)
            continue;
        const DBL power = (e.cov11 * du * du - 2.0 * e.cov01 * du * dv + e.cov00 * dv * dv) / det;
        const DBL powerCull = khrEllipse ? (sigmaExtent * sigmaExtent) : kProjPowerCullSuperSplat;
        if (power > powerCull)
            continue;
        const DBL g = GaussPower(power);
        const DBL alpha = std::min(alphaClamp, Clamp01(e.alpha * g));
        if (alpha <= 0.0 || alpha < opacityCutoff)
            continue;
        TraceThreadData::GaussianSplatHitRec rec;
        // KHR default sortingMethod cameraDistance; legacy Kerbl uses view-forward depth.
        if (khrEllipse)
            rec.t = khrSortCameraDistance ? e.camDist : e.depth;
        else
            rec.t = e.depth;
        rec.w = alpha;
        rec.idx = ei; // index into projCache.entries (colour baked in)
        hits.push_back(rec);
    }

    if (hits.empty())
        return false;

    // Singleton view-ray cull: one splat alone is usually a soft edge / floater.
    // Disabled under khrEllipse (spec has no singleton cull).
    if (cullSingletonHits && hits.size() < 2)
        return false;

    std::sort(hits.begin(), hits.end(),
              [](const TraceThreadData::GaussianSplatHitRec& a, const TraceThreadData::GaussianSplatHitRec& b) {
                  return a.t < b.t; // near first -> front-to-back
              });

    Vector3d acc(0.0, 0.0, 0.0);
    DBL T = 1.0;
    DBL depthSum = 0.0;
    DBL depthW = 0.0;
    Vector3d normalSum(0.0, 0.0, 0.0);
    for (size_t i = 0; i < hits.size() && (khrEllipse || T > (1.0 - alphaStop)); ++i)
    {
        const ProjEntry& e = projCache.entries[static_cast<size_t>(hits[i].idx)];
        const DBL alpha = hits[i].w;
        const Vector3d rgb(e.r, e.g, e.b);
        acc += rgb * (alpha * T);
        const DBL contrib = alpha * T;
        if (lightingMode == GaussianSplatLightingMode::Relight)
            normalSum += Vector3d(e.nx, e.ny, e.nz) * contrib;
        // Hit placement still uses view-forward depth (tz), even when sort key is camDist.
        depthSum += e.depth * contrib;
        depthW += contrib;
        T *= (1.0 - alpha);
    }

    const DBL alphaOut = Clamp01(1.0 - T);
    if (alphaOut <= 0.0 || (!khrEllipse && alphaOut < opacityCutoff))
        return false;

    out.colour = acc;
    out.normal = normalSum;
    if (out.normal.lengthSqr() > EPSILON)
        out.normal.normalize();
    out.transmittance = T;
    out.depth = (depthW > EPSILON) ? (depthSum / depthW) : hits[0].t;
    out.valid = true;
    return true;
}

bool GaussianSplatCloud::All_Intersections(const Ray& ray, IStack& Depth_Stack, TraceThreadData *Thread)
{
    if (points.empty())
        return false;
    BuildAcceleration();

    BasicRay localRay(ray);
    DBL lenScale = 1.0;
    if (Trans != nullptr)
    {
        MInvTransRay(localRay, ray, Trans);
        lenScale = localRay.Direction.length();
        if (lenScale < EPSILON)
            return false;
        localRay.Direction /= lenScale;
    }

    const Vector3d origin = localRay.Origin;
    const Vector3d dir = localRay.Direction;
    const Vector3d viewDir = -dir;

    GaussianSplatSegmentResult seg;
    const bool primaryProj = ray.IsPrimaryRay() && Thread != nullptr &&
                             Thread->GaussianSplatCam.camValid && Thread->GaussianSplatCam.pixelValid;

    // samples == 2 / khrEllipse: project/sort/blend when it hits.
    // khrEllipse: no volume IntegrateAlongRay refill (would break KHR silhouette).
    if ((samples == 2 || khrEllipse) && primaryProj)
    {
        GaussianSplatCamBasis cam;
        cam.origin = Thread->GaussianSplatCam.origin;
        cam.right = Thread->GaussianSplatCam.right;
        cam.up = Thread->GaussianSplatCam.up;
        cam.forward = Thread->GaussianSplatCam.forward;
        cam.fx = Thread->GaussianSplatCam.fx;
        cam.fy = Thread->GaussianSplatCam.fy;
        cam.screenU = Thread->GaussianSplatCam.screenU;
        cam.screenV = Thread->GaussianSplatCam.screenV;
        cam.screenW = Thread->GaussianSplatCam.screenW;
        cam.screenH = Thread->GaussianSplatCam.screenH;
        cam.camValid = true;
        cam.pixelValid = true;

        bool gotSeg = false;
        if (renderBackend == GaussianSplatRenderBackend::Vulkan)
        {
            // Shared immutable snapshot under vkFrameMutex (no per-thread map).
            const std::shared_ptr<const GaussianSplatVkFramebuffer> snap =
                PrepareVulkanFramebuffer(cam);
            if (snap != nullptr && snap->valid)
            {
                gotSeg = GaussianSplatVkSamplePixel(*snap, cam.screenU, cam.screenV, seg);
                // A transparent pixel in a completed GPU frame is a miss, not a backend failure.
                if (!gotSeg)
                    return false;
            }
        }
        if (!gotSeg && EnsureProjectedCache(cam))
            gotSeg = CompositeProjectedPixel(cam.screenU, cam.screenV, seg, Thread);
        if (gotSeg)
        {
            // View depth along camera forward → ray distance.
            const Vector3d rayDir = Vector3d(ray.Direction).normalized();
            const DBL cosFwd = dot(rayDir, cam.forward);
            if (cosFwd < 1.0e-6)
                return false;
            const DBL rayT = seg.depth / cosFwd;
            if (rayT < kDepthTolerance || rayT > MAX_DISTANCE)
                return false;
            Vector3d IPoint = ray.Evaluate(rayT);
            if (!(Clip.empty() || Point_In_Clip(IPoint, Clip, Thread)))
                return false;

            const DBL alphaOut = Clamp01(1.0 - seg.transmittance);
            Vector3d rgb = seg.colour;
            if (alphaOut > EPSILON)
                rgb /= alphaOut;
            // KHR: C clamped or tonemapped after float blend (display-referred).
            if (khrEllipse)
            {
                rgb = Vector3d(Clamp01(rgb[X]), Clamp01(rgb[Y]), Clamp01(rgb[Z]));
                if (khrColorSpace == "srgb_rec709_display")
                    rgb = Vector3d(SrgbToLinear(rgb[X]), SrgbToLinear(rgb[Y]), SrgbToLinear(rgb[Z]));
            }
            else
                rgb = TonemapSplatRgb(rgb);

            Thread->GaussianSplatColourValid = true;
            Thread->GaussianSplatColour = TransColour(ToMathColour(RGBColour(rgb[X], rgb[Y], rgb[Z])), 0.0, Clamp01(1.0 - alphaOut));

            Vector3d n = -ray.Direction;
            if (lightingMode == GaussianSplatLightingMode::Relight && seg.normal.lengthSqr() > EPSILON)
            {
                n = seg.normal;
                if (dot(n, ray.Direction) > 0.0)
                    n = -n;
                n.normalize();
            }
            Intersection isect(rayT, IPoint, n, this);
            isect.haveNormal = true;
            Depth_Stack->push(isect);
            return true;
        }
        if (khrEllipse)
            return false; // Spec miss: do not refill with 3D volume alpha.
    }

    // Secondary / non-projected: khrEllipse uses orthographic ray-plane Sigma
    // projection (no perspective J; KHR forward pass is defined for a camera).
    const bool useKerbl = primaryProj && !khrEllipse;
    if (!IntegrateAlongRay(origin, dir, kDepthTolerance, BOUND_HUGE, viewDir, seg, Thread, useKerbl))
        return false;

    DBL depth = seg.depth / lenScale;
    if (depth < kDepthTolerance || depth > MAX_DISTANCE)
        return false;

    Vector3d IPoint = ray.Evaluate(depth);
    if (!(Clip.empty() || Point_In_Clip(IPoint, Clip, Thread)))
        return false;

    const DBL alphaOut = Clamp01(1.0 - seg.transmittance);
    // Un-premultiply: POV multiplies pigment RGB by Opacity(=1-transmit) for emission.
    Vector3d rgb = seg.colour;
    if (alphaOut > EPSILON)
        rgb /= alphaOut;
    if (khrEllipse)
    {
        rgb = Vector3d(Clamp01(rgb[X]), Clamp01(rgb[Y]), Clamp01(rgb[Z]));
        if (khrColorSpace == "srgb_rec709_display")
            rgb = Vector3d(SrgbToLinear(rgb[X]), SrgbToLinear(rgb[Y]), SrgbToLinear(rgb[Z]));
    }
    else
        rgb = TonemapSplatRgb(rgb);

    Thread->GaussianSplatColourValid = true;
    Thread->GaussianSplatColour = TransColour(ToMathColour(RGBColour(rgb[X], rgb[Y], rgb[Z])), 0.0, Clamp01(1.0 - alphaOut));

    Vector3d n = -ray.Direction;
    if (lightingMode == GaussianSplatLightingMode::Relight && seg.normal.lengthSqr() > EPSILON)
    {
        n = seg.normal;
        if (Trans != nullptr)
            MTransNormal(n, n, Trans);
        if (dot(n, ray.Direction) > 0.0)
            n = -n;
        if (n.lengthSqr() > EPSILON)
            n.normalize();
    }
    Intersection isect(depth, IPoint, n, this);
    isect.haveNormal = true;
    Depth_Stack->push(isect);
    return true;
}

void GaussianSplatCloud::Determine_Textures(Intersection *, bool, WeightedTextureVector& textures, TraceThreadData *Thread)
{
    TEXTURE *tex = EnsureThreadSplatTexture(Thread);
    if (Thread->GaussianSplatColourValid && tex->Pigment != nullptr)
        tex->Pigment->colour = Thread->GaussianSplatColour;

    // Default: pure SH emission. Material path: splat RGB as pigment + scene finish/IOR.
    if (tex->Finish != nullptr)
    {
        if (WantsMaterialShading())
        {
            if (Texture != nullptr && Texture->Finish != nullptr)
            {
                const FINISH *src = Texture->Finish;
                tex->Finish->Diffuse = src->Diffuse;
                tex->Finish->DiffuseBack = src->DiffuseBack;
                tex->Finish->Brilliance = src->Brilliance;
                tex->Finish->Specular = src->Specular;
                tex->Finish->Roughness = src->Roughness;
                tex->Finish->Phong = src->Phong;
                tex->Finish->Phong_Size = src->Phong_Size;
                tex->Finish->Ambient = src->Ambient;
                tex->Finish->Emission = src->Emission;
                tex->Finish->Reflection_Max = src->Reflection_Max;
                tex->Finish->Reflection_Min = src->Reflection_Min;
                tex->Finish->Reflection_Falloff = src->Reflection_Falloff;
                tex->Finish->Reflection_Fresnel = src->Reflection_Fresnel;
                tex->Finish->Fresnel = src->Fresnel;
                tex->Finish->Metallic = src->Metallic;
                tex->Finish->Conserve_Energy = src->Conserve_Energy;
            }
            else
            {
                tex->Finish->Diffuse = 0.85f;
                tex->Finish->DiffuseBack = 0.0f;
                tex->Finish->Ambient = MathColour(0.08);
                tex->Finish->Specular = 0.15f;
                tex->Finish->Roughness = 0.05f;
                tex->Finish->Phong = 0.0f;
                tex->Finish->Emission = MathColour(0.15);
                tex->Finish->Reflection_Max = MathColour(0.0);
                tex->Finish->Reflection_Min = MathColour(0.0);
            }
            if (lightingMode == GaussianSplatLightingMode::Automatic &&
                tex->Finish->Emission.IsNearZero(EPSILON))
                tex->Finish->Emission = MathColour(0.15);
        }
        else
        {
            tex->Finish->Ambient = MathColour(0.0);
            tex->Finish->Diffuse = 0.0f;
            tex->Finish->DiffuseBack = 0.0f;
            tex->Finish->Specular = 0.0f;
            tex->Finish->Phong = 0.0f;
            tex->Finish->Emission = MathColour(1.0);
            tex->Finish->Reflection_Max = MathColour(0.0);
            tex->Finish->Reflection_Min = MathColour(0.0);
        }
    }

    textures.push_back(WeightedTexture(1.0, tex));
}

}
// end of namespace pov
