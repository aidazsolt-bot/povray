//******************************************************************************
///
/// @file core/shape/gaussiansplat.h
///
/// Declarations for an experimental 3D Gaussian Splatting cloud primitive.
///
/// Stores packed Inria/graphdeco-style Gaussians. Primary path for samples==2
/// or khrEllipse: Kerbl/KHR project → tile → cameraDistance-sort → 2D EWA blend.
/// khrEllipse tightens fidelity to KHR_gaussian_splatting (ellipse kernel) while
/// POV finish/interior IOR remains a separate material/relighting layer.
/// Also supports peak (1) / volume (>=3) along-ray integration for GI/hybrid.
///
//******************************************************************************

#ifndef POVRAY_CORE_GAUSSIANSPLAT_H
#define POVRAY_CORE_GAUSSIANSPLAT_H

#include "core/configcore.h"
#include "core/scene/object.h"

#include <mutex>
#include <string>
#include <vector>

namespace pov
{

class Mesh;

#define GAUSSIAN_SPLAT_OBJECT (PATCH_OBJECT)

/// Camera basis for screen-space projection (mirrors TraceThreadData::GaussianSplatProj).
struct GaussianSplatCamBasis final
{
    Vector3d origin, right, up, forward;
    DBL fx, fy;
    DBL screenU, screenV;
    DBL screenW, screenH; ///< Frame size in pixels (max_quad_frac cull).
    bool camValid;
    bool pixelValid;
    GaussianSplatCamBasis() :
        fx(1), fy(1), screenU(0), screenV(0), screenW(0), screenH(0),
        camValid(false), pixelValid(false) {}
};

/// One anisotropic Gaussian (KHR-native attrs; SH rest in side array).
struct GaussianSplatPoint final
{
    Vector3d position;
    Vector3d scale;      ///< KHR linear non-negative per-axis scales.
    DBL qw, qx, qy, qz;  ///< Unit quaternion in wxyz (from glTF xyzw on import).
    DBL opacity;         ///< KHR linear opacity in [0,1].
    Vector3d dc;         ///< SH degree-0 coeffs (SH_DEGREE_0_COEF_0 / f_dc_*).
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
        /// Sample-Viewer huge-quad cull: skip if max(3*sqrt(cov_ii)) > frac * min(W,H).
        /// 0 = disabled (legacy); 0.5 = Khronos / glTF Sample Viewer default.
        DBL maxQuadFrac;
        int samples;                 ///< 1 = 3D peak; 2 = project/sort/blend (Kerbl); >=3 = Vol3DGS volume α.
        int maxHits;                 ///< Cap on collected splat hits (0 = adaptive soft cap).
        int bvhLeafSize;             ///< BVH leaf size (1 = quality).
        DBL giWeight;                ///< Scale for bounce/GI segment contribution (Trace hook).
        DBL opacityScale;            ///< Multiplier on splat opacity (offline density control).
        /// When true (or auto-detected from scene texture/interior): splat RGB feeds
        /// pigment + POV finish/IOR instead of pure emission billboards.
        bool materialShading;
        /// Drop view rays / pixels that collect only one contributing splat
        /// (typical soft edge / floater fringe). Off = legacy compositing.
        bool cullSingletonHits;
        /// KHR_gaussian_splatting ellipse kernel fidelity:
        /// samples=2 projected path, cameraDistance sort, 3-sigma cutoff,
        /// float blend before tonemap, Sigma = M C C^T M^T, no Kerbl 0.3px
        /// low-pass, no volume refill on primary miss; secondary = orthographic
        /// ray-plane projection of Sigma (no perspective J; spec is camera-primary).
        bool khrEllipse;
        /// Authored KHR meta from Assimp (empty if source had none, e.g. PLY).
        std::string khrKernel;       ///< e.g. "ellipse"
        std::string khrColorSpace;   ///< e.g. "srgb_rec709_display"
        std::string khrProjection;   ///< e.g. "perspective" (default)
        std::string khrSortingMethod; ///< e.g. "cameraDistance" (default)
        /// When true (KHR default / authored): sort by ||mu-cam||; else view-forward depth.
        bool khrSortCameraDistance;
        /// SuperSplat-style opacity voxel surface (as_surface): replace cloud with Mesh.
        bool asSurface;
        /// Inverse-surface extract: opaque MC mesh from occupancy (no volume, no IOR).
        /// Implies asSurface + surfaceSmooth + filterFloaters; strips interior on convert.
        bool inverseSurface;
        /// Optional filesystem path to write extracted mesh as Wavefront OBJ (ASCII).
        std::string exportMeshPath;
        bool surfaceSmooth;       ///< true = marching cubes; false = voxel faces (default).
        bool filterFloaters;      ///< SuperSplat --filter-floaters (default on for as_surface).
        DBL surfaceVoxelSize;     ///< World units; 0 = auto (bbox diagonal / 120). SuperSplat default 0.05.
        DBL surfaceVoxelOpacity;  ///< Occupancy threshold. SuperSplat --voxel-opacity default 0.1.
        DBL surfaceMinOpacity;    ///< Drop Gaussians below this (sigmoid space). SuperSplat 1/255.
        DBL floaterVoxelSize;     ///< SuperSplat filter-floaters size default 0.05.
        DBL floaterVoxelOpacity;  ///< SuperSplat filter-floaters opacity default 0.1.
        DBL floaterMinOpacity;    ///< SuperSplat filter-floaters min default 0.004.

        std::vector<GaussianSplatBVHNode> bvh;
        std::vector<int> bvhOrder; ///< Permutation of point indices used by BVH leaves.

        /// One projected Gaussian in screen space (pixels from image centre).
        struct ProjEntry final
        {
            DBL meanU, meanV;
            DBL cov00, cov01, cov11;
            DBL depth;    ///< View depth along camera forward (ray hit placement).
            DBL camDist;  ///< ||mu - cam|| for KHR cameraDistance sort.
            DBL alpha;    ///< Linear opacity attribute (scaled).
            DBL r, g, b;  ///< View-dependent SH colour for this camera (pre-tonemap).
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

        /// Evaluate SH colour. @p viewDir is camera→splat in the SH/authoring frame
        /// (object space before node Trans; KHR: rotate SH / equivalently R^{-1} view).
        void EvalColour(const GaussianSplatPoint& sp, const Vector3d& viewDir, Vector3d& rgb) const;

        /// Map world-space camera→splat direction into the SH authoring frame (applies R^{-1}).
        void ViewDirForSH(const Vector3d& camToSplatWorld, Vector3d& outLocal) const;

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

        /// Binary opacity-voxel occupancy (SuperSplat collision defaults) + marching
        /// cubes. Triangle pigments/normals are computed here via EvalColour (not Assimp).
        Mesh *BuildSurfaceMesh() const;

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
