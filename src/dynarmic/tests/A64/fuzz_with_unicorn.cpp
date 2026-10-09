// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include "common/common_types.h"

#include "dynarmic/tests/fuzz_util.h"
#include "dynarmic/tests/rand_int.h"
#include "dynarmic/tests/unicorn_emu/a64_unicorn.h"
#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/frontend/A64/decoder/a64.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/opt_passes.h"

// Must be declared last for all necessary operator<< to be declared prior to this.
#include <fmt/format.h>
#include <fmt/ostream.h>

using namespace Dynarmic;

// Instructions the generators skip: ones whose translation hits a fatal assert in this tree
// (FP16 forms of the complex-number instructions).
static bool SkipInFuzzing(u32 instruction) {
    static const std::vector<InstructionGenerator> skipped = [] {
        const std::vector<std::tuple<std::string, const char*>> list{
#define INST(fn, name, bitstring) {#fn, bitstring},
#include "dynarmic/frontend/A64/decoder/a64.inc"
#undef INST
        };
        std::vector<InstructionGenerator> result;
        for (const auto& [fn, bitstring] : list)
            if (fn.starts_with("FCADD") || fn.starts_with("FCMLA"))
                result.emplace_back(InstructionGenerator{bitstring});
        return result;
    }();
    // The 0xCE crypto group (SHA512, SM3, SM4, ...): its emitters use absolute addresses that
    // do not encode when the code buffer is far from the binary's data.
    return (instruction >> 24) == 0xCE
        || std::any_of(skipped.begin(), skipped.end(), [&](const auto& g) { return g.Match(instruction); });
}

static bool ShouldTestInst(u32 instruction, u64 pc, bool is_last_inst) {
    const A64::LocationDescriptor location{pc, {}};
    IR::Block block{location};
    bool should_continue = A64::TranslateSingleInstruction(block, location, instruction);
    if (!should_continue && !is_last_inst)
        return false;
    for (const auto& ir_inst : block.instructions) {
        switch (ir_inst.GetOpcode()) {
        case IR::Opcode::A64ExceptionRaised:
        case IR::Opcode::A64CallSupervisor:
        case IR::Opcode::A64DataCacheOperationRaised:
        case IR::Opcode::A64GetCNTPCT:
            return false;
        default:
            continue;
        }
    }
    return true;
}

static u32 GenRandomInst(u64 pc, bool is_last_inst) {
    static const struct InstructionGeneratorInfo {
        std::vector<InstructionGenerator> generators;
        std::vector<InstructionGenerator> invalid;
    } instructions = [] {
        const std::vector<std::tuple<std::string, const char*>> list{
#define INST(fn, name, bitstring) {#fn, bitstring},
#include "dynarmic/frontend/A64/decoder/a64.inc"
#undef INST
        };

        std::vector<InstructionGenerator> generators;
        std::vector<InstructionGenerator> invalid;

        // List of instructions not to test
        const std::vector<std::string> do_not_test{
            // Unimplemented in QEMU
            "STLLR",
            // Unimplemented in QEMU
            "LDLAR",
            // Dynarmic and QEMU currently differ on how the exclusive monitor's address range works.
            "STXR",
            "STLXR",
            "STXP",
            "STLXP",
            "LDXR",
            "LDAXR",
            "LDXP",
            "LDAXP",
            // Behaviour differs from QEMU
            "MSR_reg",
            "MSR_imm",
            "MRS",
            // Does not need test
            "SVC",
            "BRK"
        };

        for (const auto& [fn, bitstring] : list) {
            if (fn == "UnallocatedEncoding") {
                continue;
            }
            if (std::find(do_not_test.begin(), do_not_test.end(), fn) != do_not_test.end()) {
                invalid.emplace_back(InstructionGenerator{bitstring});
                continue;
            }
            generators.emplace_back(InstructionGenerator{bitstring});
        }
        return InstructionGeneratorInfo{generators, invalid};
    }();

    while (true) {
        const size_t index = RandInt<size_t>(0, instructions.generators.size() - 1);
        const u32 inst = instructions.generators[index].Generate();

        if (std::any_of(instructions.invalid.begin(), instructions.invalid.end(), [inst](const auto& invalid) { return invalid.Match(inst); })) {
            continue;
        }
        if (!SkipInFuzzing(inst) && ShouldTestInst(inst, pc, is_last_inst)) {
            return inst;
        }
    }
}

static u32 GenFloatInst(u64 pc, bool is_last_inst) {
    static const std::vector<InstructionGenerator> instruction_generators = [] {
        const std::vector<std::tuple<std::string, std::string, const char*>> list{
#define INST(fn, name, bitstring) {#fn, #name, bitstring},
#include "dynarmic/frontend/A64/decoder/a64.inc"
#undef INST
        };

        // List of instructions not to test
        const std::vector<std::string> do_not_test{};

        std::vector<InstructionGenerator> result;

        for (const auto& [fn, name, bitstring] : list) {
            (void)name;

            if (fn[0] != 'F') {
                continue;
            } else if (std::find(do_not_test.begin(), do_not_test.end(), fn) != do_not_test.end()) {
                continue;
            }
            result.emplace_back(InstructionGenerator{bitstring});
        }

        return result;
    }();

    while (true) {
        const size_t index = RandInt<size_t>(0, instruction_generators.size() - 1);
        const u32 instruction = instruction_generators[index].Generate();

        if (!SkipInFuzzing(instruction) && ShouldTestInst(instruction, pc, is_last_inst)) {
            return instruction;
        }
    }
}

