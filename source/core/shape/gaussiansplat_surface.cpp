//******************************************************************************
///
/// @file core/shape/gaussiansplat_surface.cpp
///
/// SuperSplat-inspired surface from Gaussian splat storage only:
///   - filter_floaters defaults 0.05 / 0.1 / 0.004 (configurable)
///   - binary occupancy voxels (voxel_size / voxel_opacity configurable)
///   - mesh: voxel faces (default) or surface_smooth marching cubes
///   - triangle pigment + normals via EvalColour (not Assimp)
///
//******************************************************************************

#include "core/shape/gaussiansplat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include "base/colour.h"
#include "base/pov_mem.h"
#include "core/bounding/boundingbox.h"
#include "core/colour/spectral.h"
#include "core/material/interior.h"
#include "core/material/pigment.h"
#include "core/material/texture.h"
#include "core/math/matrix.h"
#include "core/math/vector.h"
#include "core/shape/mesh.h"

#include "base/povdebug.h"

namespace pov
{

namespace
{

#include "core/shape/gaussiansplat_mc_luts.inc"

inline DBL Clamp01Local(DBL v)
{
    return (v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v);
}

void QuatToR(DBL w, DBL x, DBL y, DBL z, MATRIX R)
{
    DBL n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n > EPSILON)
    {
        w /= n;
        x /= n;
        y /= n;
        z /= n;
    }
    MIdentity(R);
    R[0][0] = 1 - 2 * (y * y + z * z);
    R[0][1] = 2 * (x * y - z * w);
    R[0][2] = 2 * (x * z + y * w);
    R[1][0] = 2 * (x * y + z * w);
    R[1][1] = 1 - 2 * (x * x + z * z);
    R[1][2] = 2 * (y * z - x * w);
    R[2][0] = 2 * (x * z - y * w);
    R[2][1] = 2 * (y * z + x * w);
    R[2][2] = 1 - 2 * (x * x + y * y);
}

TEXTURE *MakeSurfaceTriTexture(DBL r, DBL g, DBL b, DBL localOpacity, const FINISH *srcFinish,
                               bool iorMode, bool inverseMode)
{
    TEXTURE *tex = Create_Texture();
    Destroy_Pigment(tex->Pigment);
    tex->Pigment = Create_Pigment();
    if (tex->Finish == nullptr)
        tex->Finish = Create_Finish();

    const DBL opac = Clamp01Local(localOpacity);
    // Inverse surface: always opaque. IOR mode: density-weighted transmit.
    const DBL transmit = (inverseMode || !iorMode) ? 0.0 : Clamp01Local(1.0 - opac * 0.85);
    RGBColour rgb(Clamp01Local(r), Clamp01Local(g), Clamp01Local(b));
    tex->Pigment->colour = TransColour(pov_base::ToMathColour(rgb), 0.0, transmit);

    if (inverseMode)
    {
        // Opaque extracted surface: SH as diffuse albedo + mild specular (no glass).
        if (srcFinish != nullptr)
        {
            const FINISH *src = srcFinish;
            tex->Finish->Diffuse = src->Diffuse > EPSILON ? src->Diffuse : 0.85f;
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
            tex->Finish->Ambient = MathColour(0.05);
            tex->Finish->Diffuse = 0.85f;
            tex->Finish->Specular = 0.15f;
            tex->Finish->Roughness = 0.08f;
            tex->Finish->Emission = MathColour(0.0);
            tex->Finish->Reflection_Max = MathColour(0.0);
            tex->Finish->Reflection_Min = MathColour(0.0);
        }
        Post_Textures(tex);
        return tex;
    }

    if (srcFinish != nullptr)
    {
        const FINISH *src = srcFinish;
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
        // Spatially weight specular/reflection by local opacity (IOR look matches density).
        if (iorMode)
        {
            const DBL w = 0.35 + 0.65 * opac;
            tex->Finish->Specular = static_cast<SNGL>(Clamp01Local(static_cast<DBL>(tex->Finish->Specular) * w));
            tex->Finish->Reflection_Max = tex->Finish->Reflection_Max * w;
            tex->Finish->Reflection_Min = tex->Finish->Reflection_Min * w;
        }
    }
    else if (iorMode)
    {
        // Defaults matching rose_ior.pov glass-ish finish, weighted by local opacity.
        const DBL w = 0.35 + 0.65 * opac;
        tex->Finish->Ambient = MathColour(0.02);
        tex->Finish->Diffuse = static_cast<SNGL>(0.35);
        tex->Finish->Specular = static_cast<SNGL>(0.55 * w);
        tex->Finish->Roughness = static_cast<SNGL>(0.02);
        tex->Finish->Emission = pov_base::ToMathColour(rgb) * (0.1 * opac);
        tex->Finish->Reflection_Max = MathColour(0.12 * w);
        tex->Finish->Reflection_Min = MathColour(0.0);
        tex->Finish->Metallic = static_cast<SNGL>(0.15);
        tex->Finish->Fresnel = 1.0;
        tex->Finish->Reflection_Fresnel = true;
    }
    else
    {
        tex->Finish->Ambient = MathColour(0.08);
        tex->Finish->Diffuse = 0.75;
        tex->Finish->Specular = 0.2;
        tex->Finish->Roughness = 0.1;
    }
    Post_Textures(tex);
    return tex;
}

inline int PackRGB5(DBL r, DBL g, DBL b)
{
    const int ri = std::min(31, std::max(0, static_cast<int>(Clamp01Local(r) * 31.0 + 0.5)));
    const int gi = std::min(31, std::max(0, static_cast<int>(Clamp01Local(g) * 31.0 + 0.5)));
    const int bi = std::min(31, std::max(0, static_cast<int>(Clamp01Local(b) * 31.0 + 0.5)));
    return (ri << 10) | (gi << 5) | bi;
}

inline int Idx3(int x, int y, int z, int nx, int ny)
{
    return (z * ny + y) * nx + x;
}

struct GridSpec
{
    Vector3d origin;
    DBL voxel;
    int nx, ny, nz;
};

bool ComputeGrid(const std::vector<GaussianSplatPoint>& points, const std::vector<int>& keep,
                 DBL voxelSizeHint, DBL padSigma, GridSpec& out)
{
    Vector3d bmin(BOUND_HUGE, BOUND_HUGE, BOUND_HUGE);
    Vector3d bmax(-BOUND_HUGE, -BOUND_HUGE, -BOUND_HUGE);
    for (int id : keep)
    {
        const GaussianSplatPoint& sp = points[static_cast<size_t>(id)];
        const DBL rad = padSigma * std::max(sp.scale[X], std::max(sp.scale[Y], sp.scale[Z]));
        for (int a = 0; a < 3; ++a)
        {
            bmin[a] = std::min(bmin[a], sp.position[a] - rad);
            bmax[a] = std::max(bmax[a], sp.position[a] + rad);
        }
    }
    const Vector3d span = bmax - bmin;
    const DBL diag = span.length();
    if (diag < EPSILON)
        return false;

    DBL voxel = voxelSizeHint;
    if (voxel <= EPSILON)
        voxel = std::max(diag / 120.0, 1.0e-5);

    int nx = std::max(2, static_cast<int>(std::ceil(span[X] / voxel)) + 1);
    int ny = std::max(2, static_cast<int>(std::ceil(span[Y] / voxel)) + 1);
    int nz = std::max(2, static_cast<int>(std::ceil(span[Z] / voxel)) + 1);
    while (static_cast<long long>(nx) * ny * nz > 192LL * 192LL * 192LL)
    {
        voxel *= 1.15;
        nx = std::max(2, static_cast<int>(std::ceil(span[X] / voxel)) + 1);
        ny = std::max(2, static_cast<int>(std::ceil(span[Y] / voxel)) + 1);
        nz = std::max(2, static_cast<int>(std::ceil(span[Z] / voxel)) + 1);
    }
    out.origin = bmin;
    out.voxel = voxel;
    out.nx = nx;
    out.ny = ny;
    out.nz = nz;
    return true;
}

void StampOccupancy(const std::vector<GaussianSplatPoint>& points, const std::vector<int>& keep,
                    DBL opacityScale, DBL occThresh, const GridSpec& grid,
                    std::vector<uint8_t>& solid, std::vector<uint8_t>* contribMask)
{
    solid.assign(static_cast<size_t>(grid.nx) * grid.ny * grid.nz, 0);
    if (contribMask != nullptr)
        contribMask->assign(keep.size(), 0);

    for (size_t ki = 0; ki < keep.size(); ++ki)
    {
        const int id = keep[ki];
        const GaussianSplatPoint& sp = points[static_cast<size_t>(id)];
        const DBL opac = Clamp01Local(sp.opacity * opacityScale);
        MATRIX R;
        QuatToR(sp.qw, sp.qx, sp.qy, sp.qz, R);
        const DBL invSx = 1.0 / std::max(sp.scale[X], 1.0e-8);
        const DBL invSy = 1.0 / std::max(sp.scale[Y], 1.0e-8);
        const DBL invSz = 1.0 / std::max(sp.scale[Z], 1.0e-8);

        Vector3d cmin = sp.position;
        Vector3d cmax = sp.position;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                {
                    Vector3d local(sx * 2.5 * sp.scale[X], sy * 2.5 * sp.scale[Y], sz * 2.5 * sp.scale[Z]);
                    Vector3d world(
                        R[0][0] * local[X] + R[0][1] * local[Y] + R[0][2] * local[Z],
                        R[1][0] * local[X] + R[1][1] * local[Y] + R[1][2] * local[Z],
                        R[2][0] * local[X] + R[2][1] * local[Y] + R[2][2] * local[Z]);
                    world += sp.position;
                    for (int a = 0; a < 3; ++a)
                    {
                        cmin[a] = std::min(cmin[a], world[a]);
                        cmax[a] = std::max(cmax[a], world[a]);
                    }
                }

        const int i0 = std::max(0, static_cast<int>(std::floor((cmin[X] - grid.origin[X]) / grid.voxel)));
        const int j0 = std::max(0, static_cast<int>(std::floor((cmin[Y] - grid.origin[Y]) / grid.voxel)));
        const int k0 = std::max(0, static_cast<int>(std::floor((cmin[Z] - grid.origin[Z]) / grid.voxel)));
        const int i1 = std::min(grid.nx - 1, static_cast<int>(std::ceil((cmax[X] - grid.origin[X]) / grid.voxel)));
        const int j1 = std::min(grid.ny - 1, static_cast<int>(std::ceil((cmax[Y] - grid.origin[Y]) / grid.voxel)));
        const int k1 = std::min(grid.nz - 1, static_cast<int>(std::ceil((cmax[Z] - grid.origin[Z]) / grid.voxel)));

        bool contributed = false;
        for (int iz = k0; iz <= k1; ++iz)
            for (int iy = j0; iy <= j1; ++iy)
                for (int ix = i0; ix <= i1; ++ix)
                {
                    const Vector3d p(
                        grid.origin[X] + (ix + 0.5) * grid.voxel,
                        grid.origin[Y] + (iy + 0.5) * grid.voxel,
                        grid.origin[Z] + (iz + 0.5) * grid.voxel);
                    Vector3d d = p - sp.position;
                    const DBL lx = R[0][0] * d[X] + R[1][0] * d[Y] + R[2][0] * d[Z];
                    const DBL ly = R[0][1] * d[X] + R[1][1] * d[Y] + R[2][1] * d[Z];
                    const DBL lz = R[0][2] * d[X] + R[1][2] * d[Y] + R[2][2] * d[Z];
                    const DBL m2 = (lx * invSx) * (lx * invSx) + (ly * invSy) * (ly * invSy) + (lz * invSz) * (lz * invSz);
                    const DBL contrib = opac * std::exp(-0.5 * m2);
                    if (contrib >= occThresh)
                    {
                        solid[static_cast<size_t>(Idx3(ix, iy, iz, grid.nx, grid.ny))] = 1;
                        contributed = true;
                    }
                }
        if (contribMask != nullptr && contributed)
            (*contribMask)[ki] = 1;
    }
}

struct TriRec
{
    Vector3d v0, v1, v2;
    Vector3d n;
    DBL r, g, b;
    DBL opac;
};

void SampleColour(const GaussianSplatCloud *cloud, const std::vector<int>& keep,
                  const Vector3d& pos, const Vector3d& outwardN, DBL voxel,
                  DBL& cr, DBL& cg, DBL& cb, DBL& localOpac)
{
    Vector3d view = -outwardN;
    if (view.lengthSqr() < EPSILON)
        view = Vector3d(0.0, 0.0, 1.0);
    else
        view.normalize();

    DBL wsum = 0.0;
    DBL opacSum = 0.0;
    Vector3d acc(0.0, 0.0, 0.0);
    const DBL searchR2 = (voxel * 12.0) * (voxel * 12.0);
    for (int id : keep)
    {
        const GaussianSplatPoint& sp = cloud->points[static_cast<size_t>(id)];
        const Vector3d d = pos - sp.position;
        const DBL d2 = d.lengthSqr();
        if (d2 > searchR2)
            continue;
        const DBL op = Clamp01Local(sp.opacity * cloud->opacityScale);
        const DBL w = op / (d2 + voxel * voxel);
        if (w < 1.0e-6)
            continue;
        Vector3d rgb;
        cloud->EvalColour(sp, view, rgb);
        acc += rgb * w;
        opacSum += op * w;
        wsum += w;
    }
    if (wsum < EPSILON)
    {
        cr = cg = cb = 0.55;
        localOpac = 0.5;
        return;
    }
    acc /= wsum;
    localOpac = Clamp01Local(opacSum / wsum);
    cr = Clamp01Local(acc[X]);
    cg = Clamp01Local(acc[Y]);
    cb = Clamp01Local(acc[Z]);
}

void EmitFaces(const std::vector<uint8_t>& solid, const GridSpec& grid,
               const GaussianSplatCloud *cloud, const std::vector<int>& keep,
               std::vector<TriRec>& tris)
{
    auto empty = [&](int x, int y, int z) -> bool {
        if (x < 0 || y < 0 || z < 0 || x >= grid.nx || y >= grid.ny || z >= grid.nz)
            return true;
        return solid[static_cast<size_t>(Idx3(x, y, z, grid.nx, grid.ny))] == 0;
    };
    auto emitFace = [&](Vector3d c0, Vector3d c1, Vector3d c2, Vector3d c3, Vector3d n) {
        Vector3d e1 = c1 - c0;
        Vector3d e2 = c2 - c0;
        if (dot(cross(e1, e2), n) < 0.0)
            std::swap(c1, c3);
        TriRec t0, t1;
        t0.v0 = c0; t0.v1 = c1; t0.v2 = c2; t0.n = n;
        t1.v0 = c0; t1.v1 = c2; t1.v2 = c3; t1.n = n;
        const Vector3d mid = (c0 + c1 + c2 + c3) * 0.25;
        SampleColour(cloud, keep, mid, n, grid.voxel, t0.r, t0.g, t0.b, t0.opac);
        t1.r = t0.r; t1.g = t0.g; t1.b = t0.b; t1.opac = t0.opac;
        tris.push_back(t0);
        tris.push_back(t1);
    };

    const DBL h = grid.voxel;
    for (int iz = 0; iz < grid.nz; ++iz)
        for (int iy = 0; iy < grid.ny; ++iy)
            for (int ix = 0; ix < grid.nx; ++ix)
            {
                if (!solid[static_cast<size_t>(Idx3(ix, iy, iz, grid.nx, grid.ny))])
                    continue;
                const DBL x0 = grid.origin[X] + ix * h;
                const DBL y0 = grid.origin[Y] + iy * h;
                const DBL z0 = grid.origin[Z] + iz * h;
                const DBL x1 = x0 + h, y1 = y0 + h, z1 = z0 + h;
                if (empty(ix - 1, iy, iz))
                    emitFace(Vector3d(x0, y0, z0), Vector3d(x0, y0, z1), Vector3d(x0, y1, z1), Vector3d(x0, y1, z0), Vector3d(-1, 0, 0));
                if (empty(ix + 1, iy, iz))
                    emitFace(Vector3d(x1, y0, z0), Vector3d(x1, y1, z0), Vector3d(x1, y1, z1), Vector3d(x1, y0, z1), Vector3d(1, 0, 0));
                if (empty(ix, iy - 1, iz))
                    emitFace(Vector3d(x0, y0, z0), Vector3d(x1, y0, z0), Vector3d(x1, y0, z1), Vector3d(x0, y0, z1), Vector3d(0, -1, 0));
                if (empty(ix, iy + 1, iz))
                    emitFace(Vector3d(x0, y1, z0), Vector3d(x0, y1, z1), Vector3d(x1, y1, z1), Vector3d(x1, y1, z0), Vector3d(0, 1, 0));
                if (empty(ix, iy, iz - 1))
                    emitFace(Vector3d(x0, y0, z0), Vector3d(x0, y1, z0), Vector3d(x1, y1, z0), Vector3d(x1, y0, z0), Vector3d(0, 0, -1));
                if (empty(ix, iy, iz + 1))
                    emitFace(Vector3d(x0, y0, z1), Vector3d(x1, y0, z1), Vector3d(x1, y1, z1), Vector3d(x0, y1, z1), Vector3d(0, 0, 1));
            }
}

// Cube corner offsets and edge endpoint indices (Bourke).
const int kMcCorner[8][3] = {
    {0,0,0},{1,0,0},{1,1,0},{0,1,0},
    {0,0,1},{1,0,1},{1,1,1},{0,1,1}
};
const int kMcEdgeVert[12][2] = {
    {0,1},{1,2},{2,3},{3,0},
    {4,5},{5,6},{6,7},{7,4},
    {0,4},{1,5},{2,6},{3,7}
};

void EmitSmoothMC(const std::vector<uint8_t>& solid, const GridSpec& grid,
                  const GaussianSplatCloud *cloud, const std::vector<int>& keep,
                  std::vector<TriRec>& tris)
{
    auto val = [&](int x, int y, int z) -> DBL {
        if (x < 0 || y < 0 || z < 0 || x >= grid.nx || y >= grid.ny || z >= grid.nz)
            return 0.0;
        return solid[static_cast<size_t>(Idx3(x, y, z, grid.nx, grid.ny))] ? 1.0 : 0.0;
    };

    const DBL iso = 0.5;
    const DBL h = grid.voxel;
    for (int iz = 0; iz < grid.nz - 1; ++iz)
        for (int iy = 0; iy < grid.ny - 1; ++iy)
            for (int ix = 0; ix < grid.nx - 1; ++ix)
            {
                DBL gv[8];
                int cubeindex = 0;
                for (int c = 0; c < 8; ++c)
                {
                    gv[c] = val(ix + kMcCorner[c][0], iy + kMcCorner[c][1], iz + kMcCorner[c][2]);
                    if (gv[c] < iso)
                        cubeindex |= (1 << c);
                }
                const int edges = kMcEdgeTable[cubeindex];
                if (edges == 0)
                    continue;

                Vector3d vertlist[12];
                for (int e = 0; e < 12; ++e)
                {
                    if ((edges & (1 << e)) == 0)
                        continue;
                    const int a = kMcEdgeVert[e][0];
                    const int b = kMcEdgeVert[e][1];
                    const DBL va = gv[a];
                    const DBL vb = gv[b];
                    DBL t = 0.5;
                    const DBL den = (vb - va);
                    if (std::fabs(den) > 1.0e-8)
                        t = (iso - va) / den;
                    t = Clamp01Local(t);
                    Vector3d pa(
                        grid.origin[X] + (ix + kMcCorner[a][0]) * h,
                        grid.origin[Y] + (iy + kMcCorner[a][1]) * h,
                        grid.origin[Z] + (iz + kMcCorner[a][2]) * h);
                    Vector3d pb(
                        grid.origin[X] + (ix + kMcCorner[b][0]) * h,
                        grid.origin[Y] + (iy + kMcCorner[b][1]) * h,
                        grid.origin[Z] + (iz + kMcCorner[b][2]) * h);
                    vertlist[e] = pa + (pb - pa) * t;
                }

                for (int i = 0; kMcTriTable[cubeindex][i] != -1; i += 3)
                {
                    TriRec t;
                    t.v0 = vertlist[kMcTriTable[cubeindex][i]];
                    t.v1 = vertlist[kMcTriTable[cubeindex][i + 1]];
                    t.v2 = vertlist[kMcTriTable[cubeindex][i + 2]];
                    Vector3d n = cross(t.v1 - t.v0, t.v2 - t.v0);
                    if (n.lengthSqr() < EPSILON)
                        continue;
                    n.normalize();
                    // Prefer outward (away from denser side): flip if average solid is outside.
                    const Vector3d mid = (t.v0 + t.v1 + t.v2) / 3.0;
                    const Vector3d probe = mid + n * (h * 0.25);
                    const int px = static_cast<int>(std::floor((probe[X] - grid.origin[X]) / h));
                    const int py = static_cast<int>(std::floor((probe[Y] - grid.origin[Y]) / h));
                    const int pz = static_cast<int>(std::floor((probe[Z] - grid.origin[Z]) / h));
                    if (val(px, py, pz) > 0.5)
                        n = -n;
                    t.n = n;
                    SampleColour(cloud, keep, mid, n, h, t.r, t.g, t.b, t.opac);
                    tris.push_back(t);
                }
            }
}

Mesh *BuildMeshFromTris(std::vector<TriRec>& tris, const FINISH *srcFinish, bool iorMode, bool inverseMode)
{
    if (tris.empty())
        return nullptr;

    std::map<int, MeshIndex> texOfColour;
    std::vector<TEXTURE *> textures;
    textures.reserve(256);
    auto texFor = [&](DBL r, DBL g, DBL b, DBL opac) -> MeshIndex {
        // Quantize colour + opacity so IOR/transmit buckets stay bounded.
        const int key = (PackRGB5(r, g, b) << 5) |
                        std::min(31, std::max(0, static_cast<int>(Clamp01Local(opac) * 31.0 + 0.5)));
        auto it = texOfColour.find(key);
        if (it != texOfColour.end())
            return it->second;
        const MeshIndex id = static_cast<MeshIndex>(textures.size());
        textures.push_back(MakeSurfaceTriTexture(r, g, b, opac, srcFinish, iorMode, inverseMode));
        texOfColour[key] = id;
        return id;
    };

    Mesh *mesh = new Mesh();
    const size_t nTri = tris.size();
    const size_t nVert = nTri * 3;
    MeshVector *vertexArray = reinterpret_cast<MeshVector *>(POV_MALLOC(nVert * sizeof(MeshVector), "splat surface verts"));
    MeshVector *normalArray = reinterpret_cast<MeshVector *>(POV_MALLOC((nVert + nTri) * sizeof(MeshVector), "splat surface norms"));
    MeshUVVector *uvArray = reinterpret_cast<MeshUVVector *>(POV_MALLOC(sizeof(MeshUVVector), "splat surface uv"));
    uvArray[0] = MeshUVVector(0.0, 0.0);
    MESH_TRIANGLE *triangleArray = reinterpret_cast<MESH_TRIANGLE *>(POV_MALLOC(nTri * sizeof(MESH_TRIANGLE), "splat surface tris"));

    for (size_t ti = 0; ti < nTri; ++ti)
    {
        const TriRec& t = tris[ti];
        const MeshIndex vbase = static_cast<MeshIndex>(ti * 3);
        vertexArray[vbase + 0] = MeshVector(t.v0);
        vertexArray[vbase + 1] = MeshVector(t.v1);
        vertexArray[vbase + 2] = MeshVector(t.v2);
        normalArray[vbase + 0] = MeshVector(t.n);
        normalArray[vbase + 1] = MeshVector(t.n);
        normalArray[vbase + 2] = MeshVector(t.n);

        MESH_TRIANGLE &tri = triangleArray[ti];
        mesh->Init_Mesh_Triangle(&tri);
        tri.P1 = vbase + 0;
        tri.P2 = vbase + 1;
        tri.P3 = vbase + 2;
        tri.N1 = vbase + 0;
        tri.N2 = vbase + 1;
        tri.N3 = vbase + 2;
        tri.UV1 = tri.UV2 = tri.UV3 = 0;
        tri.Smooth = true;
        tri.Texture = texFor(t.r, t.g, t.b, t.opac);

        Vector3d N;
        mesh->Compute_Mesh_Triangle(&tri, true, t.v0, t.v1, t.v2, N);
        const MeshIndex nFace = static_cast<MeshIndex>(nVert + ti);
        tri.Normal_Ind = nFace;
        normalArray[nFace] = MeshVector(N);
    }

    TEXTURE **textureArray = reinterpret_cast<TEXTURE **>(POV_MALLOC(textures.size() * sizeof(TEXTURE *), "splat surface tex"));
    for (size_t i = 0; i < textures.size(); ++i)
        textureArray[i] = textures[i];

    mesh->Type |= TEXTURED_OBJECT | PATCH_OBJECT;
    mesh->has_inside_vector = false;
    Set_Flag(mesh, MULTITEXTURE_FLAG);
    mesh->Data = reinterpret_cast<MESH_DATA *>(POV_MALLOC(sizeof(MESH_DATA), "splat surface data"));
    mesh->Data->References = 1;
    mesh->Data->Tree = nullptr;
    mesh->Data->Normals = normalArray;
    mesh->Data->Triangles = triangleArray;
    mesh->Data->Vertices = vertexArray;
    mesh->Data->UVCoords = uvArray;
    mesh->Textures = textureArray;
    mesh->Data->Number_Of_Normals = static_cast<MeshIndex>(nVert + nTri);
    mesh->Data->Number_Of_Triangles = static_cast<MeshIndex>(nTri);
    mesh->Data->Number_Of_Vertices = static_cast<MeshIndex>(nVert);
    mesh->Data->Number_Of_UVCoords = 1;
    mesh->Number_Of_Textures = static_cast<MeshIndex>(textures.size());
    mesh->Compute_BBox();
    mesh->Build_Mesh_BBox_Tree();
    return mesh;
}

} // namespace

