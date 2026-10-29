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
#include <memory>
#include <string>
#include <vector>

namespace pov
{

class Mesh;
struct GaussianSplatVkFramebuffer;

enum class GaussianSplatRenderBackend
{
    Cpu = 0,
    Vulkan = 1
};

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
    unsigned transformIndex; ///< Per-node Assimp transform, or UINT_MAX.
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
    Vector3d normal;       ///< Alpha/transmittance-weighted relighting normal.
    DBL transmittance;     ///< Remaining transmittance (1 - accumulated alpha).
    DBL depth;             ///< Weighted depth in world ray parameter space (caller scale).
    bool valid;            ///< True if any contribution exceeded cutoff.
};

enum class GaussianSplatLightingMode
{
    Automatic, ///< Legacy behavior for non-KHR clouds.
    Khr,       ///< Full view-dependent SH, emitted without POV material lighting.
    Relight    ///< SH degree 0 pigment with covariance normals and POV finish.
};

class GaussianSplatCloud final : public NonsolidObject
{
    public:
        std::vector<GaussianSplatPoint> points;
        std::vector<TRANSFORM> sourceTransforms; ///< glTF node-global transforms referenced by points.
        std::vector<DBL> restCoeffs; ///< Per-point blocks of size restCount.
        unsigned restCount;          ///< Coeffs per point (0, 9, 24, or 45 typical).
        bool restCoeffsInterleaved;  ///< KHR sidecar: coefficient-major RGB; legacy PLY: channel-major.
        int shDegree;                ///< Max SH degree to evaluate (0..3).
        DBL opacityCutoff;           ///< Skip weights below this.
        DBL alphaStop;               ///< Stop compositing when accumulated alpha exceeds this.
        DBL alphaClamp;              ///< Maximum per-splat alpha (Sample Viewer default 0.99).
        DBL sigmaExtent;             ///< Gaussian support radius in standard deviations (default 3).
        DBL ewaFilter;               ///< Screen-space covariance diagonal regularizer (default 0.3).
        /// Sample-Viewer huge-quad cull: skip if max(sigmaExtent*sqrt(cov_ii)) > frac * min(W,H).
        /// 0 = disabled (legacy); 0.5 = Khronos / glTF Sample Viewer default.
        DBL maxQuadFrac;
        int samples;                 ///< 1 = 3D peak; 2 = project/sort/blend (Kerbl); >=3 = Vol3DGS volume α.
        int maxHits;                 ///< Cap on collected splat hits (0 = adaptive soft cap).
        int bvhLeafSize;             ///< BVH leaf size (1 = quality).
        DBL giWeight;                ///< Scale for bounce/GI segment contribution (Trace hook).
        DBL opacityScale;            ///< Multiplier on splat opacity (offline density control).
        GaussianSplatLightingMode lightingMode;
        /// When true (or auto-detected from scene texture/interior): splat RGB feeds
        /// pigment + POV finish/IOR instead of pure emission billboards.
        bool materialShading;
        /// Drop view rays / pixels that collect only one contributing splat
        /// (typical soft edge / floater fringe). Off = legacy compositing.
        bool cullSingletonHits;
        /// KHR_gaussian_splatting ellipse kernel fidelity:
        /// samples=2 projected path, cameraDistance sort, configurable sigma cutoff/EWA,
        /// float blend before tonemap, Sigma = M C C^T M^T,
        /// no volume refill on primary miss; secondary = orthographic
        /// ray-plane projection of Sigma (no perspective J; spec is camera-primary).
        bool khrEllipse;
        /// Authored KHR meta from Assimp (empty if source had none, e.g. PLY).
        std::string khrKernel;       ///< e.g. "ellipse"
        std::string khrColorSpace;   ///< e.g. "srgb_rec709_display"
        std::string khrProjection;   ///< e.g. "perspective" (default)
        std::string khrSortingMethod; ///< e.g. "cameraDistance" (default)
        /// When true (KHR default / authored): sort by ||mu-cam||; else view-forward depth.
        bool khrSortCameraDistance;
        /// Primary projected path: Cpu (default) or Vulkan compute (falls back to Cpu).
        GaussianSplatRenderBackend renderBackend;
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
            DBL nx, ny, nz; ///< Covariance-derived world normal for relighting.
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
        /// Immutable camera snapshot shared with render threads.
        mutable std::shared_ptr<const GaussianSplatVkFramebuffer> vkFrame;
        mutable std::mutex vkFrameMutex;
        /// Serializes first-time BVH / raySplatData build (render threads race otherwise).
        mutable std::mutex accelMutex;

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

        /// Build BVH after points/restCoeffs are filled. Thread-safe; no-op if already built.
        void BuildAcceleration();

        /// Evaluate SH colour. @p viewDir is camera→splat in the SH/authoring frame
        /// (object space before node Trans; KHR: rotate SH / equivalently R^{-1} view).
        void EvalColour(const GaussianSplatPoint& sp, const Vector3d& viewDir, Vector3d& rgb) const;

        /// Evaluate the view-independent degree-0 SH colour used as relit pigment.
        void EvalDiffuseColour(const GaussianSplatPoint& sp, Vector3d& rgb) const;

        /// Map world-space camera→splat direction into the SH authoring frame (applies R^{-1}).
        void ViewDirForSH(const GaussianSplatPoint& sp, const Vector3d& camToSplatWorld, Vector3d& outLocal) const;

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

        /// Prepare once per camera/size, before tracing pixels. Invalid snapshots
        /// retain failed attempts so render threads use CPU without retrying per pixel.
        std::shared_ptr<const GaussianSplatVkFramebuffer> PrepareVulkanFramebuffer(const GaussianSplatCamBasis& cam) const;

        /// Binary opacity-voxel occupancy (SuperSplat collision defaults) + marching
        /// cubes. Triangle pigments/normals are computed here via EvalColour (not Assimp).
        Mesh *BuildSurfaceMesh() const;

    private:
        struct RaySplatData
        {
            DBL covariance[3][3];
            Vector3d normal;
        };
        std::vector<RaySplatData> raySplatData;

        void BuildBVHRecursive(int nodeIndex, int begin, int end, int depth);
        bool RayAABB(const Vector3d& origin, const Vector3d& invDir, const Vector3d& bmin, const Vector3d& bmax,
                     DBL& tNear, DBL& tFar) const;
        bool SplatContribution(size_t pointIndex, const Vector3d& origin, const Vector3d& dir,
                               DBL tSeg0, DBL tSeg1, DBL& tHit, DBL& weight,
                               TraceThreadData *Thread, bool useKerbl, const Vector3d planeAxes[2]) const;
        void BuildProjectedCache(const GaussianSplatCamBasis& cam) const;
};

}
#endif // POVRAY_CORE_GAUSSIANSPLAT_H