static Dynarmic::A64::UserConfig GetUserConfig(A64TestEnv& jit_env) {
    Dynarmic::A64::UserConfig jit_user_config{};
    jit_user_config.callbacks = &jit_env;
    jit_user_config.optimizations &= ~OptimizationFlag::FastDispatch;
    // The below corresponds to the settings for qemu's aarch64_max_initfn
    jit_user_config.dczid_el0 = 7;
    jit_user_config.ctr_el0 = 0x80038003;
    return jit_user_config;
}

static void RunTestInstance(Dynarmic::A64::Jit& jit, A64Unicorn& uni, A64TestEnv& jit_env, A64TestEnv& uni_env, Dynarmic::A64::UserConfig& conf, const A64Unicorn::RegisterArray& regs, const A64Unicorn::VectorArray& vecs, const size_t instructions_start, const std::vector<u32>& instructions, const u32 pstate, const u32 fpcr) {
    jit_env.code_mem = instructions;
    uni_env.code_mem = instructions;
    jit_env.code_mem.emplace_back(0x14000000);  // B .
    uni_env.code_mem.emplace_back(0x14000000);  // B .
    jit_env.code_mem_start_address = instructions_start;
    uni_env.code_mem_start_address = instructions_start;
    jit_env.modified_memory.clear();
    uni_env.modified_memory.clear();

    const u64 initial_sp = RandInt<u64>(0x30'0000'0000, 0x40'0000'0000) * 4;

    jit.SetRegisters(regs);
    jit.SetVectors(vecs);
    jit.SetPC(instructions_start);
    jit.SetSP(initial_sp);
    jit.SetFpcr(fpcr);
    jit.SetFpsr(0);
    jit.SetPstate(pstate);
    jit.ClearCache();
    uni.SetRegisters(regs);
    uni.SetVectors(vecs);
    uni.SetPC(instructions_start);
    uni.SetSP(initial_sp);
    uni.SetFpcr(fpcr);
    uni.SetFpsr(0);
    uni.SetPstate(pstate);
    uni.ClearPageCache();

    jit_env.ticks_left = instructions.size();
    CheckedRun([&]() { jit.Run(); });

    uni_env.ticks_left = instructions.size() * 4;
    uni.Run();

    // SCOPE_FAIL {
    //     fmt::print("Instruction Listing:\n");
    //     for (u32 instruction : instructions) {
    //         fmt::print("{:08x} {}\n", instruction, Common::DisassembleAArch64(instruction));
    //     }
    //     fmt::print("\n");

    //     fmt::print("Initial register listing:\n");
    //     for (size_t i = 0; i < regs.size(); ++i) {
    //         fmt::print("{:3s}: {:016x}\n", A64::RegToString(static_cast<A64::Reg>(i)), regs[i]);
    //     }
    //     for (size_t i = 0; i < vecs.size(); ++i) {
    //         fmt::print("{:3s}: {:016x}{:016x}\n", A64::VecToString(static_cast<A64::Vec>(i)), vecs[i][1], vecs[i][0]);
    //     }
    //     fmt::print("sp : {:016x}\n", initial_sp);
    //     fmt::print("pc : {:016x}\n", instructions_start);
    //     fmt::print("p  : {:08x}\n", pstate);
    //     fmt::print("fpcr {:08x}\n", fpcr);
    //     fmt::print("fpcr.AHP   {}\n", FP::FPCR{fpcr}.AHP());
    //     fmt::print("fpcr.DN    {}\n", FP::FPCR{fpcr}.DN());
    //     fmt::print("fpcr.FZ    {}\n", FP::FPCR{fpcr}.FZ());
    //     fmt::print("fpcr.RMode {}\n", static_cast<size_t>(FP::FPCR{fpcr}.RMode()));
    //     fmt::print("fpcr.FZ16  {}\n", FP::FPCR{fpcr}.FZ16());
    //     fmt::print("\n");

    //     fmt::print("Final register listing:\n");
    //     fmt::print("     unicorn          dynarmic\n");
    //     const auto uni_regs = uni.GetRegisters();
    //     for (size_t i = 0; i < regs.size(); ++i) {
    //         fmt::print("{:3s}: {:016x} {:016x} {}\n", A64::RegToString(static_cast<A64::Reg>(i)), uni_regs[i], jit.GetRegisters()[i], uni_regs[i] != jit.GetRegisters()[i] ? "*" : "");
    //     }
    //     const auto uni_vecs = uni.GetVectors();
    //     for (size_t i = 0; i < vecs.size(); ++i) {
    //         fmt::print("{:3s}: {:016x}{:016x} {:016x}{:016x} {}\n", A64::VecToString(static_cast<A64::Vec>(i)),
    //                    uni_vecs[i][1], uni_vecs[i][0],
    //                    jit.GetVectors()[i][1], jit.GetVectors()[i][0],
    //                    uni_vecs[i] != jit.GetVectors()[i] ? "*" : "");
    //     }
    //     fmt::print("sp : {:016x} {:016x} {}\n", uni.GetSP(), jit.GetSP(), uni.GetSP() != jit.GetSP() ? "*" : "");
    //     fmt::print("pc : {:016x} {:016x} {}\n", uni.GetPC(), jit.GetPC(), uni.GetPC() != jit.GetPC() ? "*" : "");
    //     fmt::print("p  : {:08x} {:08x} {}\n", uni.GetPstate(), jit.GetPstate(), (uni.GetPstate() & 0xF0000000) != (jit.GetPstate() & 0xF0000000) ? "*" : "");
    //     fmt::print("qc : {:08x} {:08x} {}\n", uni.GetFpsr(), jit.GetFpsr(), FP::FPSR{uni.GetFpsr()}.QC() != FP::FPSR{jit.GetFpsr()}.QC() ? "*" : "");
    //     fmt::print("\n");

    //     fmt::print("Modified memory:\n");
    //     fmt::print("                 uni dyn\n");
    //     auto uni_iter = uni_env.modified_memory.begin();
    //     auto jit_iter = jit_env.modified_memory.begin();
    //     while (uni_iter != uni_env.modified_memory.end() || jit_iter != jit_env.modified_memory.end()) {
    //         if (uni_iter == uni_env.modified_memory.end() || (jit_iter != jit_env.modified_memory.end() && uni_iter->first > jit_iter->first)) {
    //             fmt::print("{:016x}:    {:02x} *\n", jit_iter->first, jit_iter->second);
    //             jit_iter++;
    //         } else if (jit_iter == jit_env.modified_memory.end() || jit_iter->first > uni_iter->first) {
    //             fmt::print("{:016x}: {:02x}    *\n", uni_iter->first, uni_iter->second);
    //             uni_iter++;
    //         } else if (uni_iter->first == jit_iter->first) {
    //             fmt::print("{:016x}: {:02x} {:02x} {}\n", uni_iter->first, uni_iter->second, jit_iter->second, uni_iter->second != jit_iter->second ? "*" : "");
    //             uni_iter++;
    //             jit_iter++;
    //         }
    //     }
    //     fmt::print("\n");

    //     const auto get_code = [&jit_env](u64 vaddr) { return jit_env.MemoryReadCode(vaddr); };
    //     const A64::LocationDescriptor location{instructions_start, FP::FPCR{fpcr}};
    //     IR::Block ir_block{location};
    //     A64::Translate(ir_block, location, get_code, {});
    //     fmt::print("IR:\n{}\n", IR::DumpBlock(ir_block));
    //     Optimization::Optimize(ir_block, conf, {});
    //     fmt::print("Optimized IR:\n{}\n", IR::DumpBlock(ir_block));
    //     fmt::print("x86_64:\n{}", jit.Disassemble());
    //     fmt::print("Interrupts:\n");
    //     for (auto& i : uni_env.interrupts) {
    //         puts(i.c_str());
    //     }
    // };

    REQUIRE(uni_env.code_mem_modified_by_guest == jit_env.code_mem_modified_by_guest);
    if (uni_env.code_mem_modified_by_guest) {
        return;
    }

    REQUIRE(uni.GetPC() + 4 == jit.GetPC());
    REQUIRE(uni.GetRegisters() == jit.GetRegisters());
    REQUIRE(uni.GetVectors() == jit.GetVectors());
    REQUIRE(uni.GetSP() == jit.GetSP());
    REQUIRE((uni.GetPstate() & 0xF0000000) == (jit.GetPstate() & 0xF0000000));
    REQUIRE(uni_env.modified_memory == jit_env.modified_memory);
    REQUIRE(uni_env.interrupts.empty());
    REQUIRE(FP::FPSR{uni.GetFpsr()}.QC() == FP::FPSR{jit.GetFpsr()}.QC());
}

