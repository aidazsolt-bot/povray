//******************************************************************************
///
/// @file parser/parser_assimp.cpp
///
/// Experimental model import via Assimp (Arris build with Gaussian-splat side channel).
///
/// Syntax:
///   assimp { "file.ext" }                 // mesh and/or Gaussian splats (auto)
///   mesh { assimp "file.ext" }            // triangle mesh only
///
/// Data is taken from Assimp's aiScene / aiMesh / aiMaterial and, when present,
/// aiGetGaussianSplat(). Animation clips are reported (not yet time-sampled).
///
//******************************************************************************

// Unit header file must be the first file included within POV-Ray *.cpp files (pulls in config)
#include "parser/parser.h"

#if POV_PARSER_EXPERIMENTAL_ASSIMP_IMPORT

// C++ standard header files
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// Assimp (Arris) — must match the library linked at build time
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/material.h>
#include <assimp/anim.h>
#include <assimp/gaussian.h>

// POV-Ray header files (base module)
#include "base/fileinputoutput.h"
#include "base/fileutil.h"
#include "base/image/image.h"
#include "base/path.h"
#include "base/pov_err.h"
#include "base/pov_mem.h"
#include "base/stringutilities.h"

// POV-Ray header files (core module)
#include "core/colour/spectral.h"
#include "core/material/normal.h"
#include "core/material/pattern.h"
#include "core/material/texture.h"
#include "core/math/matrix.h"
#include "core/scene/object.h"
#include "core/scene/scenedata.h"
#include "core/shape/csg.h"
#include "core/shape/gaussiansplat.h"
#include "core/shape/mesh.h"
#include "core/shape/sphere.h"
#include "core/support/imageutil.h"

// this must be the last file included
#include "base/povdebug.h"

