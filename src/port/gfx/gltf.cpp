#include "gltf.hpp"
#include "mod_archive.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace omega::gltf {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("glTF: " + message); }

struct Parser {
    std::string_view text;
    std::size_t at = 0;
    int depth = 0;

    void space() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
    }
    char peek() {
        space();
        if (at >= text.size()) fail("JSON ends early");
        return text[at];
    }
    void expect(char c) {
        if (peek() != c) fail(std::string("JSON expected '") + c + "' at byte " + std::to_string(at));
        ++at;
    }
    bool literal(std::string_view word) {
        if (text.substr(at, word.size()) != word) return false;
        at += word.size();
        return true;
    }
    static void utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) out.push_back(static_cast<char>(code));
        else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    std::uint32_t hex4() {
        if (at + 4 > text.size()) fail("JSON \\u escape ends early");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[at++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("JSON bad \\u escape");
        }
        return value;
    }
    std::string string_value() {
        expect('"');
        std::string out;
        while (true) {
            if (at >= text.size()) fail("JSON string ends early");
            const char c = text[at++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("JSON control character in a string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at >= text.size()) fail("JSON string ends early");
            const char e = text[at++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t code = hex4();
                if (code >= 0xd800 && code < 0xdc00 && text.substr(at, 2) == "\\u") {
                    at += 2;
                    const auto low = hex4();
                    if (low < 0xdc00 || low >= 0xe000) fail("JSON bad surrogate pair");
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                utf8(out, code);
                break;
            }
            default: fail("JSON bad escape");
            }
        }
    }
    Json value() {
        if (++depth > 64) fail("JSON nested too deeply");
        Json result;
        const char c = peek();
        if (c == '{') {
            ++at;
            result.type = Json::Type::object;
            if (peek() == '}') ++at;
            else
                while (true) {
                    auto key = string_value();
                    expect(':');
                    result.object.emplace_back(std::move(key), value());
                    if (peek() == ',') { ++at; continue; }
                    expect('}');
                    break;
                }
        } else if (c == '[') {
            ++at;
            result.type = Json::Type::array;
            if (peek() == ']') ++at;
            else
                while (true) {
                    result.array.push_back(value());
                    if (peek() == ',') { ++at; continue; }
                    expect(']');
                    break;
                }
        } else if (c == '"') {
            result = Json(string_value());
        } else if (literal("true")) {
            result = Json(true);
        } else if (literal("false")) {
            result = Json(false);
        } else if (literal("null")) {
        } else {
            const auto start = at;
            if (at < text.size() && (text[at] == '-' || text[at] == '+')) ++at;
            while (at < text.size() && (std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '.' || text[at] == 'e' ||
                                        text[at] == 'E' || text[at] == '-' || text[at] == '+'))
                ++at;
            double number = 0;
            const auto parsed = std::from_chars(text.data() + start, text.data() + at, number);
            if (start == at || parsed.ec != std::errc() || parsed.ptr != text.data() + at || !std::isfinite(number))
                fail("JSON bad value at byte " + std::to_string(start));
            result = Json(number);
        }
        --depth;
        return result;
    }
};

void write_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

void write_value(std::string& out, const Json& v, int indent) {
    const auto newline = [&](int level) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    switch (v.type) {
    case Json::Type::null: out += "null"; break;
    case Json::Type::boolean: out += v.boolean ? "true" : "false"; break;
    case Json::Type::number: {
        char buffer[32];
        const double n = v.number;
        if (n == std::floor(n) && std::fabs(n) < 1e15) {
            const auto r = std::to_chars(buffer, buffer + sizeof buffer, static_cast<long long>(n));
            out.append(buffer, r.ptr);
        } else {
            // Shortest float text that reads back as the same float (glTF data is single precision).
            const auto r = std::to_chars(buffer, buffer + sizeof buffer, static_cast<float>(n));
            out.append(buffer, r.ptr);
        }
        break;
    }
    case Json::Type::string: write_string(out, v.string); break;
    case Json::Type::array: {
        // Short numeric arrays (vectors, matrices) on one line.
        const bool flat = v.array.size() <= 16 && std::all_of(v.array.begin(), v.array.end(), [](const Json& e) {
                              return e.type == Json::Type::number;
                          });
        out.push_back('[');
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            if (i) out.push_back(',');
            if (!flat) newline(indent + 1);
            write_value(out, v.array[i], indent + 1);
        }
        if (!flat && !v.array.empty()) newline(indent);
        out.push_back(']');
        break;
    }
    case Json::Type::object: {
        out.push_back('{');
        for (std::size_t i = 0; i < v.object.size(); ++i) {
            if (i) out.push_back(',');
            newline(indent + 1);
            write_string(out, v.object[i].first);
            out += ": ";
            write_value(out, v.object[i].second, indent + 1);
        }
        if (!v.object.empty()) newline(indent);
        out.push_back('}');
        break;
    }
    }
}