TEST_CASE("A64: Single random instruction", "[a64][unicorn]") {
    A64TestEnv jit_env{};
    A64TestEnv uni_env{};

    Dynarmic::A64::UserConfig conf{GetUserConfig(jit_env)};
    Dynarmic::A64::Jit jit{conf};
    A64Unicorn uni{uni_env};

    A64Unicorn::RegisterArray regs;
    A64Unicorn::VectorArray vecs;
    std::vector<u32> instructions(1);

    for (size_t iteration = 0; iteration < 100000; ++iteration) {
        std::generate(regs.begin(), regs.end(), [] { return RandInt<u64>(0, ~u64(0)); });
        std::generate(vecs.begin(), vecs.end(), RandomVector);

        instructions[0] = GenRandomInst(0, true);

        const u64 start_address = RandInt<u64>(0, 0x10'0000'0000) * 4;
        const u32 pstate = RandInt<u32>(0, 0xF) << 28;
        const u32 fpcr = RandomFpcr();

        INFO("Instruction: 0x" << std::hex << instructions[0]);

        RunTestInstance(jit, uni, jit_env, uni_env, conf, regs, vecs, start_address, instructions, pstate, fpcr);
    }
}

TEST_CASE("A64: Floating point instructions", "[a64][unicorn]") {
    A64TestEnv jit_env{};
    A64TestEnv uni_env{};

    Dynarmic::A64::UserConfig conf{GetUserConfig(jit_env)};
    Dynarmic::A64::Jit jit{conf};
    A64Unicorn uni{uni_env};

    static constexpr std::array<u64, 80> float_numbers{
        0x00000000,  // positive zero
        0x00000001,  // smallest positive denormal
        0x00000076,  //
        0x00002b94,  //
        0x00636d24,  //
        0x007fffff,  // largest positive denormal
        0x00800000,  // smallest positive normalised real
        0x00800002,  //
        0x01398437,  //
        0x0ba98d27,  //
        0x0ba98d7a,  //
        0x751f853a,  //
        0x7f7ffff0,  //
        0x7f7fffff,  // largest positive normalised real
        0x7f800000,  // positive infinity
        0x7f800001,  // first positive SNaN
        0x7f984a37,  //
        0x7fbfffff,  // last positive SNaN
        0x7fc00000,  // first positive QNaN
        0x7fd9ba98,  //
        0x7fffffff,  // last positive QNaN
        0x80000000,  // negative zero
        0x80000001,  // smallest negative denormal
        0x80000076,  //
        0x80002b94,  //
        0x80636d24,  //
        0x807fffff,  // largest negative denormal
        0x80800000,  // smallest negative normalised real
        0x80800002,  //
        0x81398437,  //
        0x8ba98d27,  //
        0x8ba98d7a,  //
        0xf51f853a,  //
        0xff7ffff0,  //
        0xff7fffff,  // largest negative normalised real
        0xff800000,  // negative infinity
        0xff800001,  // first negative SNaN
        0xff984a37,  //
        0xffbfffff,  // last negative SNaN
        0xffc00000,  // first negative QNaN
        0xffd9ba98,  //
        0xffffffff,  // last negative QNaN
        // some random numbers follow
        0x4f3495cb,
        0xe73a5134,
        0x7c994e9e,
        0x6164bd6c,
        0x09503366,
        0xbf5a97c9,
        0xe6ff1a14,
        0x77f31e2f,
        0xaab4d7d8,
        0x0966320b,
        0xb26bddee,
        0xb5c8e5d3,
        0x317285d3,
        0x3c9623b1,
        0x51fd2c7c,
        0x7b906a6c,
        0x3f800000,
        0x3dcccccd,
        0x3f000000,
        0x42280000,
        0x3eaaaaab,
        0xc1200000,
        0xbf800000,
        0xbf8147ae,
        0x3f8147ae,
        0x415df525,
        0xc79b271e,
        0x460e8c84,
        // some 64-bit-float upper-halves
        0x7ff00000,  // +SNaN / +Inf
        0x7ff0abcd,  // +SNaN
        0x7ff80000,  // +QNaN
        0x7ff81234,  // +QNaN
        0xfff00000,  // -SNaN / -Inf
        0xfff05678,  // -SNaN
        0xfff80000,  // -QNaN
        0xfff809ef,  // -QNaN
        0x3ff00000,  // Number near +1.0
        0xbff00000,  // Number near -1.0
    };

    const auto gen_float = [&] {
        if (RandInt<size_t>(0, 1) == 0) {
            return RandInt<u64>(0, 0xffffffff);
        }
        return float_numbers[RandInt<size_t>(0, float_numbers.size() - 1)];
    };

    const auto gen_vector = [&] {
        u64 upper = (gen_float() << 32) | gen_float();
        u64 lower = (gen_float() << 32) | gen_float();
        return Vector{lower, upper};
    };

    A64Unicorn::RegisterArray regs;
    A64Unicorn::VectorArray vecs;
    std::vector<u32> instructions(1);

    for (size_t iteration = 0; iteration < 100000; ++iteration) {
        std::generate(regs.begin(), regs.end(), gen_float);
        std::generate(vecs.begin(), vecs.end(), gen_vector);

        instructions[0] = GenFloatInst(0, true);

        const u64 start_address = RandInt<u64>(0, 0x10'0000'0000) * 4;
        const u32 pstate = RandInt<u32>(0, 0xF) << 28;
        const u32 fpcr = RandomFpcr();

        INFO("Instruction: 0x" << std::hex << instructions[0]);

        RunTestInstance(jit, uni, jit_env, uni_env, conf, regs, vecs, start_address, instructions, pstate, fpcr);
    }
}

