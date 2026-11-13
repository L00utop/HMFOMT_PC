#pragma once

#include "hal/gba_types.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace hm::game {

// Clean C++17 port of StanHash/fomt's AScriptEngine (src/script_engine.cc),
// compatible with 'RIFF' / 'SCR ' bytecode compiled by StanHash/mary.
class ScriptVm {
public:
    static constexpr std::size_t STACK_MAX = 100;
    static constexpr std::size_t VMMEM_MAX = 100;

    // Host procedure callback invoked on opcode 0x21 (CALL id).
    // Return: 0 = continue executing, 1 = yield/pause VM for frame/dialog, 2 = end script.
    using OnCallHandler = std::function<int(ScriptVm& vm, hal::i32 call_id)>;

    ScriptVm() = default;

    void set_on_call(OnCallHandler handler);
    bool load(const void* script_data, std::size_t size);
    void reset();

    // Runs instructions until yield (1), end (2), or error (<0).
    // Returns active state (1 = running/yielded, 0 = finished/idle).
    int run_until_yield();
    int step_instruction();

    bool is_active() const { return active_ != 0; }
    int state() const { return active_; }

    const char* get_string(hal::u32 id) const;
    hal::u32 string_count() const { return string_count_; }

    // Stack & VM memory accessors for OnCall host procedures
    void push(hal::i32 val);
    hal::i32 pop();
    hal::i32 top() const;
    std::size_t stack_size() const { return stack_top_; }

    hal::i32 get_var(std::size_t idx) const;
    void set_var(std::size_t idx, hal::i32 val);

    // Helper: builds a valid RIFF/SCR binary container (CODE + STR) for tests and built-in events
    static std::vector<hal::u8> build_riff_script(
        const std::vector<hal::u8>& code,
        const std::vector<std::string>& strings);

private:
    struct JumpEntry {
        hal::i32 value;
        hal::u32 target;
    };

    hal::u8  opcode(hal::u32 offset) const;
    hal::u8  opcode_flag(hal::u32 offset) const;
    hal::u32 operand32(hal::u32 offset) const;
    hal::u32 operand16(hal::u32 offset) const;
    hal::u32 operand8(hal::u32 offset) const;

    int active_ = 0;
    std::vector<hal::u8> buffer_;

    hal::u32 code_offset_ = 0;
    hal::u32 code_length_ = 0;
    hal::u32 pc_ = 0;

    hal::u32 string_count_ = 0;
    hal::u32 string_table_offset_ = 0;
    hal::u32 string_pool_offset_ = 0;

    hal::u32 jump_count_ = 0;
    hal::u32 jump_table_offset_ = 0;

    std::array<hal::i32, STACK_MAX> stack_{};
    std::size_t stack_top_ = 0;
    std::array<hal::i32, VMMEM_MAX> vmmem_{};
    hal::i32 pc_base_bias_ = 0;

    OnCallHandler on_call_;
};

} // namespace hm::game