namespace pov_parser
{

using namespace pov;
using std::max;
using std::min;
using std::shared_ptr;
using std::vector;

namespace
{

constexpr DBL kShC0 = 0.28209479177387814; // 0.5 * Y_0^0

inline DBL Clamp01(DBL v)
{
    return max(0.0, min(1.0, v));
}

inline DBL Sigmoid(DBL x)
{
    return 1.0 / (1.0 + std::exp(-x));
}

bool PathLooksLikePly(const char* path)
{
    if (path == nullptr)
        return false;
    const char* dot = std::strrchr(path, '.');
    if (dot == nullptr)
        return false;
    return (std::strcmp(dot, ".ply") == 0) || (std::strcmp(dot, ".PLY") == 0);
}

void QuaternionToMatrix(DBL w, DBL x, DBL y, DBL z, MATRIX out)
{
    DBL n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n < EPSILON)
    {
        w = 1.0;
        x = y = z = 0.0;
    }
    else
    {
        w /= n;
        x /= n;
        y /= n;
        z /= n;
    }

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

TEXTURE *MakeColourTexture(DBL r, DBL g, DBL b, DBL transmit = 0.0, DBL ambient = 0.2)
{
    TEXTURE *tex = Create_Texture();
    Destroy_Pigment(tex->Pigment);
    tex->Pigment = Create_Pigment();
    if (tex->Finish == nullptr)
        tex->Finish = Create_Finish();

    RGBColour rgb(Clamp01(r), Clamp01(g), Clamp01(b));
    MathColour mc = ToMathColour(rgb);
    tex->Pigment->colour = TransColour(mc, 0.0, Clamp01(transmit));
    tex->Finish->Ambient = MathColour(ambient);
    Post_Textures(tex);
    return tex;
}

TEXTURE *MakeSplatTexture(DBL r, DBL g, DBL b, DBL opacity)
{
    TEXTURE *tex = MakeColourTexture(r, g, b, Clamp01(1.0 - opacity), 0.9);
    RGBColour rgb(Clamp01(r), Clamp01(g), Clamp01(b));
    tex->Finish->Emission = ToMathColour(rgb) * opacity;
    Post_Textures(tex);
    return tex;
}

std::string DirnameOf(const std::string &path)
{
    const size_t p = path.find_last_of("/\\");
    if (p == std::string::npos)
        return ".";
    if (p == 0)
        return "/";
    return path.substr(0, p);
}

std::string BasenameOf(const std::string &path)
{
    const size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

std::string StemOf(const std::string &path)
{
    const std::string base = BasenameOf(path);
    const size_t d = base.find_last_of('.');
    return (d == std::string::npos) ? base : base.substr(0, d);
}

bool FileReadable(const std::string &path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    return in.good();
}

/// Resolve Assimp texture filename to an on-disk path (model dir + extension fallbacks).
std::string ResolveAssimpTextureFile(Parser *parser, const std::string &modelDir, const char *texPath)
{
    if (texPath == nullptr || texPath[0] == '\0')
        return std::string();

    std::string raw(texPath);
    // Embedded textures (*0, *1, ...) not supported yet.
    if (raw[0] == '*')
        return std::string();

    const std::string baseName = BasenameOf(raw);
    const std::string stem = StemOf(baseName);

    static const char *kExts[] = { "", ".jpg", ".jpeg", ".png", ".tga", ".bmp", ".JPG", ".PNG", ".TGA", nullptr };

    std::vector<std::string> candidates;
    auto add = [&](const std::string &p) {
        if (!p.empty())
            candidates.push_back(p);
    };

    // Absolute / as-given path first
    add(raw);
    add(modelDir + "/" + raw);
    add(modelDir + "/" + baseName);

    // Extension swaps on basename in model directory (Assimp often says .tga, assets are .jpg)
    for (const char **e = kExts; *e != nullptr; ++e)
    {
        if ((*e)[0] == '\0')
            continue;
        add(modelDir + "/" + stem + *e);
    }

    for (const std::string &cand : candidates)
    {
        if (FileReadable(cand))
            return cand;
    }
    return std::string();
}

int FiletypeFromPath(const std::string &path)
{
    // Follow symlinks so foo.tga -> foo.jpg is detected as JPEG.
    std::string use = path;
#if defined(_POSIX_VERSION) || defined(__unix__) || defined(__APPLE__)
    char realBuf[4096];
    if (realpath(path.c_str(), realBuf) != nullptr)
        use = realBuf;
#endif
    UCS2String ext = GetFileExtension(Path(SysToUCS2String(use.c_str())));
    if (ext.empty())
        return JPEG_FILE; // BobLamp-style fallback
    const int mask = gFile_Type_To_Mask[InferFileTypeFromExt(ext)];
    return (mask != NO_FILE) ? mask : JPEG_FILE;
}

ImageData *LoadAssimpImageData(Parser *parser, const std::string &imagePath, bool gammaCorrect)
{
    ImageData *image = Create_Image();
    ImageReadOptions options;
    options.gammacorrect = gammaCorrect;
    if (gammaCorrect && parser->sceneData->workingGamma)
        options.workingGamma = SimpleGammaCurvePtr(parser->sceneData->workingGamma);

    const int filetype = FiletypeFromPath(imagePath);
    try
    {
        image->data = parser->Read_Image(filetype, SysToUCS2String(imagePath.c_str()).c_str(), options);
    }
    catch (...)
    {
        Destroy_Image(image);
        throw;
    }
    if (image->data == nullptr)
    {
        Destroy_Image(image);
        parser->Error("Cannot read Assimp texture image '%s'.", imagePath.c_str());
    }

    image->Use = USE_COLOUR;
    image->Map_Type = PLANAR_MAP;
    image->iwidth = image->data->GetWidth();
    image->iheight = image->data->GetHeight();
    image->width = static_cast<SNGL>(image->iwidth);
    image->height = static_cast<SNGL>(image->iheight);
    return image;
}

TEXTURE *MakeImageMappedTexture(Parser *parser, const std::string &imagePath, DBL ambient)
{
    ImageData *image = LoadAssimpImageData(parser, imagePath, true);

    TEXTURE *tex = Create_Texture();
    Destroy_Pigment(tex->Pigment);
    tex->Pigment = Create_Pigment();
    tex->Pigment->Type = IMAGE_MAP_PATTERN;
    tex->Pigment->pattern = PatternPtr(new ColourImagePattern());
    if (ImagePatternImpl *pat = dynamic_cast<ImagePatternImpl *>(tex->Pigment->pattern.get()))
        pat->pImage = image;
    else
        parser->Error("Internal error creating Assimp image_map pigment.");

    if (tex->Finish == nullptr)
        tex->Finish = Create_Finish();
    tex->Finish->Ambient = MathColour(ambient);
    tex->Finish->Diffuse = 0.7f;
    Post_Textures(tex);
    return tex;
}

void AttachAssimpBumpMap(Parser *parser, TEXTURE *tex, const std::string &imagePath, DBL amount)
{
    ImageData *image = LoadAssimpImageData(parser, imagePath, false);
    TNORMAL *tn = Create_Tnormal();
    tn->Type = BITMAP_PATTERN;
    tn->Amount = static_cast<SNGL>(amount);
    shared_ptr<ImagePattern> pattern(new ImagePattern());
    pattern->waveFrequency = 0.0;
    tn->pattern = pattern;
    if (ImagePatternImpl *pat = dynamic_cast<ImagePatternImpl *>(tn->pattern.get()))
        pat->pImage = image;
    else
        parser->Error("Internal error creating Assimp bump_map.");
    tex->Tnormal = tn;
}

std::string TryGetAssimpTexture(Parser *parser, const aiMaterial *mat, const std::string &modelDir,
                                aiTextureType type, const char *label, bool &usedImageMap)
{
    if (mat == nullptr)
        return std::string();
    aiString texPath;
    if (mat->GetTexture(type, 0, &texPath) != AI_SUCCESS || texPath.length == 0)
        return std::string();
    const std::string resolved = ResolveAssimpTextureFile(parser, modelDir, texPath.C_Str());
    if (resolved.empty())
    {
        parser->Warning("Assimp %s map '%s' not found next to model (dir '%s').",
                        label, texPath.C_Str(), modelDir.c_str());
        return std::string();
    }
    parser->Warning("Assimp %s map '%s' -> '%s'.", label, texPath.C_Str(), resolved.c_str());
    usedImageMap = true;
    return resolved;
}

TEXTURE *MakeTextureFromAssimpMaterial(Parser *parser, const aiMaterial *mat, const std::string &modelDir,
                                       bool &usedImageMap)
{
    aiColor3D kd(0.8f, 0.8f, 0.8f);
    aiColor3D ka(0.1f, 0.1f, 0.1f);
    aiColor3D ks(0.0f, 0.0f, 0.0f);
    float opacity = 1.0f;
    float shininess = 0.0f;
    if (mat != nullptr)
    {
        mat->Get(AI_MATKEY_COLOR_DIFFUSE, kd);
        mat->Get(AI_MATKEY_COLOR_AMBIENT, ka);
        mat->Get(AI_MATKEY_COLOR_SPECULAR, ks);
        mat->Get(AI_MATKEY_OPACITY, opacity);
        mat->Get(AI_MATKEY_SHININESS, shininess);
    }
    DBL ambient = 0.15 + 0.35 * (0.2126 * ka.r + 0.7152 * ka.g + 0.0722 * ka.b);
    ambient = Clamp01(ambient);

    TEXTURE *tex = nullptr;
    std::string diffusePath = TryGetAssimpTexture(parser, mat, modelDir, aiTextureType_DIFFUSE, "diffuse", usedImageMap);
    if (diffusePath.empty())
        diffusePath = TryGetAssimpTexture(parser, mat, modelDir, aiTextureType_BASE_COLOR, "base_color", usedImageMap);

    if (!diffusePath.empty())
        tex = MakeImageMappedTexture(parser, diffusePath, ambient);
    else
        tex = MakeColourTexture(kd.r, kd.g, kd.b, Clamp01(1.0 - opacity), ambient);

    if (tex->Finish == nullptr)
        tex->Finish = Create_Finish();

    const DBL specAmt = 0.2126 * ks.r + 0.7152 * ks.g + 0.0722 * ks.b;
    if (specAmt > 0.01)
    {
        tex->Finish->Specular = static_cast<SNGL>(Clamp01(specAmt));
        if (shininess > 1.0f)
            tex->Finish->Roughness = static_cast<SNGL>(1.0 / (1.0 + shininess / 10.0));
    }

    // Extra maps Assimp exposes (specular image used as notice; bump/height applied)
    std::string unusedFlag = std::string();
    bool mapFlag = false;
    const std::string specularMap = TryGetAssimpTexture(parser, mat, modelDir, aiTextureType_SPECULAR, "specular", mapFlag);
    if (!specularMap.empty())
        parser->Warning("Assimp specular map applied as finish specular from Ks (image map not layered yet): '%s'.",
                        specularMap.c_str());
    usedImageMap = usedImageMap || mapFlag;

    mapFlag = false;
    std::string bumpPath = TryGetAssimpTexture(parser, mat, modelDir, aiTextureType_HEIGHT, "height/bump", mapFlag);
    if (bumpPath.empty())
        bumpPath = TryGetAssimpTexture(parser, mat, modelDir, aiTextureType_DISPLACEMENT, "displacement", mapFlag);
    if (!bumpPath.empty())
        AttachAssimpBumpMap(parser, tex, bumpPath, 0.35);
    usedImageMap = usedImageMap || mapFlag;

    mapFlag = false;
    const std::string normalPath = TryGetAssimpTexture(parser, mat, modelDir, aiTextureType_NORMALS, "normals", mapFlag);
    if (normalPath.empty())
        TryGetAssimpTexture(parser, mat, modelDir, aiTextureType_NORMAL_CAMERA, "normals_camera", mapFlag);
    if (!normalPath.empty() && tex->Tnormal == nullptr)
    {
        // Approximate normal maps via bump_map (true tangent-space normals not wired yet).
        AttachAssimpBumpMap(parser, tex, normalPath, 0.25);
        parser->Warning("Assimp normal map approximated as bump_map: '%s'.", normalPath.c_str());
    }
    else if (!normalPath.empty())
        parser->Warning("Assimp normal map '%s' noted (height/bump already attached).", normalPath.c_str());
    usedImageMap = usedImageMap || mapFlag;

    Post_Textures(tex);
    return tex;
}

std::string ResolveAssimpPath(Parser *parser, UCS2 *fileName)
{
    UCS2String formal(fileName);
    UCS2String found;
    shared_ptr<IStream> stream = parser->Locate_File(formal, POV_File_Text_User, found, false);
    if (stream != nullptr && !found.empty())
        return UCS2toSysString(found);
    // Fall back to the literal path (Assimp resolves relative to process cwd).
    return UCS2toSysString(fileName);
}

unsigned AssimpPostProcessFlags(const char *sysPath, bool preTransformVertices)
{
    unsigned flags =
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_SortByPType;
    if (preTransformVertices)
        flags |= aiProcess_PreTransformVertices;
    // JoinIdenticalVertices can scramble 3DGS ↔ vertex indexing on PLY (Arris / RenderAssimp).
    if (!PathLooksLikePly(sysPath))
        flags |= aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality;
    return flags;
}

void ReportAnimations(Parser *parser, const aiScene *scene)
{
    if (scene == nullptr || scene->mNumAnimations == 0)
        return;

    parser->Warning("Assimp scene contains %u animation clip(s); currently imported at bind pose (time sampling not yet applied).",
                    scene->mNumAnimations);
    const unsigned limit = min(scene->mNumAnimations, 8u);
    for (unsigned i = 0; i < limit; ++i)
    {
        const aiAnimation *anim = scene->mAnimations[i];
        if (anim == nullptr)
            continue;
        const char *name = (anim->mName.length > 0) ? anim->mName.C_Str() : "(unnamed)";
        parser->Warning("  anim[%u] '%s': duration=%.3f ticks, ticks/s=%.3f, channels=%u",
                        i, name, anim->mDuration, anim->mTicksPerSecond, anim->mNumChannels);
    }
}

void ReportAssimpBounds(Parser *parser, const aiScene *scene)
{
    if (scene == nullptr || scene->mNumMeshes == 0)
        return;

    DBL mn[3] = { HUGE_VAL, HUGE_VAL, HUGE_VAL };
    DBL mx[3] = { -HUGE_VAL, -HUGE_VAL, -HUGE_VAL };
    bool any = false;
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        const aiMesh *mesh = scene->mMeshes[mi];
        if (mesh == nullptr || mesh->mVertices == nullptr)
            continue;
        for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi)
        {
            const aiVector3D &p = mesh->mVertices[vi];
            mn[0] = min(mn[0], static_cast<DBL>(p.x));
            mn[1] = min(mn[1], static_cast<DBL>(p.y));
            mn[2] = min(mn[2], static_cast<DBL>(p.z));
            mx[0] = max(mx[0], static_cast<DBL>(p.x));
            mx[1] = max(mx[1], static_cast<DBL>(p.y));
            mx[2] = max(mx[2], static_cast<DBL>(p.z));
            any = true;
        }
    }
    if (!any)
        return;

