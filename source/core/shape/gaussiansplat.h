//******************************************************************************
///
/// @file core/shape/gaussiansplat.h
///
/// Declarations for an experimental 3D Gaussian Splatting cloud primitive.
///
/// Stores packed Inria/graphdeco-style Gaussians and composites soft splat
/// contributions along a ray (BVH + SH colour + front-to-back alpha), returning
/// a single emission hit for POV-Ray's surface shader.
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

class GaussianSplatCloud final : public NonsolidObject
{
    public:
        std::vector<GaussianSplatPoint> points;
        std::vector<DBL> restCoeffs; ///< Per-point blocks of size restCount.
        unsigned restCount;          ///< Coeffs per point (0, 9, 24, or 45 typical).
        int shDegree;                ///< Max SH degree to evaluate (0..3).
        DBL opacityCutoff;           ///< Skip weights below this.
        DBL alphaStop;               ///< Stop compositing when accumulated alpha exceeds this.

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

    private:
        void BuildBVHRecursive(int nodeIndex, int begin, int end, int depth);
        bool RayAABB(const Vector3d& origin, const Vector3d& invDir, const Vector3d& bmin, const Vector3d& bmax,
                     DBL& tNear, DBL& tFar) const;
        bool SplatContribution(const GaussianSplatPoint& sp, const Vector3d& origin, const Vector3d& dir,
                               DBL& tHit, DBL& weight) const;
};

}
#endif // POVRAY_CORE_GAUSSIANSPLAT_H