std::vector<std::uint8_t> base64(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        else if (c == '=') break;
        else fail("bad base64 data URI");
        bits = (bits << 6) | static_cast<std::uint32_t>(value);
        if ((count += 6) >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>(bits >> count));
        }
    }
    return out;
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    // Checkpoint 132md10: a model inside a mod archive (research/MODDING.md "Mod archives").
    try {
        if (auto archived = mods::read_archived(path)) return std::move(*archived);
    } catch (const std::exception& e) {
        fail(e.what());
    }
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail("cannot open " + path.string());
    // One read of the whole file (132md7: a byte-wise stream copy cost the mod load more than the PNG decode).
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) fail("cannot read " + path.string());
    return bytes;
}

std::string percent_decode(std::string_view uri) {
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            out.push_back(static_cast<char>(std::stoi(std::string(uri.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(uri[i]);
        }
    }
    return out;
}

std::vector<std::uint8_t> uri_bytes(const Document& doc, const std::string& uri) {
    if (uri.starts_with("data:")) {
        const auto comma = uri.find(',');
        if (comma == std::string::npos || uri.substr(0, comma).find(";base64") == std::string::npos)
            fail("only base64 data URIs are supported");
        return base64(std::string_view(uri).substr(comma + 1));
    }
    if (doc.base.empty()) fail("relative URI " + uri + " without a base folder");
    const auto relative = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(uri.data()), uri.size()));
    const auto decoded = std::filesystem::path(percent_decode(uri));
    const auto path = (doc.base / decoded).lexically_normal();
    const auto rel = path.lexically_relative(doc.base.lexically_normal());
    if (decoded.is_absolute() || rel.empty() || *rel.begin() == "..") fail("URI " + uri + " leaves the model's folder");
    (void)relative;
    return read_file_bytes(path);
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    std::uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

std::size_t type_components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    fail("unknown accessor type " + type);
}

struct AccessorView {
    std::span<const std::uint8_t> bytes;   // from the first element (empty: no bufferView, every element zero)
    std::size_t count = 0, components = 0, stride = 0, size = 0;
    int type = 0;
    bool normalized = false;
    // Checkpoint 132md3: a sparse accessor's substituted elements (ascending indices; values tightly packed).
    std::vector<std::uint32_t> sparse_indices;
    std::span<const std::uint8_t> sparse_values;

    // Element i's bytes, or null for an element that is zero (no bufferView and not substituted). `cursor` walks the
    // sparse indices; i must ascend across calls sharing it.
    const std::uint8_t* element(std::size_t i, std::size_t& cursor) const {
        while (cursor < sparse_indices.size() && sparse_indices[cursor] < i) ++cursor;
        if (cursor < sparse_indices.size() && sparse_indices[cursor] == i) return sparse_values.data() + cursor * size * components;
        return bytes.empty() ? nullptr : bytes.data() + i * stride;
    }
};

std::size_t component_size(int type, const std::string& what) {
    switch (type) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: fail(what + " componentType " + std::to_string(type));
    }
}

// Bytes [offset, offset + need) of a bufferView (`need` measured from the offset), checked.
std::span<const std::uint8_t> view_bytes(const Document& doc, std::size_t view_index, std::size_t offset, std::size_t need,
                                         const std::string& what, std::size_t* length_after = nullptr) {
    const auto* views = doc.json.find("bufferViews");
    if (!views || view_index >= views->array.size()) fail(what + " bufferView missing");
    const auto& view = views->array[view_index];
    const auto buffer = index_of(view, "buffer", "bufferView");
    if (buffer >= doc.buffers.size()) fail("bufferView " + std::to_string(view_index) + " buffer missing");
    const std::size_t view_offset = view.find("byteOffset") ? index_of(view, "byteOffset", "bufferView") : 0;
    const std::size_t view_length = index_of(view, "byteLength", "bufferView");
    const auto& data = doc.buffers[buffer];
    if (view_offset > data.size() || view_length > data.size() - view_offset) fail("bufferView outside its buffer");
    if (offset > view_length || need > view_length - offset) fail(what + " outside its bufferView");
    if (length_after) *length_after = view_length - offset;
    return std::span<const std::uint8_t>(data).subspan(view_offset + offset, view_length - offset);
}