    const DBL cx = 0.5 * (mn[0] + mx[0]);
    const DBL cy = 0.5 * (mn[1] + mx[1]);
    const DBL cz = 0.5 * (mn[2] + mx[2]);
    const DBL ex = mx[0] - mn[0], ey = mx[1] - mn[1], ez = mx[2] - mn[2];
    const DBL r = 0.5 * std::sqrt(ex * ex + ey * ey + ez * ez);
    parser->Warning("Assimp AABB min=<%.4f,%.4f,%.4f> max=<%.4f,%.4f,%.4f> center=<%.4f,%.4f,%.4f> radius=%.4f",
                    mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], cx, cy, cz, r);
}

ObjectPtr BuildGaussianSplatCloudFromAssimp(Parser *parser, const aiScene *scene, DBL sphereScale, size_t maxCount, int shDegree)
{
    GaussianSplatCloud *cloud = new GaussianSplatCloud();
    cloud->shDegree = shDegree;
    size_t remaining = (maxCount > 0) ? maxCount : static_cast<size_t>(-1);

    for (unsigned mi = 0; mi < scene->mNumMeshes && remaining > 0; ++mi)
    {
        const aiMesh *mesh = scene->mMeshes[mi];
        const aiGaussianSplat *gs = aiGetGaussianSplat(scene, mi);
        if (mesh == nullptr || gs == nullptr || gs->mDC == nullptr || gs->mScale == nullptr || gs->mOpacity == nullptr)
            continue;

        if (cloud->restCount == 0 && gs->mNumRestCoeffs > 0)
            cloud->restCount = gs->mNumRestCoeffs;

        const unsigned n = min(mesh->mNumVertices, gs->mNumPoints);
        for (unsigned vi = 0; vi < n && remaining > 0; ++vi)
        {
            const aiVector3D &pos = mesh->mVertices[vi];
            const aiVector3D &dc = gs->mDC[vi];
            DBL opacity = Sigmoid(static_cast<DBL>(gs->mOpacity[vi]));
            if (opacity < 0.01)
                continue;

            const aiVector3D &scl = gs->mScale[vi];
            DBL sx = std::exp(static_cast<DBL>(scl.x)) * sphereScale;
            DBL sy = std::exp(static_cast<DBL>(scl.y)) * sphereScale;
            DBL sz = std::exp(static_cast<DBL>(scl.z)) * sphereScale;
            sx = max(sx, EPSILON);
            sy = max(sy, EPSILON);
            sz = max(sz, EPSILON);

            DBL qw = 1.0, qx = 0.0, qy = 0.0, qz = 0.0;
            if (gs->mRotation != nullptr)
            {
                const aiColor4D &q = gs->mRotation[vi];
                qw = q.r; qx = q.g; qy = q.b; qz = q.a;
            }

            GaussianSplatPoint sp;
            sp.position = Vector3d(pos.x, pos.y, pos.z);
            sp.scale = Vector3d(sx, sy, sz);
            sp.qw = qw; sp.qx = qx; sp.qy = qy; sp.qz = qz;
            sp.opacity = opacity;
            sp.dc = Vector3d(dc.x, dc.y, dc.z);
            sp.restOffset = static_cast<unsigned>(cloud->restCoeffs.size());

            if (cloud->restCount > 0 && gs->mRest != nullptr)
            {
                for (unsigned k = 0; k < cloud->restCount; ++k)
                    cloud->restCoeffs.push_back(static_cast<DBL>(gs->mRest[vi * gs->mNumRestCoeffs + k]));
            }
            else
            {
                sp.restOffset = 0;
            }

            cloud->points.push_back(sp);
            --remaining;
        }
    }

    if (cloud->points.empty())
    {
        delete cloud;
        parser->Error("Assimp Gaussian splat import produced no usable points.");
    }

    cloud->BuildAcceleration();
    cloud->Compute_BBox();
    parser->Warning("Imported %lu Gaussian splat(s) via Assimp as GaussianSplatCloud (SH degree <= %d, rest=%u).",
                    static_cast<unsigned long>(cloud->points.size()), cloud->shDegree, cloud->restCount);
    return reinterpret_cast<ObjectPtr>(cloud);
}

