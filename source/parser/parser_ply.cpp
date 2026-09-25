//******************************************************************************
///
/// @file parser/parser_ply.cpp
///
/// Experimental import of Stanford PLY files.
///
/// Supports:
/// - Triangle / polygon mesh PLYs (ASCII or binary), similar to Wavefront OBJ import.
/// - 3D Gaussian Splatting PLYs (Inria / graphdeco layout): each vertex becomes an
///   oriented ellipsoid approximated as a transformed unit sphere inside a CSG merge,
///   using SH degree-0 (f_dc_*) colour and sigmoid(opacity).
///
/// Scene syntax:
/// @code
///   ply { "file.ply" }                 // auto-detect mesh vs. Gaussian splat
///   mesh { ply "mesh.ply" }            // mesh files only
/// @endcode
///
/// @copyright
/// @parblock
///
/// Persistence of Vision Ray Tracer ('POV-Ray') version 3.8.
/// Copyright 1991-2019 Persistence of Vision Raytracer Pty. Ltd.
///
/// POV-Ray is free software: you can redistribute it and/or modify
/// it under the terms of the GNU Affero General Public License as
/// published by the Free Software Foundation, either version 3 of the
/// License, or (at your option) any later version.
///
/// POV-Ray is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU Affero General Public License for more details.
///
/// You should have received a copy of the GNU Affero General Public License
/// along with this program.  If not, see <http://www.gnu.org/licenses/>.
///
/// @endparblock
///
//******************************************************************************

// Unit header file must be the first file included within POV-Ray *.cpp files (pulls in config)
#include "parser/parser.h"

#if POV_PARSER_EXPERIMENTAL_PLY_IMPORT

// C++ variants of C standard header files
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

// C++ standard header files
#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <vector>

// POV-Ray header files (base module)
#include "base/fileinputoutput.h"
#include "base/pov_mem.h"
#include "base/stringutilities.h"
#include "base/textstream.h"

// POV-Ray header files (core module)
#include "core/colour/spectral.h"
#include "core/material/pigment.h"
#include "core/material/texture.h"
#include "core/math/matrix.h"
#include "core/shape/csg.h"
#include "core/shape/gaussiansplat.h"
#include "core/shape/mesh.h"
#include "core/shape/sphere.h"

// this must be the last file included
#include "base/povdebug.h"

namespace pov_parser
{

using namespace pov;

using std::max;
using std::min;
using std::shared_ptr;
using std::vector;

// SH degree-0 constant (graphdeco-inria / 3D Gaussian Splatting)
static const DBL kShC0 = 0.28209479177387814;

enum PlyFormat
{
    kPlyAscii,
    kPlyBinaryLE,
    kPlyBinaryBE
};

enum PlyPropType
{
    kPlyChar,
    kPlyUChar,
    kPlyShort,
    kPlyUShort,
    kPlyInt,
    kPlyUInt,
    kPlyFloat,
    kPlyDouble
};

struct PlyProperty final
{
    std::string name;
    PlyPropType type;
    bool isList;
    PlyPropType countType;
    PlyPropType listType;
};

struct PlyElement final
{
    std::string name;
    size_t count;
    vector<PlyProperty> properties;
};

struct PlyHeader final
{
    PlyFormat format;
    vector<PlyElement> elements;
};

struct FaceVertex final
{
    MeshIndex vertexId;
    MeshIndex normalId;
    MeshIndex uvId;
};

struct FaceData final
{
    FaceVertex vertexList[3];
};

static PlyPropType ParsePlyType(const std::string& t)
{
    if (t == "char" || t == "int8") return kPlyChar;
    if (t == "uchar" || t == "uint8") return kPlyUChar;
    if (t == "short" || t == "int16") return kPlyShort;
    if (t == "ushort" || t == "uint16") return kPlyUShort;
    if (t == "int" || t == "int32") return kPlyInt;
    if (t == "uint" || t == "uint32") return kPlyUInt;
    if (t == "float" || t == "float32") return kPlyFloat;
    if (t == "double" || t == "float64") return kPlyDouble;
    throw POV_EXCEPTION_STRING("Unsupported PLY property type");
}

static size_t PlyTypeSize(PlyPropType t)
{
    switch (t)
    {
        case kPlyChar:
        case kPlyUChar:  return 1;
        case kPlyShort:
        case kPlyUShort: return 2;
        case kPlyInt:
        case kPlyUInt:
        case kPlyFloat:  return 4;
        case kPlyDouble: return 8;
    }
    return 0;
}

static bool ReadLine(shared_ptr<IStream>& stream, std::string& line)
{
    line.clear();
    char c = 0;
    while (stream->read(&c, 1))
    {
        if (c == '\n')
            return true;
        if (c != '\r')
            line.push_back(c);
    }
    return !line.empty();
}

static std::vector<std::string> SplitWords(const std::string& line)
{
    std::vector<std::string> words;
    std::string cur;
    for (char ch : line)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            if (!cur.empty())
            {
                words.push_back(cur);
                cur.clear();
            }
        }
        else
            cur.push_back(ch);
    }
    if (!cur.empty())
        words.push_back(cur);
    return words;
}