AccessorView accessor_view(const Document& doc, std::size_t index) {
    const auto* accessors = doc.json.find("accessors");
    if (!accessors || index >= accessors->array.size()) fail("accessor " + std::to_string(index) + " missing");
    const auto& a = accessors->array[index];
    const auto what = "accessor " + std::to_string(index);
    AccessorView v;
    v.type = static_cast<int>(index_of(a, "componentType", what));
    v.count = index_of(a, "count", what);
    const auto* type = a.find("type");
    if (!type || type->type != Json::Type::string) fail(what + " has no type");
    v.components = type_components(type->string);
    if (const auto* n = a.find("normalized")) v.normalized = n->boolean;
    v.size = component_size(v.type, what);
    if (v.count > (1u << 26)) fail(what + " is too large");
    const std::size_t element = v.size * v.components;
    v.stride = element;
    // Without a bufferView every element is zero (glTF 2.0 3.6.2), unless substituted by "sparse".
    if (a.find("bufferView")) {
        const auto view_index = index_of(a, "bufferView", what);
        const auto* views = doc.json.find("bufferViews");
        if (!views || view_index >= views->array.size()) fail(what + " bufferView missing");
        const auto& view = views->array[view_index];
        const std::size_t offset = a.find("byteOffset") ? index_of(a, "byteOffset", what) : 0;
        if (view.find("byteStride")) v.stride = index_of(view, "byteStride", "bufferView");
        if (v.stride < element) fail(what + " stride below its element size");
        const std::size_t need = v.count ? v.stride * (v.count - 1) + element : 0;
        v.bytes = view_bytes(doc, view_index, offset, need, what);
        if (v.count == 0) v.bytes = {};
        else if (v.bytes.empty()) fail(what + " outside its bufferView");
    }
    // Checkpoint 132md3: sparse substitution (glTF 2.0 3.6.2.3; Blender writes shape keys so by default).
    if (const auto* sparse = a.find("sparse")) {
        const auto n = index_of(*sparse, "count", what + " sparse");
        if (n == 0 || n > v.count) fail(what + " sparse count " + std::to_string(n) + " outside 1.." + std::to_string(v.count));
        const auto* indices = sparse->find("indices");
        const auto* values = sparse->find("values");
        if (!indices || !values) fail(what + " sparse needs indices and values");
        const auto index_type = static_cast<int>(index_of(*indices, "componentType", what + " sparse indices"));
        if (index_type != 5121 && index_type != 5123 && index_type != 5125) fail(what + " sparse indices are not unsigned integers");
        const auto index_size = component_size(index_type, what);
        const auto index_bytes = view_bytes(doc, index_of(*indices, "bufferView", what + " sparse indices"),
                                            indices->find("byteOffset") ? index_of(*indices, "byteOffset", what) : 0, n * index_size,
                                            what + " sparse indices");
        v.sparse_indices.resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            std::uint32_t value = 0;
            std::memcpy(&value, index_bytes.data() + k * index_size, index_size);
            if (value >= v.count) fail(what + " sparse index " + std::to_string(value) + " past its count");
            if (k && value <= v.sparse_indices[k - 1]) fail(what + " sparse indices are not strictly increasing");
            v.sparse_indices[k] = value;
        }
        v.sparse_values = view_bytes(doc, index_of(*values, "bufferView", what + " sparse values"),
                                     values->find("byteOffset") ? index_of(*values, "byteOffset", what) : 0, n * element,
                                     what + " sparse values");
    }
    return v;
}

}  // namespace

