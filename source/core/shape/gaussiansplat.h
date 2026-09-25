//******************************************************************************
///
/// @file core/shape/gaussiansplat.h
///
/// Declarations for an experimental 3D Gaussian Splatting cloud primitive.
///
/// Stores packed Inria/graphdeco-style Gaussians. Primary path for samples==2:
/// SuperSplat/Kerbl project → tile → depth-sort → 2D EWA blend (CPU). Also
/// supports peak (1) / volume (>=3) along-ray integration for GI/hybrid.
///
//******************************************************************************

#ifndef POVRAY_CORE_GAUSSIANSPLAT_H
#define POVRAY_CORE_GAUSSIANSPLAT_H

#include "core/configcore.h"
#include "core/scene/object.h"

#include <mutex>
#include <vector>

namespace pov
{

#define GAUSSIAN_SPLAT_OBJECT (PATCH_OBJECT)

/// Camera basis for screen-space projection (mirrors TraceThreadData::GaussianSplatProj).
struct GaussianSplatCamBasis final
{
    Vector3d origin, right, up, forward;
    DBL fx, fy;
    DBL screenU, screenV;
    bool camValid;
    bool pixelValid;
    GaussianSplatCamBasis() :
        fx(1), fy(1), screenU(0), screenV(0), camValid(false), pixelValid(false) {}
};

/// One anisotropic Gaussian (decoded scales / opacity; SH rest in side array).
struct GaussianSplatPoint final
{
    Vector3d position;
    Vector3d scale;      ///< Linear (exp'd) per-axis scales.
    DBL qw, qx, qy, qz;  ///< Rotation quaternion (wxyz / rot_0..3).
    DBL opacity;         ///< Already sigmoid'd into [0,1].
    Vector3d dc;         ///< SH degree-0 coeffs (f_dc_*).
    unsigned restOffset; ///< Index into restCoeffs (restCount entries).
};

struct GaussianSplatBVHNode final
{
    Vector3d bmin;
    Vector3d bmax;
    int left;   ///< Child index, or -1 if leaf.
    int right;  ///< Sibling child, or unused for leaf.
    int first;  ///< First splat index (leaf).
    int count;  ///< Splat count (leaf).
};

/// Result of integrating one cloud along a ray segment.
struct GaussianSplatSegmentResult final
{
    Vector3d colour;       ///< Premultiplied emission RGB along the segment.
    DBL transmittance;     ///< Remaining transmittance (1 - accumulated alpha).
    DBL depth;             ///< Weighted depth in world ray parameter space (caller scale).
    bool valid;            ///< True if any contribution exceeded cutoff.
};

class GaussianSplatCloud final : public NonsolidObject
{
    public:
        std::vector<GaussianSplatPoint> points;
        std::vector<DBL> restCoeffs; ///< Per-point blocks of size restCount.
        unsigned restCount;          ///< Coeffs per point (0, 9, 24, or 45 typical).
        int shDegree;                ///< Max SH degree to evaluate (0..3).
        DBL opacityCutoff;           ///< Skip weights below this.
        DBL alphaStop;               ///< Stop compositing when accumulated alpha exceeds this.
        int samples;                 ///< 1 = 3D peak; 2 = project/sort/blend (Kerbl); >=3 = Vol3DGS volume α.
        int maxHits;                 ///< Cap on collected splat hits (0 = adaptive soft cap).
        int bvhLeafSize;             ///< BVH leaf size (1 = quality).
        DBL giWeight;                ///< Scale for bounce/GI segment contribution (Trace hook).
        DBL opacityScale;            ///< Multiplier on splat opacity (offline density control).
        /// When true (or auto-detected from scene texture/interior): splat RGB feeds
        /// pigment + POV finish/IOR instead of pure emission billboards.
        bool materialShading;

        std::vector<GaussianSplatBVHNode> bvh;
        std::vector<int> bvhOrder; ///< Permutation of point indices used by BVH leaves.

