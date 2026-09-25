//******************************************************************************
///
/// @file core/shape/gaussiansplat.h
///
/// Declarations for an experimental 3D Gaussian Splatting cloud primitive.
///
/// Stores packed Inria/graphdeco-style Gaussians and composites soft splat
/// contributions along a ray (BVH + SH colour + front-to-back alpha), returning
/// a single emission hit for POV-Ray's surface shader. Also exposes
/// IntegrateAlongRay for offline segment integration (primary + bounce/GI).
///
//******************************************************************************

#ifndef POVRAY_CORE_GAUSSIANSPLAT_H
#define POVRAY_CORE_GAUSSIANSPLAT_H

#include "core/configcore.h"
#include "core/scene/object.h"

#include <vector>

namespace pov
{

#define GAUSSIAN_SPLAT_OBJECT (PATCH_OBJECT)

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
        int samples;                 ///< 1 = 3D peak; 2 = Kerbl Jacobian EWA; >=3 = Vol3DGS volume α.
        int maxHits;                 ///< Cap on collected splat hits (0 = adaptive soft cap).
        int bvhLeafSize;             ///< BVH leaf size (1 = quality).
        DBL giWeight;                ///< Scale for bounce/GI segment contribution (Trace hook).
        DBL opacityScale;            ///< Multiplier on splat opacity (offline density control).

        std::vector<GaussianSplatBVHNode> bvh;
        std::vector<int> bvhOrder; ///< Permutation of point indices used by BVH leaves.

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

        /// Build BVH after points/restCoeffs are filled. Call before render.
        void BuildAcceleration();

        /// Evaluate SH colour for view direction (world space, toward camera).
        void EvalColour(const GaussianSplatPoint& sp, const Vector3d& viewDir, Vector3d& rgb) const;

        /// Integrate SH emission + alpha along ray segment [t0,t1] in object/local space.
        /// @param viewDir  Direction toward viewer (typically -dir).
        /// @param Thread   Scratch for hit list (must be non-null).
        /// @param useKerbl When true and Thread has a valid primary-ray projection, samples==2
        ///                 uses Kerbl Σ'=JWΣWᵀJᵀ; otherwise plane-perp billboard EWA.
        bool IntegrateAlongRay(const Vector3d& origin, const Vector3d& dir,
                               DBL t0, DBL t1, const Vector3d& viewDir,
                               GaussianSplatSegmentResult& out, TraceThreadData *Thread,
                               bool useKerbl = false) const;

    private:
        void BuildBVHRecursive(int nodeIndex, int begin, int end, int depth);
        bool RayAABB(const Vector3d& origin, const Vector3d& invDir, const Vector3d& bmin, const Vector3d& bmax,
                     DBL& tNear, DBL& tFar) const;
        bool SplatContribution(const GaussianSplatPoint& sp, const Vector3d& origin, const Vector3d& dir,
                               DBL tSeg0, DBL tSeg1, DBL& tHit, DBL& weight,
                               TraceThreadData *Thread, bool useKerbl) const;
};

}
#endif // POVRAY_CORE_GAUSSIANSPLAT_H