const Json* Json::find(std::string_view key) const {
    if (type != Type::object) return nullptr;
    for (const auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

Json& Json::operator[](std::string_view key) {
    if (type == Type::null) type = Type::object;
    for (auto& [k, v] : object)
        if (k == key) return v;
    object.emplace_back(std::string(key), Json());
    return object.back().second;
}

Json parse_json(std::string_view text) {
    Parser p{text};
    auto v = p.value();
    p.space();
    if (p.at != text.size()) fail("JSON has text after its value");
    return v;
}

std::string write_json(const Json& value) {
    std::string out;
    write_value(out, value, 0);
    out.push_back('\n');
    return out;
}

Matrix identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    return r;
}

std::size_t index_of(const Json& object, std::string_view key, std::string_view what) {
    const auto* v = object.find(key);
    if (!v || v->type != Json::Type::number || v->number < 0 || v->number != std::floor(v->number) || v->number > 4e9)
        fail(std::string(what) + " needs a non-negative integer " + std::string(key));
    return static_cast<std::size_t>(v->number);
}

Document parse_document(std::span<const std::uint8_t> bytes, const std::filesystem::path& base) {
    Document doc;
    doc.base = base;
    std::optional<std::vector<std::uint8_t>> glb_bin;
    std::string_view text;
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        if (le32(bytes, 4) != 2) fail("GLB version " + std::to_string(le32(bytes, 4)));
        const auto length = le32(bytes, 8);
        if (length > bytes.size()) fail("GLB shorter than its header length");
        std::size_t at = 12;
        while (at + 8 <= length) {
            const auto chunk = le32(bytes, at), type = le32(bytes, at + 4);
            if (chunk > length - at - 8) fail("GLB chunk outside the file");
            const auto data = bytes.subspan(at + 8, chunk);
            if (type == 0x4e4f534a && text.empty()) text = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
            else if (type == 0x004e4942 && !glb_bin) glb_bin.emplace(data.begin(), data.end());
            at += 8 + ((chunk + 3) & ~std::size_t(3));
        }
        if (text.empty()) fail("GLB has no JSON chunk");
    } else {
        text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    }
    doc.json = parse_json(text);
    if (doc.json.type != Json::Type::object) fail("the document is not a JSON object");
    const auto* asset = doc.json.find("asset");
    const auto* version = asset ? asset->find("version") : nullptr;
    if (!version || version->type != Json::Type::string || !version->string.starts_with("2."))
        fail("asset.version is not 2.x");
    if (const auto* required = doc.json.find("extensionsRequired"); required && !required->array.empty())
        fail("requires extension " + required->array[0].string + " (not supported)");
    if (const auto* buffers = doc.json.find("buffers"))
        for (std::size_t i = 0; i < buffers->array.size(); ++i) {
            const auto& b = buffers->array[i];
            const auto length = index_of(b, "byteLength", "buffer");
            std::vector<std::uint8_t> data;
            if (const auto* uri = b.find("uri")) data = uri_bytes(doc, uri->string);
            else if (i == 0 && glb_bin) data = *glb_bin;
            else fail("buffer " + std::to_string(i) + " has no data");
            if (data.size() < length) fail("buffer " + std::to_string(i) + " shorter than its byteLength");
            data.resize(length);
            doc.buffers.push_back(std::move(data));
        }
    return doc;
}

Document read_document(const std::filesystem::path& path) {
    const auto bytes = read_file_bytes(path);
    return parse_document(bytes, path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path());
}

std::vector<float> read_floats(const Document& doc, std::size_t accessor, std::size_t& components) {
    const auto v = accessor_view(doc, accessor);
    components = v.components;
    std::vector<float> out(v.count * v.components);
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < v.count; ++i) {
        const auto* e = v.element(i, cursor);
        if (!e) continue;
        for (std::size_t c = 0; c < v.components; ++c) {
            const auto* p = e + c * v.size;
            float value = 0;
            switch (v.type) {
            case 5126: std::memcpy(&value, p, 4); break;
            case 5121: value = v.normalized ? *p / 255.0f : *p; break;
            case 5120: { const auto s = static_cast<std::int8_t>(*p); value = v.normalized ? std::max(s / 127.0f, -1.0f) : s; break; }
            case 5123: { std::uint16_t u; std::memcpy(&u, p, 2); value = v.normalized ? u / 65535.0f : u; break; }
            case 5122: { std::int16_t s; std::memcpy(&s, p, 2); value = v.normalized ? std::max(s / 32767.0f, -1.0f) : s; break; }
            case 5125: { std::uint32_t u; std::memcpy(&u, p, 4); value = static_cast<float>(u); break; }
            }
            if (!std::isfinite(value)) fail("accessor " + std::to_string(accessor) + " has a non-finite value");
            out[i * v.components + c] = value;
        }
    }
    return out;
}