TEST_CASE("A64: Small random block", "[a64][unicorn]") {
    A64TestEnv jit_env{};
    A64TestEnv uni_env{};

    Dynarmic::A64::UserConfig conf{GetUserConfig(jit_env)};
    Dynarmic::A64::Jit jit{conf};
    A64Unicorn uni{uni_env};

    A64Unicorn::RegisterArray regs;
    A64Unicorn::VectorArray vecs;
    std::vector<u32> instructions(5);

    for (size_t iteration = 0; iteration < 100000; ++iteration) {
        std::generate(regs.begin(), regs.end(), [] { return RandInt<u64>(0, ~u64(0)); });
        std::generate(vecs.begin(), vecs.end(), RandomVector);

        instructions[0] = GenRandomInst(0, false);
        instructions[1] = GenRandomInst(4, false);
        instructions[2] = GenRandomInst(8, false);
        instructions[3] = GenRandomInst(12, false);
        instructions[4] = GenRandomInst(16, true);

        const u64 start_address = RandInt<u64>(0, 0x10'0000'0000) * 4;
        const u32 pstate = RandInt<u32>(0, 0xF) << 28;
        const u32 fpcr = RandomFpcr();

        INFO("Instruction 1: 0x" << std::hex << instructions[0]);
        INFO("Instruction 2: 0x" << std::hex << instructions[1]);
        INFO("Instruction 3: 0x" << std::hex << instructions[2]);
        INFO("Instruction 4: 0x" << std::hex << instructions[3]);
        INFO("Instruction 5: 0x" << std::hex << instructions[4]);

        RunTestInstance(jit, uni, jit_env, uni_env, conf, regs, vecs, start_address, instructions, pstate, fpcr);
    }
}

