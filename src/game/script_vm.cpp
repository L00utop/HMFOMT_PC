#include "script_vm.hpp"

#include <algorithm>
#include <cstring>

namespace hm::game {

using namespace hm::hal;

namespace {

inline u32 read_u32_le(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

inline void append_u32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

} // namespace

void ScriptVm::set_on_call(OnCallHandler handler) {
    on_call_ = std::move(handler);
}

void ScriptVm::reset() {
    active_ = 0;
    buffer_.clear();
    code_offset_ = 0;
    code_length_ = 0;
    pc_ = 0;
    string_count_ = 0;
    string_table_offset_ = 0;
    string_pool_offset_ = 0;
    jump_count_ = 0;
    jump_table_offset_ = 0;
    stack_.fill(0);
    stack_top_ = 0;
    vmmem_.fill(0);
    pc_base_bias_ = 0;
}

bool ScriptVm::load(const void* script_data, std::size_t size) {
    reset();
    if (!script_data || size < 12) return false;

    buffer_.assign(static_cast<const u8*>(script_data), static_cast<const u8*>(script_data) + size);

    const u32 riff_magic = read_u32_le(&buffer_[0]);
    const u32 riff_len   = read_u32_le(&buffer_[4]);
    const u32 scr_magic  = read_u32_le(&buffer_[8]);

    // 'RIFF' (0x46464952) and 'SCR ' (0x20524353)
    if (riff_magic != 0x46464952u || scr_magic != 0x20524353u) {
        reset();
        return false;
    }

    const u32 max_end = std::min<u32>(static_cast<u32>(buffer_.size()), 8u + riff_len);
    u32 offset = 12u;
    bool found_code = false;

    while (offset + 8u <= max_end) {
        const u32 chunk_magic = read_u32_le(&buffer_[offset]);
        const u32 chunk_len   = read_u32_le(&buffer_[offset + 4u]);
        offset += 8u;

        if (offset + chunk_len > buffer_.size()) break;

        if (chunk_magic == 0x45444F43u && chunk_len >= 4u) { // 'CODE'
            code_length_ = read_u32_le(&buffer_[offset]);
            code_offset_ = offset + 4u;
            if (code_offset_ + code_length_ <= buffer_.size()) {
                pc_ = 0;
                found_code = true;
            }
        } else if (chunk_magic == 0x20525453u && chunk_len >= 4u) { // 'STR '
            string_count_ = read_u32_le(&buffer_[offset]);
            string_table_offset_ = offset + 4u;
            string_pool_offset_  = string_table_offset_ + string_count_ * 4u;
        } else if (chunk_magic == 0x504D554Au && chunk_len >= 4u) { // 'JUMP'
            jump_count_ = read_u32_le(&buffer_[offset]);
            jump_table_offset_ = offset + 4u;
        }

        offset += chunk_len;
    }

    if (!found_code) {
        reset();
        return false;
    }

    active_ = 1;
    return true;
}

void ScriptVm::push(i32 val) {
    if (stack_top_ < STACK_MAX) {
        stack_[stack_top_++] = val;
    }
}

i32 ScriptVm::pop() {
    if (stack_top_ == 0) return 0;
    return stack_[--stack_top_];
}

i32 ScriptVm::top() const {
    return (stack_top_ > 0) ? stack_[stack_top_ - 1] : 0;
}

i32 ScriptVm::get_var(std::size_t idx) const {
    return (idx < VMMEM_MAX) ? vmmem_[idx] : 0;
}

void ScriptVm::set_var(std::size_t idx, i32 val) {
    if (idx < VMMEM_MAX) vmmem_[idx] = val;
}

const char* ScriptVm::get_string(u32 id) const {
    if (id >= string_count_) return "Error";
    const u32 entry_off = string_table_offset_ + id * 4u;
    if (entry_off + 4u > buffer_.size()) return "Error";
    const u32 str_rel = read_u32_le(&buffer_[entry_off]);
    const u32 str_abs = string_pool_offset_ + str_rel;
    if (str_abs >= buffer_.size()) return "Error";
    return reinterpret_cast<const char*>(&buffer_[str_abs]);
}

u8 ScriptVm::opcode(u32 offset) const {
    return (offset < code_length_) ? (buffer_[code_offset_ + offset] & 0x7Fu) : 0xFFu;
}

u8 ScriptVm::opcode_flag(u32 offset) const {
    return (offset < code_length_) ? (buffer_[code_offset_ + offset] >> 7) : 0u;
}

u32 ScriptVm::operand32(u32 offset) const {
    if (offset + 5u > code_length_) return 0;
    u32 r = read_u32_le(&buffer_[code_offset_ + offset + 1u]);
    if (opcode_flag(offset) != 0) r = static_cast<u32>(static_cast<i32>(r) + pc_base_bias_);
    return r;
}

u32 ScriptVm::operand16(u32 offset) const {
    if (offset + 3u > code_length_) return 0;
    const u8* p = &buffer_[code_offset_ + offset + 1u];
    u32 r = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8);
    if (opcode_flag(offset) != 0) r = static_cast<u32>(static_cast<i32>(r) + pc_base_bias_);
    return r;
}

u32 ScriptVm::operand8(u32 offset) const {
    if (offset + 2u > code_length_) return 0;
    u32 r = buffer_[code_offset_ + offset + 1u];
    if (opcode_flag(offset) != 0) r = static_cast<u32>(static_cast<i32>(r) + pc_base_bias_);
    return r;
}

int ScriptVm::run_until_yield() {
    if (active_ != 1) return active_;

    for (int safety = 0; safety < 10000; ++safety) {
        const int r = step_instruction();
        if (r == 0) continue;
        if (r == 1) return 1; // Yielded for host UI/frame
        // 2 = normal halt, < 0 = runtime error
        active_ = 0;
        return 0;
    }
    return active_;
}

int ScriptVm::step_instruction() {
    if (pc_ >= code_length_) return -1;

    auto top_ref = [&]() -> i32& {
        static i32 dummy = 0;
        if (stack_top_ == 0) return dummy;
        return stack_[stack_top_ - 1];
    };

    switch (opcode(pc_)) {
        case 0x00: pc_ += 1; break; // NOP

        case 0x01: { // STORE var
            const i32 op2 = pop();
            const std::size_t op1 = static_cast<std::size_t>(top());
            if (op1 < VMMEM_MAX) vmmem_[op1] = op2;
            top_ref() = op2;
            pc_ += 1;
            break;
        }
        case 0x02: { // ADD_ASSIGN
            const i32 op2 = pop();
            const std::size_t op1 = static_cast<std::size_t>(top());
            const i32 val = (op1 < VMMEM_MAX ? vmmem_[op1] : 0) + op2;
            if (op1 < VMMEM_MAX) vmmem_[op1] = val;
            top_ref() = val;
            pc_ += 1;
            break;
        }
        case 0x03: { // SUB_ASSIGN
            const i32 op2 = pop();
            const std::size_t op1 = static_cast<std::size_t>(top());
            const i32 val = (op1 < VMMEM_MAX ? vmmem_[op1] : 0) - op2;
            if (op1 < VMMEM_MAX) vmmem_[op1] = val;
            top_ref() = val;
            pc_ += 1;
            break;
        }
        case 0x04: { // MUL_ASSIGN
            const i32 op2 = pop();
            const std::size_t op1 = static_cast<std::size_t>(top());
            const i32 val = (op1 < VMMEM_MAX ? vmmem_[op1] : 0) * op2;
            if (op1 < VMMEM_MAX) vmmem_[op1] = val;
            top_ref() = val;
            pc_ += 1;
            break;
        }
        case 0x05: { // DIV_ASSIGN
            const i32 op2 = pop();
            if (op2 == 0) return -2;
            const std::size_t op1 = static_cast<std::size_t>(top());
            const i32 val = (op1 < VMMEM_MAX ? vmmem_[op1] : 0) / op2;
            if (op1 < VMMEM_MAX) vmmem_[op1] = val;
            top_ref() = val;
            pc_ += 1;
            break;
        }
        case 0x06: { // MOD_ASSIGN
            const i32 op2 = pop();
            if (op2 == 0) return -2;
            const std::size_t op1 = static_cast<std::size_t>(top());
            const i32 val = (op1 < VMMEM_MAX ? vmmem_[op1] : 0) % op2;
            if (op1 < VMMEM_MAX) vmmem_[op1] = val;
            top_ref() = val;
            pc_ += 1;
            break;
        }
        case 0x07: { const i32 op2 = pop(); top_ref() = top() + op2; pc_ += 1; break; }
        case 0x08: { const i32 op2 = pop(); top_ref() = top() - op2; pc_ += 1; break; }
        case 0x09: { const i32 op2 = pop(); top_ref() = top() * op2; pc_ += 1; break; }
        case 0x0A: { const i32 op2 = pop(); if (op2 == 0) return -2; top_ref() = top() / op2; pc_ += 1; break; }
        case 0x0B: { const i32 op2 = pop(); if (op2 == 0) return -2; top_ref() = top() % op2; pc_ += 1; break; }
        case 0x0C: { const i32 op2 = pop(); top_ref() = (top() && op2) ? 1 : 0; pc_ += 1; break; }
        case 0x0D: { const i32 op2 = pop(); top_ref() = (top() || op2) ? 1 : 0; pc_ += 1; break; }
        case 0x0E: { top_ref() = top() + 1; pc_ += 1; break; }
        case 0x0F: { top_ref() = top() - 1; pc_ += 1; break; }
        case 0x10: { top_ref() = -top(); pc_ += 1; break; }
        case 0x11: { top_ref() = !top() ? 1 : 0; pc_ += 1; break; }
        case 0x12: { // CMP
            const i32 op2 = pop();
            const i32 op1 = pop();
            push(op1 > op2 ? 1 : (op1 < op2 ? -1 : 0));
            pc_ += 1;
            break;
        }
        case 0x13: { // PUSH_VAR
            const u32 var = operand32(pc_);
            push(get_var(var));
            pc_ += 5;
            break;
        }
        case 0x14: { // POP_STORE_VAR
            const u32 var = operand32(pc_);
            set_var(var, pop());
            pc_ += 5;
            break;
        }
        case 0x15: { push(top()); pc_ += 1; break; }
        case 0x16: { pop(); pc_ += 1; break; }
        case 0x17: { push(static_cast<i32>(operand32(pc_))); pc_ += 5; break; }
        case 0x18: { pc_ = operand32(pc_); break; }
        case 0x19: { const u32 t = operand32(pc_); pc_ += 5; if (pop() < 0) pc_ = t; break; }
        case 0x1A: { const u32 t = operand32(pc_); pc_ += 5; if (pop() <= 0) pc_ = t; break; }
        case 0x1B: { const u32 t = operand32(pc_); pc_ += 5; if (pop() == 0) pc_ = t; break; }
        case 0x1C: { const u32 t = operand32(pc_); pc_ += 5; if (pop() != 0) pc_ = t; break; }
        case 0x1D: { const u32 t = operand32(pc_); pc_ += 5; if (pop() >= 0) pc_ = t; break; }
        case 0x1E: { const u32 t = operand32(pc_); pc_ += 5; if (pop() > 0) pc_ = t; break; }
        case 0x1F: { pc_ = static_cast<u32>(pop()); break; }
        case 0x20: { pc_ += 1; return 2; } // END
        case 0x21: { // CALL
            const i32 call_id = static_cast<i32>(operand32(pc_));
            pc_ += 5;
            return on_call_ ? on_call_(*this, call_id) : 0;
        }
        case 0x22: { push(static_cast<i32>(operand16(pc_))); pc_ += 3; break; }
        case 0x23: { push(static_cast<i32>(operand8(pc_))); pc_ += 2; break; }
        case 0x24: { // SWITCH / JUMP TABLE
            const u32 jt_idx = operand32(pc_);
            const u32 jt_ptr_off = jump_table_offset_ + jt_idx * 4u;
            if (jt_ptr_off + 4u <= buffer_.size()) {
                const u32 jt_off = jump_table_offset_ + read_u32_le(&buffer_[jt_ptr_off]);
                if (jt_off + 8u <= buffer_.size()) {
                    const u32 count = read_u32_le(&buffer_[jt_off]);
                    const i32 def_target = static_cast<i32>(read_u32_le(&buffer_[jt_off + 4u]));
                    const i32 key = pop();

                    std::vector<JumpEntry> entries(count);
                    for (u32 i = 0; i < count && (jt_off + 8u + (i + 1u) * 8u <= buffer_.size()); ++i) {
                        entries[i].value  = static_cast<i32>(read_u32_le(&buffer_[jt_off + 8u + i * 8u]));
                        entries[i].target = read_u32_le(&buffer_[jt_off + 12u + i * 8u]);
                    }
                    auto it = std::lower_bound(entries.begin(), entries.end(), key,
                        [](const JumpEntry& a, i32 b) { return a.value < b; });
                    if (it != entries.end() && it->value == key) {
                        pc_ = it->target;
                        return 0;
                    }
                    if (def_target > 0) {
                        pc_ = static_cast<u32>(def_target);
                        return 0;
                    }
                }
            }
            pc_ += 5;
            break;
        }
        default:
            return -1;
    }
    return 0;
}

std::vector<u8> ScriptVm::build_riff_script(
    const std::vector<u8>& code,
    const std::vector<std::string>& strings) {

    std::vector<u8> code_chunk;
    append_u32(code_chunk, 0x45444F43u); // 'CODE'
    append_u32(code_chunk, static_cast<u32>(4u + code.size()));
    append_u32(code_chunk, static_cast<u32>(code.size()));
    code_chunk.insert(code_chunk.end(), code.begin(), code.end());

    std::vector<u8> str_pool;
    std::vector<u32> offsets;
    offsets.reserve(strings.size());
    for (const auto& s : strings) {
        offsets.push_back(static_cast<u32>(str_pool.size()));
        str_pool.insert(str_pool.end(), s.begin(), s.end());
        str_pool.push_back(0);
    }

    std::vector<u8> str_chunk;
    append_u32(str_chunk, 0x20525453u); // 'STR '
    append_u32(str_chunk, static_cast<u32>(4u + offsets.size() * 4u + str_pool.size()));
    append_u32(str_chunk, static_cast<u32>(offsets.size()));
    for (u32 off : offsets) append_u32(str_chunk, off);
    str_chunk.insert(str_chunk.end(), str_pool.begin(), str_pool.end());

    const u32 payload_len = static_cast<u32>(4u + code_chunk.size() + str_chunk.size());
    std::vector<u8> out;
    append_u32(out, 0x46464952u); // 'RIFF'
    append_u32(out, payload_len);
    append_u32(out, 0x20524353u); // 'SCR '
    out.insert(out.end(), code_chunk.begin(), code_chunk.end());
    out.insert(out.end(), str_chunk.begin(), str_chunk.end());
    return out;
}

} // namespace hm::game