ObjectPtr BuildGaussianSplatFromAssimp(Parser *parser, const aiScene *scene, DBL sphereScale, size_t maxCount, bool approximate, int shDegree)
{
    if (!approximate)
        return BuildGaussianSplatCloudFromAssimp(parser, scene, sphereScale, maxCount, shDegree);

    CSG *root = new CSGMerge();
    size_t created = 0;
    size_t remaining = (maxCount > 0) ? maxCount : static_cast<size_t>(-1);

    for (unsigned mi = 0; mi < scene->mNumMeshes && remaining > 0; ++mi)
    {
        const aiMesh *mesh = scene->mMeshes[mi];
        const aiGaussianSplat *gs = aiGetGaussianSplat(scene, mi);
        if (mesh == nullptr || gs == nullptr || gs->mDC == nullptr || gs->mScale == nullptr || gs->mOpacity == nullptr)
            continue;

        const unsigned n = min(mesh->mNumVertices, gs->mNumPoints);
        for (unsigned vi = 0; vi < n && remaining > 0; ++vi)
        {
            const aiVector3D &pos = mesh->mVertices[vi];
            const aiVector3D &dc = gs->mDC[vi];
            DBL r = 0.5 + kShC0 * dc.x;
            DBL g = 0.5 + kShC0 * dc.y;
            DBL b = 0.5 + kShC0 * dc.z;
            DBL opacity = Sigmoid(static_cast<DBL>(gs->mOpacity[vi]));
            if (opacity < 0.01)
                continue;

            const aiVector3D &scl = gs->mScale[vi];
            DBL sx = std::exp(static_cast<DBL>(scl.x)) * sphereScale;
            DBL sy = std::exp(static_cast<DBL>(scl.y)) * sphereScale;
            DBL sz = std::exp(static_cast<DBL>(scl.z)) * sphereScale;
            sx = max(sx, EPSILON);
            sy = max(sy, EPSILON);
            sz = max(sz, EPSILON);

            DBL qw = 1.0, qx = 0.0, qy = 0.0, qz = 0.0;
            if (gs->mRotation != nullptr)
            {
                const aiColor4D &q = gs->mRotation[vi];
                qw = q.r;
                qx = q.g;
                qy = q.b;
                qz = q.a;
            }

            Sphere *sph = new Sphere();
            sph->Center = Vector3d(0.0, 0.0, 0.0);
            sph->Radius = 1.0;
            sph->Texture = MakeSplatTexture(r, g, b, opacity);
            sph->Type |= TEXTURED_OBJECT;

            TRANSFORM scaleT, rotT, transT;
            Compute_Scaling_Transform(&scaleT, Vector3d(sx, sy, sz));

            MATRIX rotM;
            QuaternionToMatrix(qw, qx, qy, qz, rotM);
            Compute_Matrix_Transform(&rotT, rotM);
            Compose_Transforms(&scaleT, &rotT);

            Compute_Translation_Transform(&transT, Vector3d(pos.x, pos.y, pos.z));
            Compose_Transforms(&scaleT, &transT);

            Transform_Object(reinterpret_cast<ObjectPtr>(sph), &scaleT);
            sph->Compute_BBox();
            sph->Type |= IS_CHILD_OBJECT;
            root->children.push_back(reinterpret_cast<ObjectPtr>(sph));
            ++created;
            --remaining;
        }
    }

    if (created == 0)
    {
        delete root;
        parser->Error("Assimp Gaussian splat import produced no usable points.");
    }

    root->Compute_BBox();
    parser->Warning("Imported %lu Gaussian splat(s) via Assimp as CSG merge of ellipsoidal spheres (approximate).",
                    static_cast<unsigned long>(created));
    return reinterpret_cast<ObjectPtr>(root);
}