std::vector<std::uint32_t> read_uints(const Document& doc, std::size_t accessor, std::size_t& components) {
    const auto v = accessor_view(doc, accessor);
    if (v.type != 5121 && v.type != 5123 && v.type != 5125)
        fail("accessor " + std::to_string(accessor) + " is not unsigned integers");
    components = v.components;
    std::vector<std::uint32_t> out(v.count * v.components);
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < v.count; ++i) {
        const auto* e = v.element(i, cursor);
        if (!e) continue;
        for (std::size_t c = 0; c < v.components; ++c) {
            const auto* p = e + c * v.size;
            std::uint32_t value = 0;
            if (v.size == 1) value = *p;
            else if (v.size == 2) { std::uint16_t u; std::memcpy(&u, p, 2); value = u; }
            else std::memcpy(&value, p, 4);
            out[i * v.components + c] = value;
        }
    }
    return out;
}

std::vector<std::uint8_t> image_bytes(const Document& doc, std::size_t index) {
    const auto* images = doc.json.find("images");
    if (!images || index >= images->array.size()) fail("image " + std::to_string(index) + " missing");
    const auto& image = images->array[index];
    if (const auto* uri = image.find("uri")) return uri_bytes(doc, uri->string);
    const auto view_index = index_of(image, "bufferView", "image");
    const auto* views = doc.json.find("bufferViews");
    if (!views || view_index >= views->array.size()) fail("image bufferView missing");
    const auto& view = views->array[view_index];
    const auto buffer = index_of(view, "buffer", "bufferView");
    if (buffer >= doc.buffers.size()) fail("image buffer missing");
    const std::size_t offset = view.find("byteOffset") ? index_of(view, "byteOffset", "bufferView") : 0;
    const auto length = index_of(view, "byteLength", "bufferView");
    const auto& data = doc.buffers[buffer];
    if (offset > data.size() || length > data.size() - offset) fail("image bufferView outside its buffer");
    return {data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + length)};
}

Matrix node_local(const Json& node) {
    const auto numbers = [&](const char* key, std::size_t n, std::span<const float> fallback) {
        std::vector<float> v(fallback.begin(), fallback.end());
        if (const auto* a = node.find(key)) {
            if (a->array.size() != n) fail(std::string("node ") + key + " needs " + std::to_string(n) + " numbers");
            for (std::size_t i = 0; i < n; ++i) {
                if (a->array[i].type != Json::Type::number) fail(std::string("node ") + key + " has a non-number");
                v[i] = static_cast<float>(a->array[i].number);
            }
        }
        return v;
    };
    if (node.find("matrix")) {
        const auto id = identity();
        const auto m = numbers("matrix", 16, id);
        Matrix r;
        std::copy(m.begin(), m.end(), r.begin());
        return r;
    }
    const float zero3[3] = {0, 0, 0}, one3[3] = {1, 1, 1}, unit_q[4] = {0, 0, 0, 1};
    const auto t = numbers("translation", 3, zero3), s = numbers("scale", 3, one3), q = numbers("rotation", 4, unit_q);
    float x = q[0], y = q[1], z = q[2], w = q[3];
    const float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 0) { x /= len; y /= len; z /= len; w /= len; }
    Matrix r{};
    r[0] = (1 - 2 * (y * y + z * z)) * s[0];
    r[1] = (2 * (x * y + z * w)) * s[0];
    r[2] = (2 * (x * z - y * w)) * s[0];
    r[4] = (2 * (x * y - z * w)) * s[1];
    r[5] = (1 - 2 * (x * x + z * z)) * s[1];
    r[6] = (2 * (y * z + x * w)) * s[1];
    r[8] = (2 * (x * z + y * w)) * s[2];
    r[9] = (2 * (y * z - x * w)) * s[2];
    r[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r[12] = t[0];
    r[13] = t[1];
    r[14] = t[2];
    r[15] = 1;
    return r;
}