static PlyHeader ReadPlyHeader(shared_ptr<IStream>& stream, Parser* parser)
{
    PlyHeader header;
    header.format = kPlyAscii;

    std::string line;
    if (!ReadLine(stream, line) || line != "ply")
        parser->Error("Not a PLY file (missing 'ply' magic).");

    while (ReadLine(stream, line))
    {
        auto words = SplitWords(line);
        if (words.empty())
            continue;
        if (words[0] == "comment" || words[0] == "obj_info")
            continue;
        if (words[0] == "format")
        {
            if (words.size() < 2)
                parser->Error("Malformed PLY format line.");
            if (words[1] == "ascii")
                header.format = kPlyAscii;
            else if (words[1] == "binary_little_endian")
                header.format = kPlyBinaryLE;
            else if (words[1] == "binary_big_endian")
                header.format = kPlyBinaryBE;
            else
                parser->Error("Unsupported PLY format '%s'.", words[1].c_str());
            continue;
        }
        if (words[0] == "element")
        {
            if (words.size() < 3)
                parser->Error("Malformed PLY element line.");
            PlyElement el;
            el.name = words[1];
            el.count = static_cast<size_t>(std::strtoull(words[2].c_str(), nullptr, 10));
            header.elements.push_back(el);
            continue;
        }
        if (words[0] == "property")
        {
            if (header.elements.empty())
                parser->Error("PLY property before any element.");
            PlyProperty prop;
            prop.isList = false;
            if (words.size() < 3)
                parser->Error("Malformed PLY property line.");
            if (words[1] == "list")
            {
                if (words.size() < 5)
                    parser->Error("Malformed PLY list property line.");
                prop.isList = true;
                prop.countType = ParsePlyType(words[2]);
                prop.listType = ParsePlyType(words[3]);
                prop.type = prop.listType;
                prop.name = words[4];
            }
            else
            {
                prop.type = ParsePlyType(words[1]);
                prop.name = words[2];
            }
            header.elements.back().properties.push_back(prop);
            continue;
        }
        if (words[0] == "end_header")
            return header;
    }

    parser->Error("Unexpected end of file while reading PLY header.");
    return header; // unreachable
}

static uint64_t ReadIntegerValue(shared_ptr<IStream>& stream, PlyPropType type, PlyFormat format, Parser* parser)
{
    unsigned char buf[8];
    size_t n = PlyTypeSize(type);
    if (!stream->read(buf, n))
        parser->Error("Unexpected end of file while reading PLY binary data.");

    auto assemble = [&](size_t bytes) -> uint64_t {
        uint64_t v = 0;
        if (format == kPlyBinaryLE)
        {
            for (size_t i = 0; i < bytes; ++i)
                v |= (uint64_t(buf[i]) << (8 * i));
        }
        else
        {
            for (size_t i = 0; i < bytes; ++i)
                v = (v << 8) | buf[i];
        }
        return v;
    };

    switch (type)
    {
        case kPlyChar:   return static_cast<uint64_t>(static_cast<int8_t>(buf[0]));
        case kPlyUChar:  return buf[0];
        case kPlyShort:  return static_cast<uint64_t>(static_cast<int16_t>(assemble(2)));
        case kPlyUShort: return assemble(2) & 0xffffu;
        case kPlyInt:    return static_cast<uint64_t>(static_cast<int32_t>(assemble(4)));
        case kPlyUInt:   return assemble(4) & 0xffffffffu;
        default:
            parser->Error("Internal error: non-integer PLY type in integer reader.");
            return 0;
    }
}

