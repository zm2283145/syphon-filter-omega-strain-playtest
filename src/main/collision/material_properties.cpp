#include "material_properties.hpp"

#include "script_context.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace omega {

MaterialProperty default_material_property() {
    MaterialProperty p;
    p.size = std::bit_cast<float>(0x3e4ccccdU);
    return p;
}

int material_stricmp(const char* a, const char* b) {
    const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 0x20 : int(c); };
    for (;; ++a, ++b) {
        const int x = fold(static_cast<unsigned char>(*a)), y = fold(static_cast<unsigned char>(*b));
        if (x != y) return x - y;
        if (x == 0) return 0;
    }
}

MaterialProperty make_material_property(const char* effect, std::int32_t bank, const char* sound,
                                        const char* decal, float size, std::uint8_t blend) {
    MaterialProperty p;
    p.size = size;
    p.blend = blend;
    if (effect && material_stricmp(effect, "none") == 0) p.effect_id = -2;
    else { p.effect = effect ? effect : ""; p.effect_id = -1; }
    if (sound && material_stricmp(sound, "none") == 0) p.bank = -2;
    else if (!sound || !*sound) p.bank = -1;
    else { p.bank = bank; p.sound = sound; }
    if (decal && material_stricmp(decal, "none") == 0) p.decal_id = -2;
    else { p.decal = decal ? decal : ""; p.decal_id = -1; }
    return p;
}

void MaterialPropertyTable::initialise() {
    initialised_ = true;
    rows_.clear();
    index_.clear();
    MaterialRow row;
    row.name = "DEFAULT";
    row.props.fill(default_material_property());
    rows_.push_back(row);
    index_.emplace(row.name, std::uint8_t(0));
}

bool MaterialPropertyTable::add(const std::string& name, std::size_t matprop, const MaterialProperty& prop) {
    if (matprop >= material_property_count)
        throw std::runtime_error("Material table: property index outside MaterialProperties_MatPropName (004085a0)");
    if (!initialised_) initialise();
    if (const auto it = index_.find(name); it != index_.end()) {
        rows_[it->second].props[matprop] = prop;
        return true;
    }
    if (rows_.size() >= material_row_limit) return false;
    MaterialRow row;
    row.name = name;
    row.props.fill(default_material_property());
    row.props[matprop] = prop;
    rows_.push_back(std::move(row));
    index_.emplace(name, static_cast<std::uint8_t>(rows_.size() - 1));
    return true;
}

std::uint8_t MaterialPropertyTable::lookup(const char* name) const {
    if (!initialised_ || !name || !*name) return 0;
    const auto it = index_.find(name);
    return it == index_.end() ? 0 : it->second;
}

const MaterialRow& MaterialPropertyTable::row(std::size_t index) const {
    if (index >= rows_.size())
        throw std::runtime_error("Material table: row " + std::to_string(index) + " outside the table (00189560)");
    return rows_[index];
}

void MaterialPropertyTable::resolve_effects(const std::function<std::int32_t(const std::string&)>& request,
                                            const std::function<std::int32_t(const std::string&)>& texture) {
    if (!initialised_) initialise();
    std::map<std::string, std::int32_t> decals;   // sp+1a0 (0040afe0 .. 00408d80)
    for (auto& r : rows_)
        for (auto& p : r.props) {
            if (!p.effect.empty()) {
                if (!request) throw std::runtime_error("Material effects: the particle request 00398070 is not hosted");
                p.effect_id = request(p.effect);      // 00398070(0053b540, name, &+c)
                p.effect.clear();                     // 0013b500(+0, 0, -1)
            }
            if (p.decal.empty()) continue;
            if (const auto it = decals.find(p.decal); it != decals.end()) {
                p.decal_id = it->second;              // 00409870 hit: node +18
            } else {
                std::string name = p.decal;           // 00129360 into sp+160
                if (name.find(".tdx") == std::string::npos) name += ".tdx";   // 00129c80 / 00128f58 (004beb78)
                if (!texture) throw std::runtime_error("Material effects: the decal texture load 00380a40 is not hosted");
                p.decal_id = texture(name);           // 0036e1b0(ctx, name, level file, 0), 00380a40(*00539248, ctx)
                decals.emplace(p.decal, p.decal_id);  // 00408e30, 0040aec0
            }
            p.decal.clear();                          // 0013b500(+20, 0, -1)
        }
}

