#pragma once
// Port-only (checkpoint 132md2, research/MODDING.md "Stage 3"): a minimal JSON value and glTF 2.0 container reader /
// writer for the mod layer's model replacements and the mod tool's model export. Not game code: the PS2 game has no
// glTF. Self-contained (no JSON or glTF library), so the same code serves later platforms. Supported:
//   - .gltf (JSON) with buffers / images as relative file URIs or base64 "data:" URIs, and .glb (binary container,
//     its BIN chunk as buffer 0);
//   - accessors of FLOAT and normalised / plain UNSIGNED_BYTE / UNSIGNED_SHORT / UNSIGNED_INT components, SCALAR..MAT4,
//     with byteStride; sparse accessors (132md3: Blender's shape keys) and accessors without a bufferView (zeros);
//   - node transforms by matrix or translation / rotation / scale.
// Errors throw std::runtime_error naming the offending element.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega::gltf {

struct Json {
    enum class Type { null, boolean, number, string, array, object };
    Type type = Type::null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;   // insertion order kept (the writer's output order)

    Json() = default;
    Json(double n) : type(Type::number), number(n) {}
    Json(int n) : type(Type::number), number(n) {}
    Json(std::size_t n) : type(Type::number), number(static_cast<double>(n)) {}
    Json(bool b) : type(Type::boolean), boolean(b) {}
    Json(const char* s) : type(Type::string), string(s) {}
    Json(std::string s) : type(Type::string), string(std::move(s)) {}
    static Json make_array() { Json j; j.type = Type::array; return j; }
    static Json make_object() { Json j; j.type = Type::object; return j; }

    const Json* find(std::string_view key) const;
    // Object member access for building (appends a member when absent; appending invalidates references to the
    // object's members, so do not hold one across another new key).
    Json& operator[](std::string_view key);
    Json& push(Json value) { array.push_back(std::move(value)); return array.back(); }
};
Json parse_json(std::string_view text);
std::string write_json(const Json& value);

// A matrix is column-major float[16] (glTF's order, and the skinning code's SkinMatrix).
using Matrix = std::array<float, 16>;
Matrix identity();
Matrix multiply(const Matrix& a, const Matrix& b);

struct Document {
    Json json;
    std::vector<std::vector<std::uint8_t>> buffers;
    std::filesystem::path base;   // the folder relative URIs resolve against (empty: only data: URIs)
};
// From a file (.gltf or .glb by its first bytes); external URIs relative to the file's folder, never outside it.
Document read_document(const std::filesystem::path& path);
// From bytes (tests): `base` resolves relative URIs (empty: relative URIs are refused).
Document parse_document(std::span<const std::uint8_t> bytes, const std::filesystem::path& base = {});

// Accessor contents as floats (normalised integers mapped to 0..1), `components` per element (1 SCALAR .. 16 MAT4).
std::vector<float> read_floats(const Document& doc, std::size_t accessor, std::size_t& components);
// Accessor contents as unsigned integers (UNSIGNED_BYTE / SHORT / INT only).
std::vector<std::uint32_t> read_uints(const Document& doc, std::size_t accessor, std::size_t& components);
// The bytes of image `index` (bufferView or URI).
std::vector<std::uint8_t> image_bytes(const Document& doc, std::size_t index);
// A node's local transform; each node's parent (-1: a scene root); world (rest) transforms.
Matrix node_local(const Json& node);
std::vector<int> node_parents(const Json& json);
std::vector<Matrix> node_worlds(const Json& json);

// Integer value of a JSON member (throws when absent or not a non-negative integer, naming `what`).
std::size_t index_of(const Json& object, std::string_view key, std::string_view what);

// Building a .gltf: one buffer, appended to with 4-byte alignment.
struct Builder {
    Json json = Json::make_object();
    std::vector<std::uint8_t> bin;
    Builder();
    // A bufferView over bytes appended to `bin`; returns its index. target: 0 none, 34962 vertices, 34963 indices.
    std::size_t view(std::span<const std::uint8_t> bytes, int target = 0);
    // An accessor over a new view: componentType 5126 float / 5121 ubyte / 5123 ushort / 5125 uint.
    std::size_t accessor(std::span<const std::uint8_t> bytes, int component_type, std::size_t count, std::string_view type,
                         int target = 0, const std::vector<float>* minimum = nullptr, const std::vector<float>* maximum = nullptr);
    // The .gltf text, its buffer naming `bin_uri` (none when `bin` is empty).
    std::string finish(const std::string& bin_uri);
    // A .glb (JSON and BIN chunks).
    std::vector<std::uint8_t> finish_glb();
};

}  // namespace omega::gltf