Mesh *GaussianSplatCloud::BuildSurfaceMesh() const
{
    if (points.empty())
        return nullptr;

    std::vector<int> keep;
    keep.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i)
    {
        const GaussianSplatPoint& sp = points[i];
        const DBL op = Clamp01Local(sp.opacity * opacityScale);
        if (op < surfaceMinOpacity)
            continue;
        if (!std::isfinite(sp.position[X]) || !std::isfinite(sp.position[Y]) || !std::isfinite(sp.position[Z]))
            continue;
        if (!std::isfinite(sp.scale[X]) || !std::isfinite(sp.scale[Y]) || !std::isfinite(sp.scale[Z]))
            continue;
        if (sp.scale[X] <= 0.0 || sp.scale[Y] <= 0.0 || sp.scale[Z] <= 0.0)
            continue;
        keep.push_back(static_cast<int>(i));
    }
    if (keep.empty())
        return nullptr;

    // SuperSplat --filter-floaters [size,op,min]: drop Gaussians that never stamp a solid voxel.
    if (filterFloaters)
    {
        std::vector<int> pre;
        pre.reserve(keep.size());
        for (int id : keep)
        {
            const DBL op = Clamp01Local(points[static_cast<size_t>(id)].opacity * opacityScale);
            if (op >= floaterMinOpacity)
                pre.push_back(id);
        }
        keep.swap(pre);
        if (keep.empty())
            return nullptr;

        GridSpec fgrid;
        if (!ComputeGrid(points, keep, floaterVoxelSize, 2.5, fgrid))
            return nullptr;
        std::vector<uint8_t> fsolid;
        std::vector<uint8_t> contrib;
        StampOccupancy(points, keep, opacityScale, floaterVoxelOpacity, fgrid, fsolid, &contrib);
        std::vector<int> survivors;
        survivors.reserve(keep.size());
        for (size_t i = 0; i < keep.size(); ++i)
            if (contrib[i])
                survivors.push_back(keep[i]);
        keep.swap(survivors);
        if (keep.empty())
            return nullptr;
    }

    GridSpec grid;
    if (!ComputeGrid(points, keep, surfaceVoxelSize, 2.5, grid))
        return nullptr;
    const DBL occThresh = (surfaceVoxelOpacity > 0.0) ? surfaceVoxelOpacity : 0.1;
    std::vector<uint8_t> solid;
    StampOccupancy(points, keep, opacityScale, occThresh, grid, solid, nullptr);

    size_t solidCount = 0;
    for (uint8_t v : solid)
        if (v)
            ++solidCount;
    if (solidCount == 0)
        return nullptr;

    std::vector<TriRec> tris;
    tris.reserve(solidCount * 2);
    if (surfaceSmooth)
        EmitSmoothMC(solid, grid, this, keep, tris);
    else
        EmitFaces(solid, grid, this, keep, tris);

    const FINISH *srcFinish = (Texture != nullptr) ? Texture->Finish : nullptr;
    // inverse_surface: always opaque mesh (ignore interior IOR / glass finish heuristics).
    const bool inverseMode = inverseSurface;
    const bool iorMode = !inverseMode &&
                         ((interior != nullptr && interior->IOR > SNGL(1.0) + EPSILON) ||
                          (srcFinish != nullptr && (srcFinish->Specular > EPSILON || srcFinish->Reflection_Fresnel ||
                                                    srcFinish->Fresnel > EPSILON)));
    return BuildMeshFromTris(tris, srcFinish, iorMode, inverseMode);
}

} // namespace pov