static DBL ReadFloatValue(shared_ptr<IStream>& stream, PlyPropType type, PlyFormat format, Parser* parser)
{
    if (type == kPlyFloat)
    {
        unsigned char buf[4];
        if (!stream->read(buf, 4))
            parser->Error("Unexpected end of file while reading PLY float.");
        uint32_t bits = 0;
        if (format == kPlyBinaryLE)
        {
            bits = uint32_t(buf[0]) | (uint32_t(buf[1]) << 8) | (uint32_t(buf[2]) << 16) | (uint32_t(buf[3]) << 24);
        }
        else
        {
            bits = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8) | uint32_t(buf[3]);
        }
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return DBL(f);
    }
    if (type == kPlyDouble)
    {
        unsigned char buf[8];
        if (!stream->read(buf, 8))
            parser->Error("Unexpected end of file while reading PLY double.");
        uint64_t bits = 0;
        if (format == kPlyBinaryLE)
        {
            for (int i = 0; i < 8; ++i)
                bits |= (uint64_t(buf[i]) << (8 * i));
        }
        else
        {
            for (int i = 0; i < 8; ++i)
                bits = (bits << 8) | buf[i];
        }
        double d;
        std::memcpy(&d, &bits, sizeof(d));
        return DBL(d);
    }
    // integer promoted to float
    return DBL(static_cast<int64_t>(ReadIntegerValue(stream, type, format, parser)));
}

static DBL ReadAsciiNumber(std::vector<std::string>& words, size_t& wi, Parser* parser)
{
    if (wi >= words.size())
        parser->Error("Unexpected end of PLY ASCII record.");
    return std::strtod(words[wi++].c_str(), nullptr);
}

static int64_t ReadAsciiInteger(std::vector<std::string>& words, size_t& wi, Parser* parser)
{
    if (wi >= words.size())
        parser->Error("Unexpected end of PLY ASCII record.");
    return std::strtoll(words[wi++].c_str(), nullptr, 10);
}

static bool HasProperty(const PlyElement& el, const char* name)
{
    for (const auto& p : el.properties)
        if (p.name == name)
            return true;
    return false;
}

static bool IsGaussianSplatElement(const PlyElement& el)
{
    return HasProperty(el, "f_dc_0") && HasProperty(el, "opacity") &&
           HasProperty(el, "scale_0") && HasProperty(el, "rot_0");
}

static DBL Clamp01(DBL v)
{
    return max(0.0, min(1.0, v));
}

static DBL Sigmoid(DBL x)
{
    // numerically stable-ish sigmoid
    if (x >= 0)
    {
        DBL z = std::exp(-x);
        return 1.0 / (1.0 + z);
    }
    DBL z = std::exp(x);
    return z / (1.0 + z);
}

static void QuaternionToMatrix(DBL w, DBL x, DBL y, DBL z, MATRIX out)
{
    DBL n = std::sqrt(w*w + x*x + y*y + z*z);
    if (n < EPSILON)
    {
        MIdentity(out);
        return;
    }
    w /= n; x /= n; y /= n; z /= n;

    MIdentity(out);
    out[0][0] = 1 - 2*(y*y + z*z);
    out[0][1] = 2*(x*y - z*w);
    out[0][2] = 2*(x*z + y*w);
    out[1][0] = 2*(x*y + z*w);
    out[1][1] = 1 - 2*(x*x + z*z);
    out[1][2] = 2*(y*z - x*w);
    out[2][0] = 2*(x*z - y*w);
    out[2][1] = 2*(y*z + x*w);
    out[2][2] = 1 - 2*(x*x + y*y);
}

static TEXTURE *MakeSplatTexture(DBL r, DBL g, DBL b, DBL opacity)
{
    TEXTURE *tex = Create_Texture();
    Destroy_Pigment(tex->Pigment);
    tex->Pigment = Create_Pigment();
    if (tex->Finish == nullptr)
        tex->Finish = Create_Finish();

    RGBColour rgb(Clamp01(r), Clamp01(g), Clamp01(b));
    MathColour mc = ToMathColour(rgb);
    DBL transmit = Clamp01(1.0 - opacity);
    tex->Pigment->colour = TransColour(mc, 0.0, transmit);
    tex->Finish->Ambient = MathColour(0.9);
    tex->Finish->Emission = mc * opacity;
    Post_Textures(tex);
    return tex;
}