void BuildMeshFromAssimp(Parser *parser, Mesh *outMesh, const aiScene *scene, const std::string &modelDir)
{
    vector<Vector3d> vertexList;
    vector<Vector3d> normalList;
    vector<Vector2d> uvList;
    vector<TEXTURE *> textures;
    struct Tri
    {
        MeshIndex p1, p2, p3;
        MeshIndex n1, n2, n3;
        MeshIndex uv1, uv2, uv3;
        int tex;
        bool smooth;
    };
    vector<Tri> tris;
    bool usedImageMap = false;

    textures.reserve(scene->mNumMaterials);
    for (unsigned mi = 0; mi < scene->mNumMaterials; ++mi)
    {
        bool img = false;
        textures.push_back(MakeTextureFromAssimpMaterial(parser, scene->mMaterials[mi], modelDir, img));
        usedImageMap = usedImageMap || img;
    }

    if (textures.empty())
        textures.push_back(MakeColourTexture(0.8, 0.8, 0.8));

    uvList.push_back(Vector2d(0.0, 0.0)); // fallback UV index 0

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        const aiMesh *mesh = scene->mMeshes[mi];
        if (mesh == nullptr || mesh->mNumVertices == 0)
            continue;
        // Skip Gaussian splat point clouds (faces are 1-index stubs).
        if (aiGetGaussianSplat(scene, mi) != nullptr)
            continue;
        if ((mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0 && mesh->mNumFaces == 0)
            continue;

        const MeshIndex vBase = static_cast<MeshIndex>(vertexList.size());
        const MeshIndex nBase = static_cast<MeshIndex>(normalList.size());
        const MeshIndex uvBase = static_cast<MeshIndex>(uvList.size());
        const bool hasN = mesh->HasNormals();
        const bool hasUV = mesh->HasTextureCoords(0);
        const int texId = (mesh->mMaterialIndex < textures.size())
                              ? static_cast<int>(mesh->mMaterialIndex)
                              : 0;

        for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi)
        {
            const aiVector3D &p = mesh->mVertices[vi];
            vertexList.emplace_back(p.x, p.y, p.z);
            if (hasN)
            {
                const aiVector3D &n = mesh->mNormals[vi];
                Vector3d nv(n.x, n.y, n.z);
                if (nv.lengthSqr() < EPSILON * EPSILON)
                    nv = Vector3d(0.0, 1.0, 0.0);
                normalList.push_back(nv);
            }
            if (hasUV)
            {
                const aiVector3D &t = mesh->mTextureCoords[0][vi];
                uvList.emplace_back(t.x, t.y);
            }
        }

        for (unsigned fi = 0; fi < mesh->mNumFaces; ++fi)
        {
            const aiFace &face = mesh->mFaces[fi];
            if (face.mNumIndices < 3)
                continue;
            // Fan triangulation for any leftover polygons
            for (unsigned t = 1; t + 1 < face.mNumIndices; ++t)
            {
                Tri tri{};
                const unsigned i0 = face.mIndices[0];
                const unsigned i1 = face.mIndices[t];
                const unsigned i2 = face.mIndices[t + 1];
                tri.p1 = vBase + static_cast<MeshIndex>(i0);
                tri.p2 = vBase + static_cast<MeshIndex>(i1);
                tri.p3 = vBase + static_cast<MeshIndex>(i2);
                tri.smooth = hasN;
                if (hasN)
                {
                    tri.n1 = nBase + static_cast<MeshIndex>(i0);
                    tri.n2 = nBase + static_cast<MeshIndex>(i1);
                    tri.n3 = nBase + static_cast<MeshIndex>(i2);
                }
                if (hasUV)
                {
                    tri.uv1 = uvBase + static_cast<MeshIndex>(i0);
                    tri.uv2 = uvBase + static_cast<MeshIndex>(i1);
                    tri.uv3 = uvBase + static_cast<MeshIndex>(i2);
                }
                else
                {
                    tri.uv1 = tri.uv2 = tri.uv3 = 0;
                }
                tri.tex = texId;
                tris.push_back(tri);
            }
        }
    }

    if (vertexList.empty() || tris.empty())
        parser->Error("Assimp import found no triangle mesh data.");

    MeshVector *vertexArray = reinterpret_cast<MeshVector *>(POV_MALLOC(vertexList.size() * sizeof(MeshVector), "assimp mesh"));
    for (size_t i = 0; i < vertexList.size(); ++i)
        vertexArray[i] = MeshVector(vertexList[i]);

    MeshVector *normalArray = reinterpret_cast<MeshVector *>(
        POV_MALLOC((normalList.size() + tris.size()) * sizeof(MeshVector), "assimp mesh"));
    for (size_t i = 0; i < normalList.size(); ++i)
        normalArray[i] = MeshVector(normalList[i]);

    MeshUVVector *uvArray = reinterpret_cast<MeshUVVector *>(POV_MALLOC(uvList.size() * sizeof(MeshUVVector), "assimp mesh"));
    for (size_t i = 0; i < uvList.size(); ++i)
        uvArray[i] = MeshUVVector(uvList[i]);

    TEXTURE **textureArray = reinterpret_cast<TEXTURE **>(POV_MALLOC(textures.size() * sizeof(TEXTURE *), "assimp mesh"));
    for (size_t i = 0; i < textures.size(); ++i)
        textureArray[i] = textures[i];

    MESH_TRIANGLE *triangleArray = reinterpret_cast<MESH_TRIANGLE *>(POV_MALLOC(tris.size() * sizeof(MESH_TRIANGLE), "assimp mesh"));
    for (size_t i = 0, j = normalList.size(); i < tris.size(); ++i, ++j)
    {
        const Tri &src = tris[i];
        MESH_TRIANGLE &triangle = triangleArray[i];
        outMesh->Init_Mesh_Triangle(&triangle);
        triangle.P1 = src.p1;
        triangle.P2 = src.p2;
        triangle.P3 = src.p3;
        triangle.Texture = src.tex;
        triangle.UV1 = src.uv1;
        triangle.UV2 = src.uv2;
        triangle.UV3 = src.uv3;
        triangle.Smooth = src.smooth;
        if (src.smooth)
        {
            triangle.N1 = src.n1;
            triangle.N2 = src.n2;
            triangle.N3 = src.n3;
        }

        Vector3d P1(vertexList[triangle.P1]);
        Vector3d P2(vertexList[triangle.P2]);
        Vector3d P3(vertexList[triangle.P3]);
        Vector3d N;
        outMesh->Compute_Mesh_Triangle(&triangle, triangle.Smooth, P1, P2, P3, N);
        triangle.Normal_Ind = static_cast<MeshIndex>(j);
        normalArray[j] = MeshVector(N);
    }

    outMesh->Type |= TEXTURED_OBJECT | PATCH_OBJECT;
    outMesh->has_inside_vector = false;
    if (usedImageMap)
        Set_Flag(outMesh, UV_FLAG);

    outMesh->Data = reinterpret_cast<MESH_DATA *>(POV_MALLOC(sizeof(MESH_DATA), "assimp mesh"));
    outMesh->Data->References = 1;
    outMesh->Data->Tree = nullptr;
    outMesh->Data->Normals = normalArray;
    outMesh->Data->Triangles = triangleArray;
    outMesh->Data->Vertices = vertexArray;
    outMesh->Data->UVCoords = uvArray;
    outMesh->Textures = textureArray;

    outMesh->Data->Number_Of_Normals = static_cast<MeshIndex>(normalList.size() + tris.size());
    outMesh->Data->Number_Of_Triangles = static_cast<MeshIndex>(tris.size());
    outMesh->Data->Number_Of_Vertices = static_cast<MeshIndex>(vertexList.size());
    outMesh->Data->Number_Of_UVCoords = static_cast<MeshIndex>(uvList.size());
    outMesh->Number_Of_Textures = static_cast<MeshIndex>(textures.size());
    Set_Flag(outMesh, MULTITEXTURE_FLAG);

    parser->Warning("Imported Assimp mesh: %lu vertices, %lu triangles, %lu material(s).",
                    static_cast<unsigned long>(vertexList.size()),
                    static_cast<unsigned long>(tris.size()),
                    static_cast<unsigned long>(textures.size()));
}