TEST_CASE("A64: Large random block", "[a64][unicorn]") {
    A64TestEnv jit_env{};
    A64TestEnv uni_env{};

    Dynarmic::A64::UserConfig conf{GetUserConfig(jit_env)};
    Dynarmic::A64::Jit jit{conf};
    A64Unicorn uni{uni_env};

    A64Unicorn::RegisterArray regs;
    A64Unicorn::VectorArray vecs;

    constexpr size_t instruction_count = 100;
    std::vector<u32> instructions(instruction_count);

    for (size_t iteration = 0; iteration < 500; ++iteration) {
        std::generate(regs.begin(), regs.end(), [] { return RandInt<u64>(0, ~u64(0)); });
        std::generate(vecs.begin(), vecs.end(), RandomVector);

        for (size_t j = 0; j < instruction_count; ++j) {
            instructions[j] = GenRandomInst(j * 4, j == instruction_count - 1);
        }

        const u64 start_address = RandInt<u64>(0, 0x10'0000'0000) * 4;
        const u32 pstate = RandInt<u32>(0, 0xF) << 28;
        const u32 fpcr = RandomFpcr();

        RunTestInstance(jit, uni, jit_env, uni_env, conf, regs, vecs, start_address, instructions, pstate, fpcr);
    }
}

// Differential tests for OptimizationFlag::HostRegisterLocality: the same random blocks run in a
// JIT with the optimization and in one without it must leave exactly the same state (registers,
// vectors, flags, FPSR, memory). Needs no reference emulator, so it checks the optimization alone.
struct DifferentialOptions {
    bool fast_dispatch = false;
    bool wall_clock = false;  // no cycle counting (as in the emulator): blocks must end in an SVC
};

static void RunHostRegisterLocalityDifferentialWith(const std::function<void(std::vector<u32>&)>& gen_block, bool floats, size_t iterations, DifferentialOptions options = {});

static void RunHostRegisterLocalityDifferential(size_t block_size, bool floats, size_t iterations) {
    RunHostRegisterLocalityDifferentialWith([&](std::vector<u32>& instructions) {
        instructions.resize(block_size);
        for (size_t i = 0; i < block_size; ++i) {
            const bool last = i + 1 == block_size;
            instructions[i] = floats && RandInt<size_t>(0, 3) != 0 ? GenFloatInst(i * 4, last) : GenRandomInst(i * 4, last);
        }
    }, floats, iterations);
}

static void RunHostRegisterLocalityDifferentialWith(const std::function<void(std::vector<u32>&)>& gen_block, bool floats, size_t iterations, DifferentialOptions options) {
    A64TestEnv env_a{};
    A64TestEnv env_b{};
    Dynarmic::A64::UserConfig conf_a{GetUserConfig(env_a)};
    Dynarmic::A64::UserConfig conf_b{GetUserConfig(env_b)};
    conf_a.optimizations |= OptimizationFlag::HostRegisterLocality;
    conf_b.optimizations &= ~OptimizationFlag::HostRegisterLocality;
    if (std::getenv("DIFF_CONTROL"))  // both without: shows what differs without the optimization
        conf_a.optimizations &= ~OptimizationFlag::HostRegisterLocality;
    for (auto* conf : {&conf_a, &conf_b}) {
        if (options.fast_dispatch)
            conf->optimizations |= OptimizationFlag::FastDispatch;
        if (options.wall_clock) {
            conf->enable_cycle_counting = false;
            conf->wall_clock_cntpct = true;
        }
    }
    Dynarmic::A64::Jit jit_a{conf_a};
    Dynarmic::A64::Jit jit_b{conf_b};
    env_a.record_exceptions = env_b.record_exceptions = true;
    if (options.wall_clock) {
        env_a.halt_on_svc = &jit_a;
        env_b.halt_on_svc = &jit_b;
    }

    static constexpr std::array<u64, 16> special_floats{
        0x00000000, 0x80000000, 0x00000001, 0x007fffff, 0x00800000, 0x7f7fffff, 0x7f800000, 0xff800000,
        0x7f800001, 0x7fbfffff, 0x7fc00000, 0xffffffff, 0x3f800000, 0xbf800000, 0x3f000000, 0x4b000001,
    };
    const auto gen_u64 = [floats] {
        if (!floats || RandInt<size_t>(0, 2) == 0)
            return RandInt<u64>(0, ~u64(0));
        const auto half = [] { return RandInt<size_t>(0, 1) ? RandInt<u64>(0, 0xffffffff) : special_floats[RandInt<size_t>(0, special_floats.size() - 1)]; };
        return (half() << 32) | half();
    };

    std::array<u64, 31> regs;
    std::array<Vector, 32> vecs;
    std::vector<u32> instructions;
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        std::generate(regs.begin(), regs.end(), gen_u64);
        std::generate(vecs.begin(), vecs.end(), [&] { return Vector{gen_u64(), gen_u64()}; });
        gen_block(instructions);
        const u64 start_address = RandInt<u64>(0, 0x10'0000'0000) * 4;
        const u64 initial_sp = RandInt<u64>(0x30'0000'0000, 0x40'0000'0000) * 4;
        const u32 pstate = RandInt<u32>(0, 0xF) << 28;
        const u32 fpcr = RandomFpcr();

        std::string listing;
        for (u32 instruction : instructions)
            listing += fmt::format("{:08x} ", instruction);
        INFO("Instructions: " << listing << " fpcr " << std::hex << fpcr);
        if (std::getenv("DIFF_TRACE"))
            fmt::print(stderr, "{}\n", listing);

        for (auto [jit, env] : {std::pair{&jit_a, &env_a}, std::pair{&jit_b, &env_b}}) {
            env->code_mem = instructions;
            env->code_mem.emplace_back(0x14000000);  // B .
            env->code_mem_start_address = start_address;
            env->modified_memory.clear();
            env->interrupts.clear();
            env->code_mem_modified_by_guest = false;
            jit->SetRegisters(regs);
            jit->SetVectors(vecs);
            jit->SetPC(start_address);
            jit->SetSP(initial_sp);
            jit->SetFpcr(fpcr);
            jit->SetFpsr(0);
            jit->SetPstate(pstate);
            jit->ClearCache();
            env->ticks_left = instructions.size();
            CheckedRun([&]() { jit->Run(); });
        }

        if (std::getenv("DIFF_EXPLAIN") && jit_a.GetFpsr() != jit_b.GetFpsr()) {
            // Print both optimized IRs of the block that differs.
            for (const bool locality : {true, false}) {
                IR::Block ir_block{A64::LocationDescriptor{start_address, FP::FPCR{fpcr}}};
                A64::Translate(ir_block, A64::LocationDescriptor{start_address, FP::FPCR{fpcr}}, [&](u64 vaddr) { return env_a.MemoryReadCode(vaddr); }, {});
                Optimization::Optimize(ir_block, locality ? conf_a : conf_b, {});
                fmt::print("== {} (fpsr {:08x}):\n{}\n", locality ? "with" : "without", locality ? jit_a.GetFpsr() : jit_b.GetFpsr(), IR::DumpBlock(ir_block));
            }
        }
        REQUIRE(env_a.code_mem_modified_by_guest == env_b.code_mem_modified_by_guest);
        REQUIRE(jit_a.GetPC() == jit_b.GetPC());
        REQUIRE(jit_a.GetRegisters() == jit_b.GetRegisters());
        REQUIRE(jit_a.GetVectors() == jit_b.GetVectors());
        REQUIRE(jit_a.GetSP() == jit_b.GetSP());
        REQUIRE(jit_a.GetPstate() == jit_b.GetPstate());
        REQUIRE(jit_a.GetFpsr() == jit_b.GetFpsr());
        REQUIRE(env_a.modified_memory == env_b.modified_memory);
        REQUIRE(env_a.interrupts == env_b.interrupts);
    }
}