static ObjectPtr BuildGaussianSplatObject(Parser* parser, shared_ptr<IStream>& stream, const PlyHeader& header,
                                         const PlyElement& vertexEl, DBL sphereScale, size_t maxCount,
                                         bool approximate, int shDegree)
{
    std::map<std::string, size_t> propIndex;
    for (size_t i = 0; i < vertexEl.properties.size(); ++i)
        propIndex[vertexEl.properties[i].name] = i;

    auto require = [&](const char* name) -> size_t {
        auto it = propIndex.find(name);
        if (it == propIndex.end())
            parser->Error("Gaussian splat PLY missing required property '%s'.", name);
        return it->second;
    };

    size_t ix = require("x"), iy = require("y"), iz = require("z");
    size_t idc0 = require("f_dc_0"), idc1 = require("f_dc_1"), idc2 = require("f_dc_2");
    size_t iop = require("opacity");
    size_t is0 = require("scale_0"), is1 = require("scale_1"), is2 = require("scale_2");
    size_t ir0 = require("rot_0"), ir1 = require("rot_1"), ir2 = require("rot_2"), ir3 = require("rot_3");

    unsigned restCount = 0;
    std::vector<size_t> restIdx;
    for (;;)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "f_rest_%u", restCount);
        auto it = propIndex.find(name);
        if (it == propIndex.end())
            break;
        restIdx.push_back(it->second);
        ++restCount;
    }

    size_t count = vertexEl.count;
    if (maxCount > 0 && maxCount < count)
        count = maxCount;

    GaussianSplatCloud *cloud = nullptr;
    CSG *root = nullptr;
    if (approximate)
        root = new CSGMerge();
    else
    {
        cloud = new GaussianSplatCloud();
        cloud->shDegree = shDegree;
        cloud->restCount = restCount;
    }

    size_t created = 0;
    std::string asciiLine;
    std::vector<std::string> asciiWords;
    size_t asciiWi = 0;

    auto nextAsciiLine = [&]() {
        while (ReadLine(stream, asciiLine))
        {
            asciiWords = SplitWords(asciiLine);
            asciiWi = 0;
            if (!asciiWords.empty())
                return;
        }
        parser->Error("Unexpected end of ASCII PLY while reading Gaussian splats.");
    };

    for (size_t vi = 0; vi < vertexEl.count; ++vi)
    {
        vector<DBL> scalars(vertexEl.properties.size(), 0.0);
        vector<vector<int64_t>> lists(vertexEl.properties.size());

        if (header.format == kPlyAscii)
        {
            if (asciiWi >= asciiWords.size())
                nextAsciiLine();
            for (size_t pi = 0; pi < vertexEl.properties.size(); ++pi)
            {
                const PlyProperty& prop = vertexEl.properties[pi];
                if (prop.isList)
                {
                    int64_t n = ReadAsciiInteger(asciiWords, asciiWi, parser);
                    lists[pi].resize(static_cast<size_t>(n));
                    for (int64_t k = 0; k < n; ++k)
                        lists[pi][static_cast<size_t>(k)] = ReadAsciiInteger(asciiWords, asciiWi, parser);
                }
                else
                    scalars[pi] = ReadAsciiNumber(asciiWords, asciiWi, parser);
            }
        }
        else
        {
            for (size_t pi = 0; pi < vertexEl.properties.size(); ++pi)
            {
                const PlyProperty& prop = vertexEl.properties[pi];
                if (prop.isList)
                {
                    uint64_t n = ReadIntegerValue(stream, prop.countType, header.format, parser);
                    lists[pi].resize(static_cast<size_t>(n));
                    for (uint64_t k = 0; k < n; ++k)
                        lists[pi][static_cast<size_t>(k)] = static_cast<int64_t>(ReadIntegerValue(stream, prop.listType, header.format, parser));
                }
                else
                    scalars[pi] = ReadFloatValue(stream, prop.type, header.format, parser);
            }
        }

        if (vi >= count)
            continue;

        Vector3d center(scalars[ix], scalars[iy], scalars[iz]);
        DBL opacity = Sigmoid(scalars[iop]);
        if (opacity < (1.0 / 255.0))
            continue;

        DBL sx = std::exp(scalars[is0]) * sphereScale;
        DBL sy = std::exp(scalars[is1]) * sphereScale;
        DBL sz = std::exp(scalars[is2]) * sphereScale;
        sx = max(sx, EPSILON);
        sy = max(sy, EPSILON);
        sz = max(sz, EPSILON);

        DBL qw = scalars[ir0], qx = scalars[ir1], qy = scalars[ir2], qz = scalars[ir3];

        if (approximate)
        {
            DBL r = 0.5 + kShC0 * scalars[idc0];
            DBL g = 0.5 + kShC0 * scalars[idc1];
            DBL b = 0.5 + kShC0 * scalars[idc2];
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
            Compute_Translation_Transform(&transT, center);
            Compose_Transforms(&scaleT, &transT);
            Transform_Object(reinterpret_cast<ObjectPtr>(sph), &scaleT);
            sph->Compute_BBox();
            sph->Type |= IS_CHILD_OBJECT;
            root->children.push_back(reinterpret_cast<ObjectPtr>(sph));
        }
        else
        {
            GaussianSplatPoint sp;
            sp.position = center;
            sp.scale = Vector3d(sx, sy, sz);
            sp.qw = qw; sp.qx = qx; sp.qy = qy; sp.qz = qz;
            sp.opacity = opacity;
            sp.dc = Vector3d(scalars[idc0], scalars[idc1], scalars[idc2]);
            sp.restOffset = static_cast<unsigned>(cloud->restCoeffs.size());
            for (unsigned k = 0; k < restCount; ++k)
                cloud->restCoeffs.push_back(scalars[restIdx[k]]);
            cloud->points.push_back(sp);
        }
        ++created;
    }

    if (created == 0)
        parser->Error("No usable Gaussian splats found in PLY file.");

    if (approximate)
    {
        root->Compute_BBox();
        parser->Warning("Imported %lu Gaussian splat(s) as CSG ellipsoidal spheres (approximate).",
                        static_cast<unsigned long>(created));
        return reinterpret_cast<ObjectPtr>(root);
    }

    cloud->BuildAcceleration();
    cloud->Compute_BBox();
    parser->Warning("Imported %lu Gaussian splat(s) as GaussianSplatCloud (SH degree <= %d, rest=%u).",
                    static_cast<unsigned long>(cloud->points.size()), cloud->shDegree, cloud->restCount);
    return reinterpret_cast<ObjectPtr>(cloud);
}