const aiScene *ImportAssimpScene(Parser *parser, Assimp::Importer &importer, const std::string &sysPath)
{
    // First pass without PreTransformVertices so animation clips remain available for reporting.
    // (aiProcess_PreTransformVertices bakes bind pose and drops mAnimations.)
    const aiScene *probe = importer.ReadFile(sysPath, AssimpPostProcessFlags(sysPath.c_str(), false));
    if (probe == nullptr || (!probe->HasMeshes() && probe->mNumMeshes == 0))
    {
        const char *err = importer.GetErrorString();
        parser->Error("Assimp failed to import '%s': %s", sysPath.c_str(), (err && err[0]) ? err : "unknown error");
    }
    ReportAnimations(parser, probe);

    // Second pass: bake node transforms into mesh data (bind pose) for POV geometry.
    const aiScene *scene = importer.ReadFile(sysPath, AssimpPostProcessFlags(sysPath.c_str(), true));
    if (scene == nullptr || (!scene->HasMeshes() && scene->mNumMeshes == 0))
    {
        const char *err = importer.GetErrorString();
        parser->Error("Assimp failed to import '%s' (pre-transform pass): %s",
                      sysPath.c_str(), (err && err[0]) ? err : "unknown error");
    }
    ReportAssimpBounds(parser, scene);
    return scene;
}