        /// One projected Gaussian in screen space (pixels from image centre).
        struct ProjEntry final
        {
            DBL meanU, meanV;
            DBL cov00, cov01, cov11;
            DBL depth;   ///< View depth along camera forward.
            DBL alpha;   ///< Sigmoid opacity (scaled).
            DBL r, g, b; ///< View-dependent SH colour for this camera.
        };

        /// Frame cache: project all splats once, bin into screen tiles (CSR).
        struct ProjCache final
        {
            bool valid;
            Vector3d origin, right, up, forward;
            DBL fx, fy;
            int tileSize;
            int tilesX, tilesY;
            DBL tileOriginU, tileOriginV;
            std::vector<ProjEntry> entries;
            std::vector<int> tileOffsets; ///< tilesX*tilesY + 1
            std::vector<int> tileIndices;
            ProjCache() :
                valid(false), fx(0), fy(0), tileSize(16),
                tilesX(0), tilesY(0), tileOriginU(0), tileOriginV(0) {}
        };
        mutable ProjCache projCache;
        mutable std::mutex projCacheMutex;

        GaussianSplatCloud();
        virtual ~GaussianSplatCloud() override;

        virtual ObjectPtr Copy() override;

        virtual bool All_Intersections(const Ray&, IStack&, TraceThreadData *) override;
        virtual bool Inside(const Vector3d&, TraceThreadData *) const override;
        virtual void Normal(Vector3d&, Intersection *, TraceThreadData *) const override;
        virtual void Translate(const Vector3d&, const TRANSFORM *) override;
        virtual void Rotate(const Vector3d&, const TRANSFORM *) override;
        virtual void Scale(const Vector3d&, const TRANSFORM *) override;
        virtual void Transform(const TRANSFORM *) override;
        virtual void Compute_BBox() override;
        virtual void Determine_Textures(Intersection *, bool, WeightedTextureVector&, TraceThreadData *) override;
        virtual bool IsOpaque() const override;

        /// True if scene texture finish or interior IOR should drive shading.
        bool WantsMaterialShading() const;

        /// Build BVH after points/restCoeffs are filled. Call before render.
        void BuildAcceleration();

        /// Evaluate SH colour. @p viewDir is camera→splat (SuperSplat / Inria convention).
        void EvalColour(const GaussianSplatPoint& sp, const Vector3d& viewDir, Vector3d& rgb) const;

        /// Integrate SH emission + alpha along ray segment [t0,t1] in object/local space.
        /// @param viewDir  Direction toward viewer (typically -dir) — flipped to camera→splat for SH.
        /// @param Thread   Scratch for hit list (must be non-null).
        /// @param useKerbl When true and Thread has a valid primary-ray projection, samples==2
        ///                 uses Kerbl Σ'=JWΣWᵀJᵀ along-ray (legacy); prefer Projected path.
        bool IntegrateAlongRay(const Vector3d& origin, const Vector3d& dir,
                               DBL t0, DBL t1, const Vector3d& viewDir,
                               GaussianSplatSegmentResult& out, TraceThreadData *Thread,
                               bool useKerbl = false) const;

        /// Build/reuse screen-space projection cache for this camera (thread-safe).
        bool EnsureProjectedCache(const GaussianSplatCamBasis& cam) const;

        /// SuperSplat-style pixel: tile lookup → depth sort → 2D EWA front-to-back blend.
        bool CompositeProjectedPixel(DBL screenU, DBL screenV,
                                     GaussianSplatSegmentResult& out, TraceThreadData *Thread) const;

    private:
        void BuildBVHRecursive(int nodeIndex, int begin, int end, int depth);
        bool RayAABB(const Vector3d& origin, const Vector3d& invDir, const Vector3d& bmin, const Vector3d& bmax,
                     DBL& tNear, DBL& tFar) const;
        bool SplatContribution(const GaussianSplatPoint& sp, const Vector3d& origin, const Vector3d& dir,
                               DBL tSeg0, DBL tSeg1, DBL& tHit, DBL& weight,
                               TraceThreadData *Thread, bool useKerbl) const;
        void BuildProjectedCache(const GaussianSplatCamBasis& cam) const;
};

}
#endif // POVRAY_CORE_GAUSSIANSPLAT_H