static void BuildMeshFromPly(Parser* parser, Mesh* mesh, shared_ptr<IStream>& stream, const PlyHeader& header,
                             const PlyElement& vertexEl, const PlyElement* faceEl)
{
    if (faceEl == nullptr)
        parser->Error("PLY mesh import requires a 'face' element.");

    std::map<std::string, size_t> vProp;
    for (size_t i = 0; i < vertexEl.properties.size(); ++i)
        vProp[vertexEl.properties[i].name] = i;

    if (!vProp.count("x") || !vProp.count("y") || !vProp.count("z"))
        parser->Error("PLY mesh vertices must have x, y, z properties.");

    size_t ix = vProp["x"], iy = vProp["y"], iz = vProp["z"];
    bool hasN = vProp.count("nx") && vProp.count("ny") && vProp.count("nz");
    bool hasUV = (vProp.count("s") && vProp.count("t")) || (vProp.count("u") && vProp.count("v"));
    size_t inx = 0, iny = 0, inz = 0, iu = 0, iv = 0;
    if (hasN) { inx = vProp["nx"]; iny = vProp["ny"]; inz = vProp["nz"]; }
    if (hasUV)
    {
        if (vProp.count("s")) { iu = vProp["s"]; iv = vProp["t"]; }
        else { iu = vProp["u"]; iv = vProp["v"]; }
    }

    vector<Vector3d> vertexList;
    vector<Vector3d> normalList;
    vector<Vector2d> uvList;
    vertexList.reserve(vertexEl.count);
    if (hasN) normalList.reserve(vertexEl.count);
    if (hasUV) uvList.reserve(vertexEl.count);

    std::string asciiLine;
    std::vector<std::string> asciiWords;
    size_t asciiWi = 0;
    auto nextAsciiLine = [&]() {
        while (ReadLine(stream, asciiLine))
        {
            asciiWords = SplitWords(asciiLine);
            asciiWi = 0;
            if (!asciiWords.empty())
                return;
        }
        parser->Error("Unexpected end of ASCII PLY while reading mesh data.");
    };

    auto readVertexRecord = [&](vector<DBL>& scalars, vector<vector<int64_t>>& lists) {
        scalars.assign(vertexEl.properties.size(), 0.0);
        lists.assign(vertexEl.properties.size(), {});
        if (header.format == kPlyAscii)
        {
            if (asciiWi >= asciiWords.size())
                nextAsciiLine();
            for (size_t pi = 0; pi < vertexEl.properties.size(); ++pi)
            {
                const PlyProperty& prop = vertexEl.properties[pi];
                if (prop.isList)
                {
                    int64_t n = ReadAsciiInteger(asciiWords, asciiWi, parser);
                    lists[pi].resize(static_cast<size_t>(n));
                    for (int64_t k = 0; k < n; ++k)
                        lists[pi][static_cast<size_t>(k)] = ReadAsciiInteger(asciiWords, asciiWi, parser);
                }
                else
                    scalars[pi] = ReadAsciiNumber(asciiWords, asciiWi, parser);
            }
        }
        else
        {
            for (size_t pi = 0; pi < vertexEl.properties.size(); ++pi)
            {
                const PlyProperty& prop = vertexEl.properties[pi];
                if (prop.isList)
                {
                    uint64_t n = ReadIntegerValue(stream, prop.countType, header.format, parser);
                    lists[pi].resize(static_cast<size_t>(n));
                    for (uint64_t k = 0; k < n; ++k)
                        lists[pi][static_cast<size_t>(k)] = static_cast<int64_t>(ReadIntegerValue(stream, prop.listType, header.format, parser));
                }
                else
                    scalars[pi] = ReadFloatValue(stream, prop.type, header.format, parser);
            }
        }
    };

    for (size_t vi = 0; vi < vertexEl.count; ++vi)
    {
        vector<DBL> scalars;
        vector<vector<int64_t>> lists;
        readVertexRecord(scalars, lists);
        vertexList.emplace_back(scalars[ix], scalars[iy], scalars[iz]);
        if (hasN)
            normalList.emplace_back(scalars[inx], scalars[iny], scalars[inz]);
        if (hasUV)
            uvList.emplace_back(scalars[iu], scalars[iv]);
    }

    // Find face index list property
    size_t faceListProp = size_t(-1);
    for (size_t i = 0; i < faceEl->properties.size(); ++i)
    {
        if (faceEl->properties[i].isList &&
            (faceEl->properties[i].name == "vertex_indices" || faceEl->properties[i].name == "vertex_index"))
        {
            faceListProp = i;
            break;
        }
    }
    if (faceListProp == size_t(-1))
        parser->Error("PLY face element has no vertex_indices list property.");

    vector<FaceData> faceList;
    faceList.reserve(faceEl->count * 2);
    bool havePolygonFaces = false;

    for (size_t fi = 0; fi < faceEl->count; ++fi)
    {
        vector<DBL> scalars(faceEl->properties.size(), 0.0);
        vector<vector<int64_t>> lists(faceEl->properties.size());

        if (header.format == kPlyAscii)
        {
            if (asciiWi >= asciiWords.size())
                nextAsciiLine();
            for (size_t pi = 0; pi < faceEl->properties.size(); ++pi)
            {
                const PlyProperty& prop = faceEl->properties[pi];
                if (prop.isList)
                {
                    int64_t n = ReadAsciiInteger(asciiWords, asciiWi, parser);
                    lists[pi].resize(static_cast<size_t>(n));
                    for (int64_t k = 0; k < n; ++k)
                        lists[pi][static_cast<size_t>(k)] = ReadAsciiInteger(asciiWords, asciiWi, parser);
                }
                else
                    scalars[pi] = ReadAsciiNumber(asciiWords, asciiWi, parser);
            }
        }
        else
        {
            for (size_t pi = 0; pi < faceEl->properties.size(); ++pi)
            {
                const PlyProperty& prop = faceEl->properties[pi];
                if (prop.isList)
                {
                    uint64_t n = ReadIntegerValue(stream, prop.countType, header.format, parser);
                    lists[pi].resize(static_cast<size_t>(n));
                    for (uint64_t k = 0; k < n; ++k)
                        lists[pi][static_cast<size_t>(k)] = static_cast<int64_t>(ReadIntegerValue(stream, prop.listType, header.format, parser));
                }
                else
                    scalars[pi] = ReadFloatValue(stream, prop.type, header.format, parser);
            }
        }

        const vector<int64_t>& idxs = lists[faceListProp];
        if (idxs.size() < 3)
            parser->Error("PLY face with fewer than 3 vertices.");
        if (idxs.size() > 3)
            havePolygonFaces = true;

        // Fan triangulation
        for (size_t t = 1; t + 1 < idxs.size(); ++t)
        {
            FaceData face;
            for (int c = 0; c < 3; ++c)
            {
                int64_t id = (c == 0) ? idxs[0] : idxs[t + (c - 1)];
                if (id < 0)
                    id += static_cast<int64_t>(vertexList.size());
                if (id < 0 || static_cast<size_t>(id) >= vertexList.size())
                    parser->Error("PLY face vertex index out of range.");
                face.vertexList[c].vertexId = static_cast<MeshIndex>(id + 1); // 1-based like OBJ path
                face.vertexList[c].normalId = hasN ? static_cast<MeshIndex>(id + 1) : 0;
                face.vertexList[c].uvId = hasUV ? static_cast<MeshIndex>(id + 1) : 0;
            }
            faceList.push_back(face);
        }
    }

    if (havePolygonFaces)
        parser->Warning("Non-triangular faces found in PLY file. Faces will only import properly if they are convex and planar.");

    if (vertexList.empty())
        parser->Error("No vertices in PLY file.");
    if (faceList.empty())
        parser->Error("No faces in PLY file.");

    MeshVector *vertexArray = reinterpret_cast<MeshVector *>(POV_MALLOC(vertexList.size() * sizeof(MeshVector), "triangle mesh data"));
    for (size_t i = 0; i < vertexList.size(); ++i)
        vertexArray[i] = MeshVector(vertexList[i]);

    MeshVector *normalArray = nullptr;
    size_t normalCount = 0;
    if (hasN)
    {
        normalArray = reinterpret_cast<MeshVector *>(POV_MALLOC((normalList.size() + faceList.size()) * sizeof(MeshVector), "triangle mesh data"));
        for (size_t i = 0; i < normalList.size(); ++i)
            normalArray[i] = MeshVector(normalList[i]);
        normalCount = normalList.size();
    }
    else
    {
        normalArray = reinterpret_cast<MeshVector *>(POV_MALLOC(faceList.size() * sizeof(MeshVector), "triangle mesh data"));
        normalCount = 0;
    }

    if (uvList.empty())
        uvList.push_back(Vector2d(0.0, 0.0));
    MeshUVVector *uvArray = reinterpret_cast<MeshUVVector *>(POV_MALLOC(uvList.size() * sizeof(MeshUVVector), "triangle mesh data"));
    for (size_t i = 0; i < uvList.size(); ++i)
        uvArray[i] = MeshUVVector(uvList[i]);

    MESH_TRIANGLE *triangleArray = reinterpret_cast<MESH_TRIANGLE *>(POV_MALLOC(faceList.size() * sizeof(MESH_TRIANGLE), "triangle mesh data"));
    for (size_t i = 0, j = normalCount; i < faceList.size(); ++i, ++j)
    {
        const FaceData& src = faceList[i];
        MESH_TRIANGLE& triangle = triangleArray[i];
        mesh->Init_Mesh_Triangle(&triangle);
        triangle.P1 = src.vertexList[0].vertexId - 1;
        triangle.P2 = src.vertexList[1].vertexId - 1;
        triangle.P3 = src.vertexList[2].vertexId - 1;
        triangle.Texture = -1;
        triangle.UV1 = max(1, src.vertexList[0].uvId) - 1;
        triangle.UV2 = max(1, src.vertexList[1].uvId) - 1;
        triangle.UV3 = max(1, src.vertexList[2].uvId) - 1;
        triangle.Smooth = hasN;
        if (hasN)
        {
            triangle.N1 = src.vertexList[0].normalId - 1;
            triangle.N2 = src.vertexList[1].normalId - 1;
            triangle.N3 = src.vertexList[2].normalId - 1;
        }

        Vector3d P1(vertexList[triangle.P1]);
        Vector3d P2(vertexList[triangle.P2]);
        Vector3d P3(vertexList[triangle.P3]);
        Vector3d N;
        mesh->Compute_Mesh_Triangle(&triangle, triangle.Smooth, P1, P2, P3, N);
        triangle.Normal_Ind = static_cast<MeshIndex>(j);
        normalArray[j] = MeshVector(N);
    }

    mesh->Data = reinterpret_cast<MESH_DATA *>(POV_MALLOC(sizeof(MESH_DATA), "triangle mesh data"));
    mesh->Data->References = 1;
    mesh->Data->Tree = nullptr;
    mesh->Type |= PATCH_OBJECT;
    mesh->has_inside_vector = false;

    mesh->Data->Normals   = normalArray;
    mesh->Data->Triangles = triangleArray;
    mesh->Data->Vertices  = vertexArray;
    mesh->Data->UVCoords  = uvArray;
    mesh->Textures        = nullptr;

    mesh->Data->Number_Of_Normals   = static_cast<MeshIndex>(normalCount + faceList.size());
    mesh->Data->Number_Of_Triangles = static_cast<MeshIndex>(faceList.size());
    mesh->Data->Number_Of_Vertices  = static_cast<MeshIndex>(vertexList.size());
    mesh->Data->Number_Of_UVCoords  = static_cast<MeshIndex>(uvList.size());
    mesh->Number_Of_Textures        = 0;
}

