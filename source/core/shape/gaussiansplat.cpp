//******************************************************************************
///
/// @file core/shape/gaussiansplat.cpp
///
/// Experimental 3D Gaussian Splatting cloud: BVH traversal, soft Gaussian
/// weight along the ray (peak / Kerbl Jacobian EWA / Vol3DGS volume α), SH colour,
/// front-to-back alpha compositing. Offline CPU path via IntegrateAlongRay.
///
//******************************************************************************

#include "core/shape/gaussiansplat.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
const DBL kExtentSigma = 3.0;
const DBL kSqrtTwoPi = 2.5066282746310002; // √(2π)

inline DBL Clamp01(DBL v)
{
    return (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
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
    out[1][2] = 2 * (y * z + x * w);
    out[2][0] = 2 * (x * z - y * w);
    out[2][1] = 2 * (y * z + x * w);
    out[2][2] = 1 - 2 * (x * x + y * y);
}

void SplatWorldAABB(const GaussianSplatPoint& sp, Vector3d& bmin, Vector3d& bmax)
{
    MATRIX R;
    QuatToMatrix(sp.qw, sp.qx, sp.qy, sp.qz, R);
    // Conservative AABB: 3-sigma extent of oriented ellipsoid axes projected to world axes.
    Vector3d ext(
        kExtentSigma * (std::fabs(R[0][0]) * sp.scale[X] + std::fabs(R[0][1]) * sp.scale[Y] + std::fabs(R[0][2]) * sp.scale[Z]),
        kExtentSigma * (std::fabs(R[1][0]) * sp.scale[X] + std::fabs(R[1][1]) * sp.scale[Y] + std::fabs(R[1][2]) * sp.scale[Z]),
        kExtentSigma * (std::fabs(R[2][0]) * sp.scale[X] + std::fabs(R[2][1]) * sp.scale[Y] + std::fabs(R[2][2]) * sp.scale[Z]));
    bmin = sp.position - ext;
    bmax = sp.position + ext;
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
    shDegree(3),
    opacityCutoff(1.0 / 255.0),
    alphaStop(0.999),
    samples(4),
    maxHits(0),
    bvhLeafSize(1),
    giWeight(1.0),
    opacityScale(1.0)
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
    New->restCoeffs = restCoeffs;
    New->restCount = restCount;
    New->shDegree = shDegree;
    New->opacityCutoff = opacityCutoff;
    New->alphaStop = alphaStop;
    New->samples = samples;
    New->maxHits = maxHits;
    New->bvhLeafSize = bvhLeafSize;
    New->giWeight = giWeight;
    New->opacityScale = opacityScale;
    New->bvh = bvh;
    New->bvhOrder = bvhOrder;
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
    Compute_BBox();
}

void GaussianSplatCloud::Compute_BBox()
{
    if (points.empty())
    {
        Make_BBox(BBox, 0, 0, 0, 0, 0, 0);
        return;
    }

    if (bvh.empty())
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
    bvh.clear();
    bvhOrder.resize(points.size());
    for (size_t i = 0; i < points.size(); ++i)
        bvhOrder[i] = static_cast<int>(i);
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
        SplatWorldAABB(points[static_cast<size_t>(bvhOrder[static_cast<size_t>(i)])], smin, smax);
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

bool GaussianSplatCloud::SplatContribution(const GaussianSplatPoint& sp,
                                           const Vector3d& origin, const Vector3d& dir,
                                           DBL tSeg0, DBL tSeg1, DBL& tHit, DBL& weight,
                                           TraceThreadData *Thread, bool useKerbl) const
{
    const DBL opac = Clamp01(sp.opacity * opacityScale);
    if (opac < opacityCutoff)
        return false;

    MATRIX Rmat;
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

    auto buildSigmaWorld = [&](DBL Sigma[3][3]) {
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
    if (samples == 2)
    {
        const TraceThreadData::GaussianSplatProj *proj =
            (useKerbl && Thread != nullptr && Thread->GaussianSplatCam.camValid &&
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

            DBL SigmaW[3][3];
            buildSigmaWorld(SigmaW);

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

            // J (2×3): affine approx of projective transform at mean (Kerbl et al.).
            const DBL fx = proj->fx, fy = proj->fy;
            const DBL invZ = 1.0 / tz;
            const DBL invZ2 = invZ * invZ;
            const DBL J00 = fx * invZ, J02 = -fx * tx * invZ2;
            const DBL J11 = fy * invZ, J12 = -fy * ty * invZ2;

            // cov2d = J * Σ_cam * Jᵀ  (2×2), then +0.3 px low-pass on diagonal.
            auto Sj = [&](int row, int colJ) -> DBL {
                // (Σ_cam * Jᵀ)[row][colJ]
                if (colJ == 0)
                    return SigmaC[row][0] * J00 + SigmaC[row][2] * J02;
                return SigmaC[row][1] * J11 + SigmaC[row][2] * J12;
            };
            DBL cov00 = J00 * Sj(0, 0) + J02 * Sj(2, 0);
            DBL cov01 = J00 * Sj(0, 1) + J02 * Sj(2, 1);
            DBL cov11 = J11 * Sj(1, 1) + J12 * Sj(2, 1);
            cov00 += 0.3;
            cov11 += 0.3;

            const DBL det = cov00 * cov11 - cov01 * cov01;
            if (det < EPSILON)
                return false;

            const DBL meanU = fx * tx * invZ;
            const DBL meanV = fy * ty * invZ;
            const DBL du = proj->screenU - meanU;
            const DBL dv = proj->screenV - meanV;
            const DBL power = (cov11 * du * du - 2.0 * cov01 * du * dv + cov00 * dv * dv) / det;
            if (power > (kExtentSigma * kExtentSigma))
                return false;

            weight = Clamp01(opac * std::exp(-0.5 * power));
            return weight >= opacityCutoff;
        }

        // Billboard fallback: Σ projected into plane ⟂ ray (no perspective J).
        const Vector3d om = sp.position - origin;
        tHit = dot(om, dir);
        if (tHit < tSeg0 || tHit > tSeg1)
            return false;

        const Vector3d mid = origin + dir * tHit;
        const Vector3d delta = sp.position - mid;

        DBL Sigma[3][3];
        buildSigmaWorld(Sigma);

        Vector3d axis = (std::fabs(dir[Z]) < 0.9) ? Vector3d(0.0, 0.0, 1.0) : Vector3d(0.0, 1.0, 0.0);
        Vector3d u = cross(dir, axis);
        const DBL ul = u.length();
        if (ul < EPSILON)
            return false;
        u /= ul;
        Vector3d v = cross(dir, u);

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
        if (power > (kExtentSigma * kExtentSigma))
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
        if (tHit < (tSeg0 - kExtentSigma * beta) || tHit > (tSeg1 + kExtentSigma * beta))
            return false;

        const Vector3d closest = oL + dL * tHit;
        const DBL gPeak = std::exp(-0.5 * closest.lengthSqr());
        if (gPeak < kWeightEps)
            return false;

        const DBL invSigma = 1.0 / (beta * std::sqrt(2.0));
        const DBL tLo = std::max(tSeg0, tHit - kExtentSigma * beta);
        const DBL tHi = std::min(tSeg1, tHit + kExtentSigma * beta);
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

void GaussianSplatCloud::EvalColour(const GaussianSplatPoint& sp, const Vector3d& viewDir, Vector3d& rgb) const
{
    // viewDir should point from splat toward camera (opposite of ray direction).
    Vector3d dir = viewDir.normalized();
    const DBL x = dir[X], y = dir[Y], z = dir[Z];

    rgb = Vector3d(
        0.5 + kShC0 * sp.dc[X],
        0.5 + kShC0 * sp.dc[Y],
        0.5 + kShC0 * sp.dc[Z]);

    const int deg = std::min(shDegree, 3);
    if (deg < 1 || restCount < 9 || restCoeffs.empty())
    {
        rgb = Vector3d(Clamp01(rgb[X]), Clamp01(rgb[Y]), Clamp01(rgb[Z]));
        return;
    }

    const DBL *rest = &restCoeffs[sp.restOffset];
    auto band = [&](int b, int c) -> DBL {
        const unsigned idx = static_cast<unsigned>(b * 3 + c);
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

    rgb = Vector3d(Clamp01(rgb[X]), Clamp01(rgb[Y]), Clamp01(rgb[Z]));
}

bool GaussianSplatCloud::IntegrateAlongRay(const Vector3d& origin, const Vector3d& dir,
                                           DBL t0, DBL t1, const Vector3d& viewDir,
                                           GaussianSplatSegmentResult& out, TraceThreadData *Thread,
                                           bool useKerbl) const
{
    out.colour = Vector3d(0.0, 0.0, 0.0);
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
        DBL tn, tf;
        if (!RayAABB(origin, invDir, node.bmin, node.bmax, tn, tf))
            continue;
        if (tf < seg0 || tn > seg1)
            continue;
        if (node.left < 0)
        {
            for (int i = 0; i < node.count && static_cast<int>(hits.size()) < hitCap; ++i)
            {
                const int pi = bvhOrder[static_cast<size_t>(node.first + i)];
                DBL tHit = 0, w = 0;
                if (SplatContribution(points[static_cast<size_t>(pi)], origin, dir, seg0, seg1, tHit, w,
                                      Thread, useKerbl))
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
                                      bvh[static_cast<size_t>(node.left)].bmax, l0, l1);
            const bool hitR = RayAABB(origin, invDir, bvh[static_cast<size_t>(node.right)].bmin,
                                      bvh[static_cast<size_t>(node.right)].bmax, r0, r1);
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

    std::sort(hits.begin(), hits.end(),
              [](const TraceThreadData::GaussianSplatHitRec& a, const TraceThreadData::GaussianSplatHitRec& b) {
                  return a.t < b.t;
              });

    Vector3d acc(0.0, 0.0, 0.0);
    DBL T = 1.0;
    DBL depthSum = 0.0;
    DBL depthW = 0.0;

    for (size_t i = 0; i < hits.size() && T > (1.0 - alphaStop); ++i)
    {
        // SplatContribution already returns final per-splat α (peak/EWA/volume).
        const DBL alpha = hits[i].w;
        if (alpha < opacityCutoff)
            continue;
        const GaussianSplatPoint& spoint = points[static_cast<size_t>(hits[i].idx)];
        Vector3d rgb;
        EvalColour(spoint, viewDir, rgb);
        acc += rgb * (alpha * T);
        const DBL contrib = alpha * T;
        depthSum += hits[i].t * contrib;
        depthW += contrib;
        T *= (1.0 - alpha);
    }

    const DBL alphaOut = Clamp01(1.0 - T);
    if (alphaOut < opacityCutoff)
        return false;

    out.colour = acc;
    out.transmittance = T;
    out.depth = (depthW > EPSILON) ? (depthSum / depthW) : hits[0].t;
    out.valid = true;
    return true;
}

bool GaussianSplatCloud::All_Intersections(const Ray& ray, IStack& Depth_Stack, TraceThreadData *Thread)
{
    if (points.empty())
        return false;
    if (bvh.empty())
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
    const bool useKerbl = ray.IsPrimaryRay() && Thread != nullptr &&
                          Thread->GaussianSplatCam.camValid && Thread->GaussianSplatCam.pixelValid;
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
    rgb = Vector3d(Clamp01(rgb[X]), Clamp01(rgb[Y]), Clamp01(rgb[Z]));

    Thread->GaussianSplatColourValid = true;
    Thread->GaussianSplatColour = TransColour(ToMathColour(RGBColour(rgb[X], rgb[Y], rgb[Z])), 0.0, Clamp01(1.0 - alphaOut));

    Vector3d n = -ray.Direction;
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
    textures.push_back(WeightedTexture(1.0, tex));
}

}
// end of namespace pov