ObjectPtr BuildAssimpObject(Parser *parser, const std::string &sysPath, DBL sphereScale, size_t maxCount,
                            bool meshOnly, bool approximate, int shDegree)
{
    Assimp::Importer importer;
    const aiScene *scene = ImportAssimpScene(parser, importer, sysPath);

    const bool hasSplat = aiSceneHasGaussianSplat(scene) != 0;
    bool hasTris = false;
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        if (aiGetGaussianSplat(scene, mi) != nullptr)
            continue;
        const aiMesh *m = scene->mMeshes[mi];
        if (m != nullptr && m->mNumFaces > 0 && (m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) != 0)
        {
            hasTris = true;
            break;
        }
        // Some loaders leave PrimitiveTypes unset; face index count >= 3 is enough.
        if (m != nullptr && m->mNumFaces > 0)
        {
            for (unsigned fi = 0; fi < m->mNumFaces; ++fi)
            {
                if (m->mFaces[fi].mNumIndices >= 3)
                {
                    hasTris = true;
                    break;
                }
            }
        }
        if (hasTris)
            break;
    }

    if (meshOnly)
    {
        if (!hasTris)
            parser->Error("Assimp mesh { assimp ... } requires triangle mesh data (Gaussian-only scenes need top-level assimp { }).");
        Mesh *mesh = new Mesh();
        BuildMeshFromAssimp(parser, mesh, scene, DirnameOf(sysPath));
        mesh->Compute_BBox();
        mesh->Build_Mesh_BBox_Tree();
        return reinterpret_cast<ObjectPtr>(mesh);
    }

    ObjectPtr splatObj = nullptr;
    ObjectPtr meshObj = nullptr;

    if (hasSplat)
        splatObj = BuildGaussianSplatFromAssimp(parser, scene, sphereScale, maxCount, approximate, shDegree);

    if (hasTris)
    {
        Mesh *mesh = new Mesh();
        BuildMeshFromAssimp(parser, mesh, scene, DirnameOf(sysPath));
        mesh->Compute_BBox();
        mesh->Build_Mesh_BBox_Tree();
        meshObj = reinterpret_cast<ObjectPtr>(mesh);
    }

    if (splatObj != nullptr && meshObj != nullptr)
    {
        CSG *uni = new CSGUnion();
        splatObj->Type |= IS_CHILD_OBJECT;
        meshObj->Type |= IS_CHILD_OBJECT;
        uni->children.push_back(splatObj);
        uni->children.push_back(meshObj);
        uni->Compute_BBox();
        return reinterpret_cast<ObjectPtr>(uni);
    }
    if (splatObj != nullptr)
        return splatObj;
    if (meshObj != nullptr)
        return meshObj;

    parser->Error("Assimp import of '%s' produced no mesh or Gaussian splat data.", sysPath.c_str());
    return nullptr;
}

} // namespace

