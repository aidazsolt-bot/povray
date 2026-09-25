//******************************************************************************
///
/// @file core/shape/gaussiansplat.cpp
///
/// Experimental 3D Gaussian Splatting cloud: BVH traversal, soft Gaussian
/// weight along the ray, SH colour, front-to-back alpha compositing.
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
const int kMaxHitsPerRay = 512;
const DBL kExtentSigma = 3.0;

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
    opacityCutoff(0.01),
    alphaStop(0.995)
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
    if (count <= 4 || depth > 48)
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
                                           DBL& tHit, DBL& weight) const
{
    if (sp.opacity < opacityCutoff)
        return false;

    MATRIX R;
    QuatToMatrix(sp.qw, sp.qx, sp.qy, sp.qz, R);

    // Local = R^T * (p - mean), then divide by scale → unit isotropic Gaussian.
    Vector3d delta = origin - sp.position;
    Vector3d oL(
        R[0][0] * delta[X] + R[1][0] * delta[Y] + R[2][0] * delta[Z],
        R[0][1] * delta[X] + R[1][1] * delta[Y] + R[2][1] * delta[Z],
        R[0][2] * delta[X] + R[1][2] * delta[Y] + R[2][2] * delta[Z]);
    Vector3d dL(
        R[0][0] * dir[X] + R[1][0] * dir[Y] + R[2][0] * dir[Z],
        R[0][1] * dir[X] + R[1][1] * dir[Y] + R[2][1] * dir[Z],
        R[0][2] * dir[X] + R[1][2] * dir[Y] + R[2][2] * dir[Z]);

    const DBL sx = std::max(sp.scale[X], EPSILON);
    const DBL sy = std::max(sp.scale[Y], EPSILON);
    const DBL sz = std::max(sp.scale[Z], EPSILON);
    oL[X] /= sx; oL[Y] /= sy; oL[Z] /= sz;
    dL[X] /= sx; dL[Y] /= sy; dL[Z] /= sz;

    const DBL dd = dL.lengthSqr();
    if (dd < EPSILON)
        return false;
    tHit = -dot(oL, dL) / dd;
    if (tHit < kDepthTolerance)
        return false;

    const Vector3d closest = oL + dL * tHit;
    const DBL dist2 = closest.lengthSqr();
    weight = std::exp(-0.5 * dist2);
    return weight >= kWeightEps;
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
    Vector3d invDir(
        (std::fabs(dir[X]) > EPSILON) ? (1.0 / dir[X]) : BOUND_HUGE,
        (std::fabs(dir[Y]) > EPSILON) ? (1.0 / dir[Y]) : BOUND_HUGE,
        (std::fabs(dir[Z]) > EPSILON) ? (1.0 / dir[Z]) : BOUND_HUGE);

    DBL rootNear = 0, rootFar = 0;
    if (!RayAABB(origin, invDir, bvh[0].bmin, bvh[0].bmax, rootNear, rootFar))
        return false;

    // AABB hit is required; splat contributions follow.
    struct HitRec { DBL t; DBL w; int idx; };
    HitRec hits[kMaxHitsPerRay];
    int nHits = 0;

    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0)
    {
        const int ni = stack[--sp];
        const GaussianSplatBVHNode& node = bvh[static_cast<size_t>(ni)];
        DBL t0, t1;
        if (!RayAABB(origin, invDir, node.bmin, node.bmax, t0, t1))
            continue;
        if (node.left < 0)
        {
            for (int i = 0; i < node.count && nHits < kMaxHitsPerRay; ++i)
            {
                const int pi = bvhOrder[static_cast<size_t>(node.first + i)];
                DBL tHit = 0, w = 0;
                if (SplatContribution(points[static_cast<size_t>(pi)], origin, dir, tHit, w))
                {
                    hits[nHits].t = tHit;
                    hits[nHits].w = w;
                    hits[nHits].idx = pi;
                    ++nHits;
                }
            }
        }
        else
        {
            // Push farther child first so nearer is processed first (approx).
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

    if (nHits == 0)
        return false;

    std::sort(hits, hits + nHits, [](const HitRec& a, const HitRec& b) { return a.t < b.t; });

    Vector3d acc(0.0, 0.0, 0.0);
    DBL T = 1.0; // transmittance
    DBL depthSum = 0.0;
    DBL depthW = 0.0;
    const Vector3d viewDir = -dir; // toward camera along the ray

    for (int i = 0; i < nHits && T > (1.0 - alphaStop); ++i)
    {
        const GaussianSplatPoint& spoint = points[static_cast<size_t>(hits[i].idx)];
        const DBL alpha = Clamp01(spoint.opacity * hits[i].w);
        if (alpha < opacityCutoff)
            continue;
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

    DBL depth = (depthW > EPSILON) ? (depthSum / depthW) : hits[0].t;
    depth /= lenScale;
    if (depth < kDepthTolerance || depth > MAX_DISTANCE)
        return false;

    Vector3d IPoint = ray.Evaluate(depth);
    if (!(Clip.empty() || Point_In_Clip(IPoint, Clip, Thread)))
        return false;

    // Un-premultiply: POV multiplies pigment RGB by Opacity(=1-transmit) for emission.
    Vector3d rgb = acc;
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