static shared_ptr<IStream> OpenPlyFile(Parser* parser, UCS2 *fileName)
{
    UCS2String formal(fileName);
    UCS2String ign;
    shared_ptr<IStream> stream = parser->Locate_File(formal, POV_File_Text_User, ign, true);
    if (stream == nullptr)
        parser->Error("Cannot open PLY file %s.", UCS2toSysString(fileName).c_str());
    return stream;
}

ObjectPtr Parser::Parse_Ply()
{
    Parse_Begin();

    ObjectPtr existing = Parse_Object_Id();
    if (existing != nullptr)
        return existing;

    mExperimentalFlags.plyImport = true;

    UCS2 *fileName = Parse_String(true);
    DBL sphereScale = 1.0;
    size_t maxCount = 0;
    bool approximate = false;
    int shDegree = 3;
    DBL opacityCutoff = 0.01;
    DBL alphaStop = 0.995;
    int samples = 4;
    int maxHits = 0;
    DBL giWeight = 1.0;
#if POV_PARSER_EXPERIMENTAL_ASSIMP_IMPORT
    Parse_Splat_Import_Options(sphereScale, maxCount, approximate, shDegree,
                               opacityCutoff, alphaStop, samples, maxHits, giWeight);
#else
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
#endif

    shared_ptr<IStream> stream = OpenPlyFile(this, fileName);
    PlyHeader header = ReadPlyHeader(stream, this);

    const PlyElement *vertexEl = nullptr;
    const PlyElement *faceEl = nullptr;
    for (const auto& el : header.elements)
    {
        if (el.name == "vertex")
            vertexEl = &el;
        else if (el.name == "face")
            faceEl = &el;
    }
    if (vertexEl == nullptr)
        Error("PLY file has no vertex element.");

    ObjectPtr result = nullptr;

    if (IsGaussianSplatElement(*vertexEl))
    {
        result = BuildGaussianSplatObject(this, stream, header, *vertexEl, sphereScale, maxCount, approximate, shDegree);
        if (GaussianSplatCloud *cloud = dynamic_cast<GaussianSplatCloud *>(result))
        {
            cloud->opacityCutoff = opacityCutoff;
            cloud->alphaStop = alphaStop;
            cloud->samples = samples;
            cloud->maxHits = maxHits;
            cloud->giWeight = giWeight;
        }
    }
    else
    {
        Mesh *mesh = new Mesh();
        BuildMeshFromPly(this, mesh, stream, header, *vertexEl, faceEl);
        mesh->Compute_BBox();
        result = reinterpret_cast<ObjectPtr>(mesh);
    }

    POV_FREE(fileName);
    result = Parse_Object_Mods(result);
    return result;
}

void Parser::Parse_Ply_Mesh(Mesh* mesh)
{
    UCS2 *fileName = Parse_String(true);
    shared_ptr<IStream> stream = OpenPlyFile(this, fileName);
    PlyHeader header = ReadPlyHeader(stream, this);

    const PlyElement *vertexEl = nullptr;
    const PlyElement *faceEl = nullptr;
    for (const auto& el : header.elements)
    {
        if (el.name == "vertex")
            vertexEl = &el;
        else if (el.name == "face")
            faceEl = &el;
    }
    if (vertexEl == nullptr)
        Error("PLY file has no vertex element.");
    if (IsGaussianSplatElement(*vertexEl))
        Error("Gaussian splat PLY cannot be imported via mesh { ply ... }; use top-level ply { \"file\" } instead.");

    BuildMeshFromPly(this, mesh, stream, header, *vertexEl, faceEl);
    POV_FREE(fileName);
}

}
// end of namespace pov_parser

#endif // POV_PARSER_EXPERIMENTAL_PLY_IMPORT