TEST_CASE("A64: HostRegisterLocality differential: single instructions", "[a64][differential]") {
    RunHostRegisterLocalityDifferential(1, false, 100000);
}

TEST_CASE("A64: HostRegisterLocality differential: single FP instructions", "[a64][differential]") {
    RunHostRegisterLocalityDifferential(1, true, 100000);
}

TEST_CASE("A64: HostRegisterLocality differential: small blocks", "[a64][differential]") {
    RunHostRegisterLocalityDifferential(6, false, 30000);
}

TEST_CASE("A64: HostRegisterLocality differential: small FP blocks", "[a64][differential]") {
    RunHostRegisterLocalityDifferential(6, true, 30000);
}

TEST_CASE("A64: HostRegisterLocality differential: large blocks", "[a64][differential]") {
    RunHostRegisterLocalityDifferential(60, false, 3000);
}

TEST_CASE("A64: HostRegisterLocality differential: large FP blocks", "[a64][differential]") {
    RunHostRegisterLocalityDifferential(60, true, 3000);
}

// Blocks ending in a flag-setting add/subtract (optionally followed by plain register writes)
// and a conditional branch: the case where the branch reuses the host flags (all 16 conditions).
TEST_CASE("A64: HostRegisterLocality differential: add/sub flags then B.cond", "[a64][differential]") {
    static const std::vector<InstructionGenerator> flag_ops{
        InstructionGenerator{"z0111010000mmmmm000000nnnnnddddd"},  // ADCS
        InstructionGenerator{"z1111010000mmmmm000000nnnnnddddd"},  // SBCS
        InstructionGenerator{"z0101011001mmmmmxxxiiinnnnnddddd"},  // ADDS (extended register)
        InstructionGenerator{"z1101011001mmmmmxxxiiinnnnnddddd"},  // SUBS (extended register)
        InstructionGenerator{"z0101011ss0mmmmmiiiiiinnnnnddddd"},  // ADDS (shifted register)
        InstructionGenerator{"z1101011ss0mmmmmiiiiiinnnnnddddd"},  // SUBS (shifted register)
        InstructionGenerator{"z0110001ssiiiiiiiiiiiinnnnnddddd"},  // ADDS (immediate)
        InstructionGenerator{"z1110001ssiiiiiiiiiiiinnnnnddddd"},  // SUBS (immediate)
    };
    static const InstructionGenerator movz{"z10100101ssiiiiiiiiiiiiiiiiddddd"};
    static const InstructionGenerator orr{"z0101010ss0mmmmmiiiiiinnnnnddddd"};
    static const InstructionGenerator b_cond{"01010100iiiiiiiiiiiiiiiiiii0cccc"};
    RunHostRegisterLocalityDifferentialWith([](std::vector<u32>& instructions) {
        instructions.clear();
        for (size_t i = RandInt<size_t>(0, 2); i > 0; --i)
            instructions.push_back(GenRandomInst(instructions.size() * 4, false));
        instructions.push_back(flag_ops[RandInt<size_t>(0, flag_ops.size() - 1)].Generate());
        if (RandInt<size_t>(0, 2) == 0)  // a plain register write in between (no flag change)
            instructions.push_back(movz.Generate());
        if (RandInt<size_t>(0, 5) == 0)  // or one that changes the host flags
            instructions.push_back(orr.Generate());
        instructions.push_back(b_cond.Generate());
    }, false, 200000);
}