std::uint8_t material_surface_index(const MaterialPropertyTable& table, std::span<const std::uint8_t> surface) {
    if (surface.size() < 16) throw std::runtime_error("Material surface: a COL surface record is 16 bytes (003c20c8)");
    std::string name;
    for (std::size_t i = 2; i < 16; ++i) {
        if (surface[i] == 0) return table.lookup(name.c_str());
        name.push_back(static_cast<char>(surface[i]));
    }
    throw std::runtime_error("Material surface: a COL surface name without a NUL (003c20c8)");
}

namespace {

class PreloadHost final : public Host {
public:
    explicit PreloadHost(MaterialPropertyTable& table) : table_(table) {}
    std::optional<Value> call(const Script&, const Function& function, const std::vector<Value>& arguments) override {
        if (function.name != "game::AddMaterialProperties")
            throw std::runtime_error("Material preload: unhosted native " + function.name);
        if (arguments.size() < 6 || arguments.size() > 8)
            throw std::runtime_error("Material preload: AddMaterialProperties with " + std::to_string(arguments.size()) + " arguments");
        const auto& name = text(arguments[0]);
        const auto matprop = word(arguments[1]) & 0xffU;                                   // lbu +4
        const auto bank = static_cast<std::int32_t>(static_cast<std::int8_t>(word(arguments[3])));  // lb +c
        // 00267620: size 0.2, blend 0; 002675b0: the size, blend 0; 00267540: both.
        const float size = arguments.size() >= 7 ? std::bit_cast<float>(word(arguments[6])) : std::bit_cast<float>(0x3e4ccccdU);
        const auto blend = arguments.size() == 8 ? static_cast<std::uint8_t>(word(arguments[7])) : std::uint8_t(0);
        const auto prop = make_material_property(text(arguments[2]).c_str(), bank, text(arguments[4]).c_str(),
                                                 text(arguments[5]).c_str(), size, blend);
        return Value{Word(table_.add(name, matprop, prop) ? 1U : 0U)};
    }
    Word enum_value(const std::string& name, Word) override {
        throw std::runtime_error("Material preload: unregistered enum " + name);
    }

private:
    static const std::string& text(const Value& v) {
        if (const auto* s = std::get_if<std::string>(&v)) return *s;
        throw std::runtime_error("Material preload: AddMaterialProperties expects a string");
    }
    static Word word(const Value& v) {
        if (const auto* w = std::get_if<Word>(&v)) return *w;
        throw std::runtime_error("Material preload: AddMaterialProperties expects a word");
    }
    MaterialPropertyTable& table_;
};

}

MaterialPropertyTable run_material_preload(const std::map<std::string, Script>& modules) {
    MaterialPropertyTable table;
    PreloadHost host(table);
    ScriptContext context(host, [&](const std::string& name) -> std::optional<Script> {
        const auto it = modules.find(name);
        if (it == modules.end()) return std::nullopt;
        return it->second;
    });
    if (!context.load("init")) throw std::runtime_error("Material preload: the mission has no init script");
    context.execute_init_code();
    // 003e1b60: when 003e0f50 finds no function of that name and parameter
    // count it calls the empty printf 003d7930 ("error!  function %s with %d
    // params not found!", 004bd920) and returns 0, so a mission whose init
    // has no preload (TRAINING) keeps the empty table.
    const auto& init = modules.at("init");
    const bool defined = std::any_of(init.functions.begin(), init.functions.end(), [](const auto& function) {
        return !function.external && function.name == "init::preload" && function.parameters.empty();
    });
    if (defined) context.invoke("init::preload");
    return table;
}

}