void Parser::Parse_Splat_Import_Options(DBL &sphereScale, size_t &maxCount, bool &approximate, int &shDegree)
{
    EXPECT
        CASE (MAX_COUNT_TOKEN)
            maxCount = static_cast<size_t>(std::max(0.0, Parse_Float()));
        END_CASE
        CASE (SH_DEGREE_TOKEN)
            shDegree = static_cast<int>(Parse_Float());
            if (shDegree < 0) shDegree = 0;
            if (shDegree > 3) shDegree = 3;
        END_CASE
        CASE (APPROXIMATE_TOKEN)
            approximate = true;
        END_CASE
        OTHERWISE
            UNGET
            EXIT
        END_CASE
    END_EXPECT
    (void)sphereScale;
}

ObjectPtr Parser::Parse_Assimp()
{
    Parse_Begin();

    ObjectPtr existing = Parse_Object_Id();
    if (existing != nullptr)
        return existing;

    mExperimentalFlags.assimpImport = true;

    UCS2 *fileName = Parse_String(true);
    DBL sphereScale = 1.0;
    size_t maxCount = 0;
    bool approximate = false;
    int shDegree = 3;
    Parse_Splat_Import_Options(sphereScale, maxCount, approximate, shDegree);

    std::string sysPath = ResolveAssimpPath(this, fileName);
    ObjectPtr result = BuildAssimpObject(this, sysPath, sphereScale, maxCount, false, approximate, shDegree);
    POV_FREE(fileName);
    result = Parse_Object_Mods(result);
    return result;
}

ObjectPtr Parser::Parse_Gaussian_Splat()
{
    Parse_Begin();

    ObjectPtr existing = Parse_Object_Id();
    if (existing != nullptr)
        return existing;

    mExperimentalFlags.assimpImport = true;

    UCS2 *fileName = Parse_String(true);
    DBL sphereScale = 1.0;
    size_t maxCount = 0;
    bool approximate = false;
    int shDegree = 3;
    Parse_Splat_Import_Options(sphereScale, maxCount, approximate, shDegree);

    std::string sysPath = ResolveAssimpPath(this, fileName);
    ObjectPtr result = BuildAssimpObject(this, sysPath, sphereScale, maxCount, false, approximate, shDegree);
    POV_FREE(fileName);
    result = Parse_Object_Mods(result);
    return result;
}

void Parser::Parse_Assimp_Mesh(Mesh *mesh)
{
    UCS2 *fileName = Parse_String(true);
    std::string sysPath = ResolveAssimpPath(this, fileName);

    Assimp::Importer importer;
    const aiScene *scene = ImportAssimpScene(this, importer, sysPath);
    BuildMeshFromAssimp(this, mesh, scene, DirnameOf(sysPath));
    POV_FREE(fileName);
}

}
// end of namespace pov_parser

#endif // POV_PARSER_EXPERIMENTAL_ASSIMP_IMPORT