// The same with the emulator's settings: no cycle counting (the host-flags branch only applies
// then) and fast dispatch on (the inline RSB check's miss path). Every way out of the block
// reaches an SVC, which halts the JIT, so the runs stop at the same point.
TEST_CASE("A64: HostRegisterLocality differential: add/sub flags then B.cond (wall clock)", "[a64][differential]") {
    static const std::vector<InstructionGenerator> flag_ops{
        InstructionGenerator{"z0111010000mmmmm000000nnnnnddddd"},  // ADCS
        InstructionGenerator{"z1111010000mmmmm000000nnnnnddddd"},  // SBCS
        InstructionGenerator{"z0101011001mmmmmxxxiiinnnnnddddd"},  // ADDS (extended register)
        InstructionGenerator{"z1101011001mmmmmxxxiiinnnnnddddd"},  // SUBS (extended register)
        InstructionGenerator{"z0101011ss0mmmmmiiiiiinnnnnddddd"},  // ADDS (shifted register)
        InstructionGenerator{"z1101011ss0mmmmmiiiiiinnnnnddddd"},  // SUBS (shifted register)
        InstructionGenerator{"z0110001ssiiiiiiiiiiiinnnnnddddd"},  // ADDS (immediate)
        InstructionGenerator{"z1110001ssiiiiiiiiiiiinnnnnddddd"},  // SUBS (immediate)
    };
    static const InstructionGenerator movz{"z10100101ssiiiiiiiiiiiiiiiiddddd"};
    static const InstructionGenerator orr{"z0101010ss0mmmmmiiiiiinnnnnddddd"};
    RunHostRegisterLocalityDifferentialWith([](std::vector<u32>& instructions) {
        instructions.clear();
        for (size_t i = RandInt<size_t>(0, 2); i > 0; --i)
            instructions.push_back(GenRandomInst(instructions.size() * 4, false));
        instructions.push_back(flag_ops[RandInt<size_t>(0, flag_ops.size() - 1)].Generate());
        if (RandInt<size_t>(0, 2) == 0)
            instructions.push_back(movz.Generate());
        if (RandInt<size_t>(0, 5) == 0)
            instructions.push_back(orr.Generate());
        instructions.push_back(0x54000040 | RandInt<u32>(0, 15));  // B.<random cond> +8
        instructions.push_back(0xD4000001);                         // SVC #0 (not taken)
        instructions.push_back(0xD4000021);                         // SVC #1 (taken)
    }, false, 200000, {.fast_dispatch = true, .wall_clock = true});
}

// Calls and returns with the emulator's settings (no cycle counting, fast dispatch): BL into a
// function of random instructions that ends with ADR X30 + RET (the inline RSB check). Each case
// runs three times without clearing the cache, so the RSB holds real code pointers: returning to
// the instruction after the BL (hit), then to the one after that (miss: fast dispatch), then
// back (hit). Only the ADR changes between runs (its range is invalidated). Both targets are SVCs.
static u32 AdrTo(size_t at, size_t target, u32 reg) {
    const u32 imm = u32((s32(target) - s32(at)) * 4) & 0x1FFFFF;
    return 0x10000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}

// layout(instructions, target) builds the code for a run, returning the index of the instruction
// that changes between runs.
static void RunThreeRunDifferential(const std::function<size_t(std::vector<u32>&)>& layout, const std::function<u32(size_t at, size_t target)>& variant, size_t iterations) {
    A64TestEnv env_a{};
    A64TestEnv env_b{};
    Dynarmic::A64::UserConfig conf_a{GetUserConfig(env_a)};
    Dynarmic::A64::UserConfig conf_b{GetUserConfig(env_b)};
    conf_a.optimizations |= OptimizationFlag::HostRegisterLocality;
    conf_b.optimizations &= ~OptimizationFlag::HostRegisterLocality;
    if (std::getenv("DIFF_CONTROL"))
        conf_a.optimizations &= ~OptimizationFlag::HostRegisterLocality;
    for (auto* conf : {&conf_a, &conf_b}) {
        conf->optimizations |= OptimizationFlag::FastDispatch;
        conf->enable_cycle_counting = false;
        conf->wall_clock_cntpct = true;
    }
    Dynarmic::A64::Jit jit_a{conf_a};
    Dynarmic::A64::Jit jit_b{conf_b};
    env_a.record_exceptions = env_b.record_exceptions = true;
    env_a.halt_on_svc = &jit_a;
    env_b.halt_on_svc = &jit_b;

    std::array<u64, 31> regs;
    std::array<Vector, 32> vecs;
    std::vector<u32> instructions;
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        std::generate(regs.begin(), regs.end(), [] { return RandInt<u64>(0, ~u64(0)); });
        std::generate(vecs.begin(), vecs.end(), RandomVector);
        instructions.clear();
        const size_t adr_at = layout(instructions);
        const u64 start_address = RandInt<u64>(0, 0x10'0000'0000) * 4;
        const u64 initial_sp = RandInt<u64>(0x30'0000'0000, 0x40'0000'0000) * 4;
        const u32 pstate = RandInt<u32>(0, 0xF) << 28;
        const u32 fpcr = RandomFpcr();

        std::string listing;
        for (u32 instruction : instructions)
            listing += fmt::format("{:08x} ", instruction);
        INFO("Instructions: " << listing << " fpcr " << std::hex << fpcr);

        for (size_t run = 0; run < 3; ++run) {
            const size_t target = run == 1 ? 2 : 1;
            for (auto [jit, env] : {std::pair{&jit_a, &env_a}, std::pair{&jit_b, &env_b}}) {
                env->code_mem = instructions;
                env->code_mem[adr_at] = variant(adr_at, target);
                env->code_mem.emplace_back(0x14000000);  // B .
                env->code_mem_start_address = start_address;
                env->modified_memory.clear();
                env->interrupts.clear();
                env->code_mem_modified_by_guest = false;
                if (run == 0)
                    jit->ClearCache();
                else
                    jit->InvalidateCacheRange(start_address + adr_at * 4, 4);
                jit->SetRegisters(regs);
                jit->SetVectors(vecs);
                jit->SetPC(start_address);
                jit->SetSP(initial_sp);
                jit->SetFpcr(fpcr);
                jit->SetFpsr(0);
                jit->SetPstate(pstate);
                CheckedRun([&]() { jit->Run(); });
            }
            INFO("run " << run);
            REQUIRE(jit_a.GetPC() == jit_b.GetPC());
            REQUIRE(jit_a.GetRegisters() == jit_b.GetRegisters());
            REQUIRE(jit_a.GetVectors() == jit_b.GetVectors());
            REQUIRE(jit_a.GetSP() == jit_b.GetSP());
            REQUIRE(jit_a.GetPstate() == jit_b.GetPstate());
            REQUIRE(jit_a.GetFpsr() == jit_b.GetFpsr());
            REQUIRE(env_a.modified_memory == env_b.modified_memory);
            REQUIRE(env_a.interrupts == env_b.interrupts);
        }
    }
}

