// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <boost/container/stable_vector.hpp>
#include <boost/container/small_vector.hpp>

#include "dynarmic/backend/x64/xbyak.h"
#include "dynarmic/mcl/bit.hpp"
#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/location_descriptor.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/value.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::IR {
class Block;
class Inst;
}  // namespace Dynarmic::IR

namespace Dynarmic {
enum class OptimizationFlag : u32;
}  // namespace Dynarmic

namespace Dynarmic::Backend::X64 {

class A64EmitX64;
class BlockOfCode;

using A64FullVectorWidth = std::integral_constant<size_t, 128>;

// Array alias that always sizes itself according to the given type T
// relative to the size of a vector register. e.g. T = u32 would result
// in a std::array<u32, 4>.
template<typename T>
using VectorArray = std::array<T, A64FullVectorWidth::value / mcl::bitsizeof<T>>;

template<typename T>
using HalfVectorArray = std::array<T, A64FullVectorWidth::value / mcl::bitsizeof<T> / 2>;

using SharedLabel = Xbyak::Label*;
// A U128 known to be zero above its low 64 (IsZeroExtendedLong) or 32 (IsZeroExtendedWord) bits:
// ZeroExtendLongToQuad (of ZeroExtendWordToLong), or a GetD/GetS load (movq/movd). These always
// clear the bits, whichever registers they used.
inline bool IsZeroExtendedWord(const IR::Value& value) {
    if (value.IsImmediate())
        return false;
    const IR::Inst* const inst = value.GetInstRecursive();
    if (inst->GetOpcode() == IR::Opcode::A64GetS)
        return true;
    if (inst->GetOpcode() != IR::Opcode::ZeroExtendLongToQuad)
        return false;
    const IR::Value along = inst->GetArg(0);
    return !along.IsImmediate() && along.GetInstRecursive()->GetOpcode() == IR::Opcode::ZeroExtendWordToLong;
}

inline bool IsZeroExtendedLong(const IR::Value& value) {
    if (value.IsImmediate())
        return false;
    const auto opcode = value.GetInstRecursive()->GetOpcode();
    return opcode == IR::Opcode::ZeroExtendLongToQuad || opcode == IR::Opcode::A64GetD || IsZeroExtendedWord(value);
}

/// A U32 in a vector register known to be zero above its low 32 bits (HostRegisterLocality):
/// a lane-0 VectorGetElement32 (an aliased clean source, an insertps, or a movd from a GPR), or a
/// scalar add/sub/mul/div of two such values - FPThreeOp keeps op1's upper lanes, and its NaN paths
/// write a zero-extended constant or op2 (hence both). Anything else counts as not known.
inline bool IsCleanScalar32(const IR::Value& value, int depth = 0) {
    if (value.IsImmediate() || depth > 8)
        return false;
    const IR::Inst* const inst = value.GetInstRecursive();
    switch (inst->GetOpcode()) {
    case IR::Opcode::VectorGetElement32:
        return inst->GetArg(1).IsImmediate() && inst->GetArg(1).GetU8() == 0;
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPMul32:
    case IR::Opcode::FPDiv32:
        return IsCleanScalar32(inst->GetArg(0), depth + 1) && IsCleanScalar32(inst->GetArg(1), depth + 1);
    default:
        return false;
    }
}

struct EmitContext {
    EmitContext(RegAlloc& reg_alloc, IR::Block& block, boost::container::stable_vector<Xbyak::Label>& shared_labels);
    virtual ~EmitContext();
    virtual FP::FPCR FPCR(bool fpcr_controlled = true) const = 0;
    virtual bool HasOptimization(OptimizationFlag flag) const = 0;

    [[nodiscard]] inline Xbyak::Label* GenSharedLabel() noexcept {
        return &shared_labels.emplace_back();
    }

    std::vector<std::function<void()>> deferred_emits;
    /// Add64(base, imm) whose only use is a fastmem access's address: emitted as nothing (aliasing
    /// base), the access adds imm as its displacement (HostRegisterLocality, A64).
    std::vector<const IR::Inst*> folded_address_adds;
    bool IsFoldedAddressAdd(const IR::Inst* inst) const {
        return !folded_address_adds.empty()
            && std::find(folded_address_adds.begin(), folded_address_adds.end(), inst) != folded_address_adds.end();
    }
    RegAlloc& reg_alloc;
    IR::Block& block;
    boost::container::stable_vector<Xbyak::Label>& shared_labels;
};

class EmitX64 {
public:
    struct BlockDescriptor {
        CodePtr entrypoint;  // Entrypoint of emitted code
        size_t size;         // Length in bytes of emitted code
    };
    static_assert(sizeof(BlockDescriptor) == 16);

    explicit EmitX64(BlockOfCode& code);
    virtual ~EmitX64();

    /// Looks up an emitted host block in the cache.
    std::optional<BlockDescriptor> GetBasicBlock(IR::LocationDescriptor descriptor) const;

    /// Empties the entire cache.
    virtual void ClearCache();

    /// Invalidates a selection of basic blocks.
    void InvalidateBasicBlocks(const ::Common::unordered_set<IR::LocationDescriptor>& locations);

//protected:
    // Microinstruction emitters
#define OPCODE(name, type, ...) void Emit##name(EmitContext& ctx, IR::Inst* inst);
#define A32OPC(...)
#define A64OPC(...)
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC
    void EmitInvalid(EmitContext& ctx, IR::Inst* inst);

    // Helpers
    virtual std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const = 0;
    void EmitAddCycles(size_t cycles);
    Xbyak::Label EmitCond(IR::Cond cond);
    /// Set around EmitTerminal when the host flags still hold the guest NZCV just stored (see
    /// A64EmitX64::Emit): the next EmitCond uses them instead of reloading them.
    bool nzcv_in_host_flags = false;
    BlockDescriptor RegisterBlock(const IR::LocationDescriptor& location_descriptor, CodePtr entrypoint, size_t size);
    void PushRSBHelper(Xbyak::Reg64 loc_desc_reg, Xbyak::Reg64 index_reg, IR::LocationDescriptor target);

#ifndef NDEBUG
    void EmitVerboseDebuggingOutput(RegAlloc& reg_alloc);
#endif
    virtual bool EmitLeafTerminal(IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept = 0;
    virtual bool EmitTerminal(IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept = 0;

    // Patching
    struct PatchInformation {
        boost::container::small_vector<CodePtr, 4> jg; //4*8=32
        boost::container::small_vector<CodePtr, 4> jz; //4*8=32
        boost::container::small_vector<CodePtr, 4> jmp; //4*8=32
        boost::container::small_vector<CodePtr, 4> mov_rcx; //4*8=32
    };
    void Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr);
    virtual void Unpatch(const IR::LocationDescriptor& target_desc);
    virtual void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) = 0;

    // State
    BlockOfCode& code;
    ExceptionHandler exception_handler;
    ::Common::unordered_map<IR::LocationDescriptor, BlockDescriptor> block_descriptors;
    ::Common::unordered_map<IR::LocationDescriptor, PatchInformation> patch_information;

    // We need materialized protected members
    friend class A64EmitX64;
};

}  // namespace Dynarmic::Backend::X64