std::vector<int> node_parents(const Json& json) {
    const auto* nodes = json.find("nodes");
    const std::size_t n = nodes ? nodes->array.size() : 0;
    std::vector<int> parent(n, -1);
    for (std::size_t i = 0; i < n; ++i)
        if (const auto* children = nodes->array[i].find("children"))
            for (const auto& c : children->array) {
                if (c.type != Json::Type::number || c.number < 0 || c.number >= static_cast<double>(n))
                    fail("node " + std::to_string(i) + " has a bad child");
                const auto child = static_cast<std::size_t>(c.number);
                if (parent[child] != -1 || child == i) fail("node " + std::to_string(child) + " has two parents");
                parent[child] = static_cast<int>(i);
            }
    return parent;
}

std::vector<Matrix> node_worlds(const Json& json) {
    const auto* nodes = json.find("nodes");
    const std::size_t n = nodes ? nodes->array.size() : 0;
    const auto parent = node_parents(json);
    std::vector<Matrix> world(n);
    std::vector<int> state(n, 0);
    for (std::size_t start = 0; start < n; ++start) {
        std::vector<std::size_t> chain;
        for (auto i = static_cast<int>(start); i >= 0 && state[static_cast<std::size_t>(i)] != 2; i = parent[static_cast<std::size_t>(i)]) {
            if (state[static_cast<std::size_t>(i)] == 1) fail("node hierarchy has a cycle");
            state[static_cast<std::size_t>(i)] = 1;
            chain.push_back(static_cast<std::size_t>(i));
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const auto local = node_local(nodes->array[*it]);
            world[*it] = parent[*it] < 0 ? local : multiply(world[static_cast<std::size_t>(parent[*it])], local);
            state[*it] = 2;
        }
    }
    return world;
}

Builder::Builder() {
    json["asset"]["version"] = "2.0";
}

std::size_t Builder::view(std::span<const std::uint8_t> bytes, int target) {
    while (bin.size() % 4) bin.push_back(0);
    auto& views = json["bufferViews"];
    if (views.type != Json::Type::array) views = Json::make_array();
    auto v = Json::make_object();
    v["buffer"] = 0;
    v["byteOffset"] = bin.size();
    v["byteLength"] = bytes.size();
    if (target) v["target"] = target;
    bin.insert(bin.end(), bytes.begin(), bytes.end());
    views.push(std::move(v));
    return views.array.size() - 1;
}

std::size_t Builder::accessor(std::span<const std::uint8_t> bytes, int component_type, std::size_t count, std::string_view type,
                              int target, const std::vector<float>* minimum, const std::vector<float>* maximum) {
    const auto v = view(bytes, target);
    auto& accessors = json["accessors"];
    if (accessors.type != Json::Type::array) accessors = Json::make_array();
    auto a = Json::make_object();
    a["bufferView"] = v;
    a["componentType"] = component_type;
    a["count"] = count;
    a["type"] = std::string(type);
    const auto list = [](const std::vector<float>& values) {
        auto j = Json::make_array();
        for (const float f : values) j.push(Json(static_cast<double>(f)));
        return j;
    };
    if (minimum) a["min"] = list(*minimum);
    if (maximum) a["max"] = list(*maximum);
    accessors.push(std::move(a));
    return accessors.array.size() - 1;
}

std::string Builder::finish(const std::string& bin_uri) {
    while (bin.size() % 4) bin.push_back(0);
    if (bin.empty()) return write_json(json);   // nothing to store (an empty model): no buffer (byteLength must be >= 1)
    auto buffers = Json::make_array();
    auto b = Json::make_object();
    b["byteLength"] = bin.size();
    b["uri"] = bin_uri;
    buffers.push(std::move(b));
    json["buffers"] = std::move(buffers);
    return write_json(json);
}

std::vector<std::uint8_t> Builder::finish_glb() {
    while (bin.size() % 4) bin.push_back(0);
    auto buffers = Json::make_array();
    auto b = Json::make_object();
    b["byteLength"] = bin.size();
    buffers.push(std::move(b));
    json["buffers"] = std::move(buffers);
    auto text = write_json(json);
    while (text.size() % 4) text.push_back(' ');
    std::vector<std::uint8_t> out;
    const auto put = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    put(2);
    put(static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
    put(static_cast<std::uint32_t>(text.size()));
    put(0x4e4f534a);
    out.insert(out.end(), text.begin(), text.end());
    put(static_cast<std::uint32_t>(bin.size()));
    put(0x004e4942);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

}  // namespace omega::gltf