// Calls and returns with the emulator's settings: BL into random instructions, then ADR X30 + RET
// (the inline RSB check) to the instruction after the BL (hit) or the one after that (miss).
TEST_CASE("A64: HostRegisterLocality differential: BL / RET (wall clock)", "[a64][differential]") {
    RunThreeRunDifferential([](std::vector<u32>& instructions) {
        instructions.push_back(0x94000003);  // BL +12
        instructions.push_back(0xD4000001);  // SVC #0 (the return address)
        instructions.push_back(0xD4000041);  // SVC #2
        for (size_t i = RandInt<size_t>(0, 5); i > 0; --i)
            instructions.push_back(GenRandomInst(instructions.size() * 4, false));
        const size_t adr_at = instructions.size();
        instructions.push_back(AdrTo(adr_at, 1, 30));
        instructions.push_back(0xD65F03C0);  // RET
        return adr_at;
    }, [](size_t at, size_t target) { return AdrTo(at, target, 30); }, 30000);
}

// Indirect branches (BR/BLR, the inline fast dispatch lookup) to one of two SVCs, the target
// changing between runs: table misses and hits with real code pointers.
TEST_CASE("A64: HostRegisterLocality differential: BR / BLR (wall clock)", "[a64][differential]") {
    RunThreeRunDifferential([](std::vector<u32>& instructions) {
        instructions.push_back(0x14000003);  // B +12
        instructions.push_back(0xD4000001);  // SVC #0
        instructions.push_back(0xD4000041);  // SVC #2
        for (size_t i = RandInt<size_t>(0, 5); i > 0; --i)
            instructions.push_back(GenRandomInst(instructions.size() * 4, false));
        const size_t adr_at = instructions.size();
        instructions.push_back(AdrTo(adr_at, 1, 16));
        instructions.push_back(RandInt<size_t>(0, 1) ? 0xD61F0200 : 0xD63F0200);  // BR X16 / BLR X16
        return adr_at;
    }, [](size_t at, size_t target) { return AdrTo(at, target, 16); }, 30000);
}

// Random straight-line blocks (integer and FP) ending in an SVC, with the emulator's settings.
static void RunWallClockBlocks(size_t count, bool floats, size_t iterations) {
    RunHostRegisterLocalityDifferentialWith([count, floats](std::vector<u32>& instructions) {
        instructions.clear();
        for (size_t i = 0; i < count; ++i)
            instructions.push_back(floats && RandInt<size_t>(0, 3) != 0 ? GenFloatInst(i * 4, false) : GenRandomInst(i * 4, false));
        instructions.push_back(0xD4000001);  // SVC #0
    }, floats, iterations, {.fast_dispatch = true, .wall_clock = true});
}

TEST_CASE("A64: HostRegisterLocality differential: blocks (wall clock)", "[a64][differential]") {
    RunWallClockBlocks(8, false, 30000);
}

TEST_CASE("A64: HostRegisterLocality differential: FP blocks (wall clock)", "[a64][differential]") {
    RunWallClockBlocks(8, true, 30000);
}

// Compare-and-branch / test-bit-and-branch ending a block (the CheckBit terminal, which branches on
// the host flags when the check bit was the block's last store), with the emulator's settings.
TEST_CASE("A64: HostRegisterLocality differential: CBZ / TBZ (wall clock)", "[a64][differential]") {
    RunHostRegisterLocalityDifferentialWith([](std::vector<u32>& instructions) {
        instructions.clear();
        for (size_t i = RandInt<size_t>(0, 3); i > 0; --i)
            instructions.push_back(GenRandomInst(instructions.size() * 4, false));
        const u32 rt = RandInt<u32>(0, 31);
        switch (RandInt<u32>(0, 3)) {
        case 0:  // CBZ / CBNZ Wt|Xt, +8
        case 1:
            instructions.push_back((RandInt<u32>(0, 1) << 31) | 0x34000000 | (RandInt<u32>(0, 1) << 24) | (2 << 5) | rt);
            break;
        default: {  // TBZ / TBNZ Rt, #bit, +8
            const u32 bit = RandInt<u32>(0, 63);
            instructions.push_back(((bit >> 5) << 31) | 0x36000000 | (RandInt<u32>(0, 1) << 24) | ((bit & 31) << 19) | (2 << 5) | rt);
            break;
        }
        }
        instructions.push_back(0xD4000001);  // SVC #0 (not taken)
        instructions.push_back(0xD4000021);  // SVC #1 (taken)
    }, false, 200000, {.fast_dispatch = true, .wall_clock = true});
}
