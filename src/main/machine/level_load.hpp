#pragma once

#include <cstdint>
#include <initializer_list>
#include <unordered_map>

namespace omega {

// The caller owns the EE-visible memory. Addresses are the original EE addresses;
// missing bytes read as zero, so each test supplies only its relevant inputs.
class LevelLoadMemory {
public:
    std::uint32_t read(std::uint32_t address, unsigned size = 4) const;
    void write(std::uint32_t address, unsigned size, std::uint32_t value);
    void copy(std::uint32_t address, const char* text);
    const std::unordered_map<std::uint32_t, std::uint8_t>& bytes() const { return bytes_; }
    // Opt-in strict mode for a mirror of partly modelled state: after seal(),
    // only bytes already present may be read or written; any other access
    // throws instead of reading an unmodelled zero or dropping a store.
    void seal() { sealed_ = true; }
    bool sealed() const { return sealed_; }
private:
    [[noreturn]] static void unmodelled(std::uint32_t address, const char* access);
    std::unordered_map<std::uint32_t, std::uint8_t> bytes_;
    bool sealed_{};
};

// Each external call is identified by its original function address. The host
// may update memory (constructors, file operations, script runtime) and returns
// the EE v0 word. An absent service must throw; this layer never substitutes a
// fabricated asset or renderer result.
class LevelLoadHost {
public:
    virtual ~LevelLoadHost() = default;
    virtual std::uint32_t call(LevelLoadMemory& memory, std::uint32_t address,
                               std::initializer_list<std::uint32_t> args);
    // 001684a4 reads a 64-bit heap value; 001684c4 supplies a sign-extended
    // 64-bit argument. These methods preserve those EE register widths.
    virtual std::uint64_t call_return64(LevelLoadMemory& memory, std::uint32_t address,
                                        std::initializer_list<std::uint32_t> args);
    virtual std::uint32_t call_args64(LevelLoadMemory& memory, std::uint32_t address,
                                      std::initializer_list<std::uint64_t> args);
    // Services that also take single-precision arguments in $f12, $f13, ...
    // (00168784, 00168798, 001687c4, 00168ba0). Floats travel as raw bits.
    virtual std::uint32_t call_float(LevelLoadMemory& memory, std::uint32_t address,
                                     std::initializer_list<std::uint32_t> args,
                                     std::initializer_list<std::uint32_t> float_bits);
    // A callee that keeps objects in its own frame below the caller's $sp (sp at
    // the jal). The default runs the native 00166190 (run_app_teardown) and
    // forwards any other address to call(); a recording host may override it to
    // keep 00166190 at its boundary.
    virtual std::uint32_t call_framed(LevelLoadMemory& memory, std::uint32_t address,
                                      std::initializer_list<std::uint32_t> args, std::uint32_t sp);
    // Observation boundaries around the already-native 001350e0 body. They
    // let a caller host inspect or alter state at the original call boundary.
    virtual void before_level_load(LevelLoadMemory& memory);
    virtual void after_level_load(LevelLoadMemory& memory);
};

// Port of 001350e0..00135960. The scratch stack base matches the interpreter
// harness. The native operation itself writes globals and object fields exactly
// where the function does; callees stay at explicit host boundaries.
void run_level_load(LevelLoadMemory& memory, LevelLoadHost& host,
                    std::uint32_t game = 0x4ffb50, std::uint32_t stack = 0x1f00000);

}  // namespace omega
