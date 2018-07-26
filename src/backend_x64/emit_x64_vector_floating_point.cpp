/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#include <array>
#include <tuple>
#include <type_traits>
#include <utility>

#include "backend_x64/abi.h"
#include "backend_x64/block_of_code.h"
#include "backend_x64/emit_x64.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/fp/fpcr.h"
#include "common/fp/info.h"
#include "common/fp/op.h"
#include "common/fp/util.h"
#include "common/mp/cartesian_product.h"
#include "common/mp/function_info.h"
#include "common/mp/integer.h"
#include "common/mp/list.h"
#include "common/mp/lut.h"
#include "common/mp/to_tuple.h"
#include "common/mp/vllift.h"
#include "frontend/ir/basic_block.h"
#include "frontend/ir/microinstruction.h"

namespace Dynarmic::BackendX64 {

using namespace Xbyak::util;
namespace mp = Common::mp;

template<size_t fsize, typename T>
static T ChooseOnFsize([[maybe_unused]] T f32, [[maybe_unused]] T f64) {
    static_assert(fsize == 32 || fsize == 64, "fsize must be either 32 or 64");

    if constexpr (fsize == 32) {
        return f32;
    } else {
        return f64;
    }
}

#define FCODE(NAME) (code.*ChooseOnFsize<fsize>(&Xbyak::CodeGenerator::NAME##s, &Xbyak::CodeGenerator::NAME##d))

template<size_t fsize, template<typename> class Indexer, size_t narg>
struct NaNHandler {
public:
    using FPT = mp::unsigned_integer_of_size<fsize>;

    using function_type = void(*)(std::array<VectorArray<FPT>, narg>&);

    static function_type GetDefault() {
        return GetDefaultImpl(std::make_index_sequence<narg - 1>{});
    }

private:
    template<size_t... argi>
    static function_type GetDefaultImpl(std::index_sequence<argi...>) {
        const auto result = [](std::array<VectorArray<FPT>, narg>& values) {
            VectorArray<FPT>& result = values[0];
            for (size_t elementi = 0; elementi < result.size(); ++elementi) {
                const auto current_values = Indexer<FPT>{}(elementi, values[argi + 1]...);
                if (auto r = FP::ProcessNaNs(std::get<argi>(current_values)...)) {
                    result[elementi] = *r;
                } else if (FP::IsNaN(result[elementi])) {
                    result[elementi] = FP::FPInfo<FPT>::DefaultNaN();
                }
            }
        };

        return static_cast<function_type>(result);
    }
};

template<size_t fsize, size_t nargs, typename NaNHandler>
static void HandleNaNs(BlockOfCode& code, EmitContext& ctx, std::array<Xbyak::Xmm, nargs + 1> xmms, const Xbyak::Xmm& nan_mask, NaNHandler nan_handler) {
    static_assert(fsize == 32 || fsize == 64, "fsize must be either 32 or 64");

    if (code.DoesCpuSupport(Xbyak::util::Cpu::tSSE41)) {
        code.ptest(nan_mask, nan_mask);
    } else {
        const Xbyak::Reg32 bitmask = ctx.reg_alloc.ScratchGpr().cvt32();
        code.movmskps(bitmask, nan_mask);
        code.cmp(bitmask, 0);
    }

    Xbyak::Label end;
    Xbyak::Label nan;

    code.jnz(nan, code.T_NEAR);
    code.L(end);

    code.SwitchToFarCode();
    code.L(nan);

    const Xbyak::Xmm result = xmms[0];

    code.sub(rsp, 8);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(result.getIdx()));

    const size_t stack_space = xmms.size() * 16;
    code.sub(rsp, stack_space + ABI_SHADOW_SPACE);
    for (size_t i = 0; i < xmms.size(); ++i) {
        code.movaps(xword[rsp + ABI_SHADOW_SPACE + i * 16], xmms[i]);
    }
    code.lea(code.ABI_PARAM1, ptr[rsp + ABI_SHADOW_SPACE + 0 * 16]);

    code.CallFunction(nan_handler);

    code.movaps(result, xword[rsp + ABI_SHADOW_SPACE + 0 * 16]);
    code.add(rsp, stack_space + ABI_SHADOW_SPACE);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, HostLocXmmIdx(result.getIdx()));
    code.add(rsp, 8);
    code.jmp(end, code.T_NEAR);
    code.SwitchToNearCode();
}

template<typename T>
struct DefaultIndexer {
    std::tuple<T, T> operator()(size_t i, const VectorArray<T>& a, const VectorArray<T>& b) {
        return std::make_tuple(a[i], b[i]);
    }

    std::tuple<T, T, T> operator()(size_t i, const VectorArray<T>& a, const VectorArray<T>& b, const VectorArray<T>& c) {
        return std::make_tuple(a[i], b[i], c[i]);
    }
};

template<typename T>
struct PairedIndexer {
    std::tuple<T, T> operator()(size_t i, const VectorArray<T>& a, const VectorArray<T>& b) {
        constexpr size_t halfway = std::tuple_size_v<VectorArray<T>> / 2;
        const size_t which_array = i / halfway;
        i %= halfway;
        switch (which_array) {
        case 0:
            return std::make_tuple(a[2 * i], a[2 * i + 1]);
        case 1:
            return std::make_tuple(b[2 * i], b[2 * i + 1]);
        }
        UNREACHABLE();
        return {};
    }
};

template<typename T>
struct PairedLowerIndexer {
    std::tuple<T, T> operator()(size_t i, const VectorArray<T>& a, const VectorArray<T>& b) {
        constexpr size_t array_size = std::tuple_size_v<VectorArray<T>>;
        if constexpr (array_size == 4) {
            switch (i) {
            case 0:
                return std::make_tuple(a[0], a[1]);
            case 1:
                return std::make_tuple(b[0], b[1]);
            default:
                return std::make_tuple(0, 0);
            }
        } else if constexpr (array_size == 2) {
            if (i == 0) {
                return std::make_tuple(a[0], b[0]);
            }
            return std::make_tuple(0, 0);
        }
        UNREACHABLE();
        return {};
    }
};

template<size_t fsize, template<typename> class Indexer, typename Function>
static void EmitThreeOpVectorOperation(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, Function fn, typename NaNHandler<fsize, Indexer, 3>::function_type nan_handler = NaNHandler<fsize, Indexer, 3>::GetDefault()) {
    static_assert(fsize == 32 || fsize == 64, "fsize must be either 32 or 64");

    if (!ctx.AccurateNaN() || ctx.FPSCR_DN()) {
        auto args = ctx.reg_alloc.GetArgumentInfo(inst);
        const Xbyak::Xmm xmm_a = ctx.reg_alloc.UseScratchXmm(args[0]);
        const Xbyak::Xmm xmm_b = ctx.reg_alloc.UseXmm(args[1]);

        if constexpr (std::is_member_function_pointer_v<Function>) {
            (code.*fn)(xmm_a, xmm_b);
        } else {
            fn(xmm_a, xmm_b);
        }

        if (ctx.FPSCR_DN()) {
            const Xbyak::Xmm nan_mask = ctx.reg_alloc.ScratchXmm();
            const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();
            code.pcmpeqw(tmp, tmp);
            code.movaps(nan_mask, xmm_a);
            FCODE(cmpordp)(nan_mask, nan_mask);
            code.andps(xmm_a, nan_mask);
            code.xorps(nan_mask, tmp);
            code.andps(nan_mask, fsize == 32 ? code.MConst(xword, 0x7fc0'0000'7fc0'0000, 0x7fc0'0000'7fc0'0000) : code.MConst(xword, 0x7ff8'0000'0000'0000, 0x7ff8'0000'0000'0000));
            code.orps(xmm_a, nan_mask);
        }

        ctx.reg_alloc.DefineValue(inst, xmm_a);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
    const Xbyak::Xmm xmm_a = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm xmm_b = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm nan_mask = ctx.reg_alloc.ScratchXmm();

    code.movaps(nan_mask, xmm_b);
    code.movaps(result, xmm_a);
    FCODE(cmpunordp)(nan_mask, xmm_a);
    if constexpr (std::is_member_function_pointer_v<Function>) {
        (code.*fn)(result, xmm_b);
    } else {
        fn(result, xmm_b);
    }
    FCODE(cmpunordp)(nan_mask, result);

    HandleNaNs<fsize, 2>(code, ctx, {result, xmm_a, xmm_b}, nan_mask, nan_handler);

    ctx.reg_alloc.DefineValue(inst, result);
}

template<size_t fsize, template<typename> class Indexer, typename Function>
static void EmitFourOpVectorOperation(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, Function fn, typename NaNHandler<fsize, Indexer, 4>::function_type nan_handler = NaNHandler<fsize, Indexer, 4>::GetDefault()) {
    static_assert(fsize == 32 || fsize == 64, "fsize must be either 32 or 64");

    if (!ctx.AccurateNaN() || ctx.FPSCR_DN()) {
        auto args = ctx.reg_alloc.GetArgumentInfo(inst);
        const Xbyak::Xmm xmm_a = ctx.reg_alloc.UseScratchXmm(args[0]);
        const Xbyak::Xmm xmm_b = ctx.reg_alloc.UseXmm(args[1]);
        const Xbyak::Xmm xmm_c = ctx.reg_alloc.UseXmm(args[2]);

        if constexpr (std::is_member_function_pointer_v<Function>) {
            (code.*fn)(xmm_a, xmm_b, xmm_c);
        } else {
            fn(xmm_a, xmm_b, xmm_c);
        }

        if (ctx.FPSCR_DN()) {
            const Xbyak::Xmm nan_mask = ctx.reg_alloc.ScratchXmm();
            const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();
            code.pcmpeqw(tmp, tmp);
            code.movaps(nan_mask, xmm_a);
            FCODE(cmpordp)(nan_mask, nan_mask);
            code.andps(xmm_a, nan_mask);
            code.xorps(nan_mask, tmp);
            code.andps(nan_mask, fsize == 32 ? code.MConst(xword, 0x7fc0'0000'7fc0'0000, 0x7fc0'0000'7fc0'0000) : code.MConst(xword, 0x7ff8'0000'0000'0000, 0x7ff8'0000'0000'0000));
            code.orps(xmm_a, nan_mask);
        }

        ctx.reg_alloc.DefineValue(inst, xmm_a);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
    const Xbyak::Xmm xmm_a = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm xmm_b = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm xmm_c = ctx.reg_alloc.UseXmm(args[2]);
    const Xbyak::Xmm nan_mask = ctx.reg_alloc.ScratchXmm();

    code.movaps(nan_mask, xmm_b);
    code.movaps(result, xmm_a);
    FCODE(cmpunordp)(nan_mask, xmm_a);
    FCODE(cmpunordp)(nan_mask, xmm_c);
    if constexpr (std::is_member_function_pointer_v<Function>) {
        (code.*fn)(result, xmm_b, xmm_c);
    } else {
        fn(result, xmm_b, xmm_c);
    }
    FCODE(cmpunordp)(nan_mask, result);

    HandleNaNs<fsize, 3>(code, ctx, {result, xmm_a, xmm_b, xmm_c}, nan_mask, nan_handler);

    ctx.reg_alloc.DefineValue(inst, result);
}

template<typename Lambda>
inline void EmitTwoOpFallback(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, Lambda lambda) {
    const auto fn = static_cast<mp::equivalent_function_type_t<Lambda>*>(lambda);
    
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm arg1 = ctx.reg_alloc.UseXmm(args[0]);
    ctx.reg_alloc.EndOfAllocScope();
    ctx.reg_alloc.HostCall(nullptr);

    constexpr u32 stack_space = 2 * 16;
    code.sub(rsp, stack_space + ABI_SHADOW_SPACE);
    code.lea(code.ABI_PARAM1, ptr[rsp + ABI_SHADOW_SPACE + 0 * 16]);
    code.lea(code.ABI_PARAM2, ptr[rsp + ABI_SHADOW_SPACE + 1 * 16]);
    code.mov(code.ABI_PARAM3.cvt32(), ctx.FPCR());
    code.lea(code.ABI_PARAM4, code.ptr[code.r15 + code.GetJitStateInfo().offsetof_fpsr_exc]);

    code.movaps(xword[code.ABI_PARAM2], arg1);
    code.CallFunction(fn);
    code.movaps(xmm0, xword[rsp + ABI_SHADOW_SPACE + 0 * 16]);

    code.add(rsp, stack_space + ABI_SHADOW_SPACE);

    ctx.reg_alloc.DefineValue(inst, xmm0);
}

template<typename Lambda>
inline void EmitThreeOpFallback(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, Lambda lambda) {
    const auto fn = static_cast<mp::equivalent_function_type_t<Lambda>*>(lambda);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm arg1 = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm arg2 = ctx.reg_alloc.UseXmm(args[1]);
    ctx.reg_alloc.EndOfAllocScope();
    ctx.reg_alloc.HostCall(nullptr);

#ifdef _WIN32
    constexpr u32 stack_space = 4 * 16;
    code.sub(rsp, stack_space + ABI_SHADOW_SPACE);
    code.lea(code.ABI_PARAM1, ptr[rsp + ABI_SHADOW_SPACE + 1 * 16]);
    code.lea(code.ABI_PARAM2, ptr[rsp + ABI_SHADOW_SPACE + 2 * 16]);
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE + 3 * 16]);
    code.mov(code.ABI_PARAM4.cvt32(), ctx.FPCR());
    code.lea(rax, code.ptr[code.r15 + code.GetJitStateInfo().offsetof_fpsr_exc]);
    code.mov(qword[rsp + ABI_SHADOW_SPACE + 0], rax);
#else
    constexpr u32 stack_space = 3 * 16;
    code.sub(rsp, stack_space + ABI_SHADOW_SPACE);
    code.lea(code.ABI_PARAM1, ptr[rsp + ABI_SHADOW_SPACE + 0 * 16]);
    code.lea(code.ABI_PARAM2, ptr[rsp + ABI_SHADOW_SPACE + 1 * 16]);
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE + 2 * 16]);
    code.mov(code.ABI_PARAM4.cvt32(), ctx.FPCR());
    code.lea(code.ABI_PARAM5, code.ptr[code.r15 + code.GetJitStateInfo().offsetof_fpsr_exc]);
#endif

    code.movaps(xword[code.ABI_PARAM2], arg1);
    code.movaps(xword[code.ABI_PARAM3], arg2);
    code.CallFunction(fn);

#ifdef _WIN32
    code.movaps(xmm0, xword[rsp + ABI_SHADOW_SPACE + 1 * 16]);
#else
    code.movaps(xmm0, xword[rsp + ABI_SHADOW_SPACE + 0 * 16]);
#endif

    code.add(rsp, stack_space + ABI_SHADOW_SPACE);

    ctx.reg_alloc.DefineValue(inst, xmm0);
}

template<typename Lambda>
inline void EmitFourOpFallback(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, Lambda lambda) {
    const auto fn = static_cast<mp::equivalent_function_type_t<Lambda>*>(lambda);

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm arg1 = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm arg2 = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm arg3 = ctx.reg_alloc.UseXmm(args[2]);
    ctx.reg_alloc.EndOfAllocScope();
    ctx.reg_alloc.HostCall(nullptr);

#ifdef _WIN32
    constexpr u32 stack_space = 5 * 16;
    code.sub(rsp, stack_space + ABI_SHADOW_SPACE);
    code.lea(code.ABI_PARAM1, ptr[rsp + ABI_SHADOW_SPACE + 1 * 16]);
    code.lea(code.ABI_PARAM2, ptr[rsp + ABI_SHADOW_SPACE + 2 * 16]);
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE + 3 * 16]);
    code.lea(code.ABI_PARAM4, ptr[rsp + ABI_SHADOW_SPACE + 4 * 16]);
    code.mov(qword[rsp + ABI_SHADOW_SPACE + 0], ctx.FPCR());
    code.lea(rax, code.ptr[code.r15 + code.GetJitStateInfo().offsetof_fpsr_exc]);
    code.mov(qword[rsp + ABI_SHADOW_SPACE + 8], rax);
#else
    constexpr u32 stack_space = 4 * 16;
    code.sub(rsp, stack_space + ABI_SHADOW_SPACE);
    code.lea(code.ABI_PARAM1, ptr[rsp + ABI_SHADOW_SPACE + 0 * 16]);
    code.lea(code.ABI_PARAM2, ptr[rsp + ABI_SHADOW_SPACE + 1 * 16]);
    code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE + 2 * 16]);
    code.lea(code.ABI_PARAM4, ptr[rsp + ABI_SHADOW_SPACE + 3 * 16]);
    code.mov(code.ABI_PARAM5.cvt32(), ctx.FPCR());
    code.lea(code.ABI_PARAM6, code.ptr[code.r15 + code.GetJitStateInfo().offsetof_fpsr_exc]);
#endif

    code.movaps(xword[code.ABI_PARAM2], arg1);
    code.movaps(xword[code.ABI_PARAM3], arg2);
    code.movaps(xword[code.ABI_PARAM4], arg3);
    code.CallFunction(fn);

#ifdef _WIN32
    code.movaps(xmm0, xword[rsp + ABI_SHADOW_SPACE + 1 * 16]);
#else
    code.movaps(xmm0, xword[rsp + ABI_SHADOW_SPACE + 0 * 16]);
#endif

    code.add(rsp, stack_space + ABI_SHADOW_SPACE);

    ctx.reg_alloc.DefineValue(inst, xmm0);
}

void EmitX64::EmitFPVectorAbs16(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Address mask = code.MConst(xword, 0x7FFF7FFF7FFF7FFF, 0x7FFF7FFF7FFF7FFF);

    code.pand(a, mask);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorAbs32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Address mask = code.MConst(xword, 0x7FFFFFFF7FFFFFFF, 0x7FFFFFFF7FFFFFFF);

    code.andps(a, mask);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorAbs64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Address mask = code.MConst(xword, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF);

    code.andpd(a, mask);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorAdd32(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<32, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::addps);
}

void EmitX64::EmitFPVectorAdd64(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<64, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::addpd);
}

void EmitX64::EmitFPVectorDiv32(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<32, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::divps);
}

void EmitX64::EmitFPVectorDiv64(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<64, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::divpd);
}

void EmitX64::EmitFPVectorEqual32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm b = ctx.reg_alloc.UseXmm(args[1]);

    code.cmpeqps(a, b);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorEqual64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Xmm b = ctx.reg_alloc.UseXmm(args[1]);

    code.cmpeqpd(a, b);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorGreater32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm a = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm b = ctx.reg_alloc.UseScratchXmm(args[1]);

    code.cmpltps(b, a);

    ctx.reg_alloc.DefineValue(inst, b);
}

void EmitX64::EmitFPVectorGreater64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm a = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm b = ctx.reg_alloc.UseScratchXmm(args[1]);

    code.cmpltpd(b, a);

    ctx.reg_alloc.DefineValue(inst, b);
}

void EmitX64::EmitFPVectorGreaterEqual32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm a = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm b = ctx.reg_alloc.UseScratchXmm(args[1]);

    code.cmpleps(b, a);

    ctx.reg_alloc.DefineValue(inst, b);
}

void EmitX64::EmitFPVectorGreaterEqual64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm a = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm b = ctx.reg_alloc.UseScratchXmm(args[1]);

    code.cmplepd(b, a);

    ctx.reg_alloc.DefineValue(inst, b);
}

template<size_t fsize>
static void EmitFPVectorMax(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<fsize, DefaultIndexer>(code, ctx, inst, [&](const Xbyak::Xmm& result, const Xbyak::Xmm& xmm_b){
        const Xbyak::Xmm mask = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Xmm anded = ctx.reg_alloc.ScratchXmm();

        // What we are doing here is handling the case when the inputs are differently signed zeros.
        // x86-64 treats differently signed zeros as equal while ARM does not.
        // Thus if we AND together things that x86-64 thinks are equal we'll get the positive zero.

        if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX)) {
            FCODE(vcmpeqp)(mask, result, xmm_b);
            FCODE(vandp)(anded, result, xmm_b);
            FCODE(vmaxp)(result, result, xmm_b);
            FCODE(vblendvp)(result, result, anded, mask);
        } else {
            code.movaps(mask, result);
            code.movaps(anded, result);
            FCODE(cmpneqp)(mask, xmm_b);

            code.andps(anded, xmm_b);
            FCODE(maxp)(result, xmm_b);

            code.andps(result, mask);
            code.andnps(mask, anded);
            code.orps(result, mask);
        }
    });
}

void EmitX64::EmitFPVectorMax32(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorMax<32>(code, ctx, inst);
}

void EmitX64::EmitFPVectorMax64(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorMax<64>(code, ctx, inst);
}

template<size_t fsize>
static void EmitFPVectorMin(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<fsize, DefaultIndexer>(code, ctx, inst, [&](const Xbyak::Xmm& result, const Xbyak::Xmm& xmm_b){
        const Xbyak::Xmm mask = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Xmm ored = ctx.reg_alloc.ScratchXmm();

        // What we are doing here is handling the case when the inputs are differently signed zeros.
        // x86-64 treats differently signed zeros as equal while ARM does not.
        // Thus if we OR together things that x86-64 thinks are equal we'll get the negative zero.

        if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX)) {
            FCODE(vcmpeqp)(mask, result, xmm_b);
            FCODE(vorp)(ored, result, xmm_b);
            FCODE(vminp)(result, result, xmm_b);
            FCODE(vblendvp)(result, result, ored, mask);
        } else {
            code.movaps(mask, result);
            code.movaps(ored, result);
            FCODE(cmpneqp)(mask, xmm_b);

            code.orps(ored, xmm_b);
            FCODE(minp)(result, xmm_b);

            code.andps(result, mask);
            code.andnps(mask, ored);
            code.orps(result, mask);
        }
    });
}

void EmitX64::EmitFPVectorMin32(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorMin<32>(code, ctx, inst);
}

void EmitX64::EmitFPVectorMin64(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorMin<64>(code, ctx, inst);
}

void EmitX64::EmitFPVectorMul32(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<32, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::mulps);
}

void EmitX64::EmitFPVectorMul64(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<64, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::mulpd);
}

template<size_t fsize>
void EmitFPVectorMulAdd(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    using FPT = mp::unsigned_integer_of_size<fsize>;

    if (code.DoesCpuSupport(Xbyak::util::Cpu::tFMA)) {
        const auto x64_instruction = fsize == 32 ? &Xbyak::CodeGenerator::vfmadd231ps : &Xbyak::CodeGenerator::vfmadd231pd;
        EmitFourOpVectorOperation<fsize, DefaultIndexer>(code, ctx, inst, x64_instruction,
            static_cast<void(*)(std::array<VectorArray<FPT>, 4>& values)>(
                [](std::array<VectorArray<FPT>, 4>& values) {
                    VectorArray<FPT>& result = values[0];
                    const VectorArray<FPT>& a = values[1];
                    const VectorArray<FPT>& b = values[2];
                    const VectorArray<FPT>& c = values[3];
                    for (size_t i = 0; i < result.size(); i++) {
                        if (FP::IsQNaN(a[i]) && ((FP::IsInf(b[i]) && FP::IsZero(c[i])) || (FP::IsZero(b[i]) && FP::IsInf(c[i])))) {
                            result[i] = FP::FPInfo<FPT>::DefaultNaN();
                        } else if (auto r = FP::ProcessNaNs(a[i], b[i], c[i])) {
                            result[i] = *r;
                        } else if (FP::IsNaN(result[i])) {
                            result[i] = FP::FPInfo<FPT>::DefaultNaN();
                        }
                    }
                }
            )
        );
        return;
    }

    EmitFourOpFallback(code, ctx, inst,
        [](VectorArray<FPT>& result, const VectorArray<FPT>& addend, const VectorArray<FPT>& op1, const VectorArray<FPT>& op2, FP::FPCR fpcr, FP::FPSR& fpsr) {
            for (size_t i = 0; i < result.size(); i++) {
                result[i] = FP::FPMulAdd<FPT>(addend[i], op1[i], op2[i], fpcr, fpsr);
            }
        }
    );
}

void EmitX64::EmitFPVectorMulAdd32(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorMulAdd<32>(code, ctx, inst);
}

void EmitX64::EmitFPVectorMulAdd64(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorMulAdd<64>(code, ctx, inst);
}

void EmitX64::EmitFPVectorNeg16(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Address mask = code.MConst(xword, 0x8000800080008000, 0x8000800080008000);

    code.pxor(a, mask);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorNeg32(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Address mask = code.MConst(xword, 0x8000000080000000, 0x8000000080000000);

    code.pxor(a, mask);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorNeg64(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    const Xbyak::Xmm a = ctx.reg_alloc.UseScratchXmm(args[0]);
    const Xbyak::Address mask = code.MConst(xword, 0x8000000000000000, 0x8000000000000000);

    code.pxor(a, mask);

    ctx.reg_alloc.DefineValue(inst, a);
}

void EmitX64::EmitFPVectorPairedAdd32(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<32, PairedIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::haddps);
}

void EmitX64::EmitFPVectorPairedAdd64(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<64, PairedIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::haddpd);
}

void EmitX64::EmitFPVectorPairedAddLower32(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<32, PairedLowerIndexer>(code, ctx, inst, [&](Xbyak::Xmm result, Xbyak::Xmm xmm_b) {
        const Xbyak::Xmm zero = ctx.reg_alloc.ScratchXmm();
        code.xorps(zero, zero);
        code.punpcklqdq(result, xmm_b);
        code.haddps(result, zero);
    });
}

void EmitX64::EmitFPVectorPairedAddLower64(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<64, PairedLowerIndexer>(code, ctx, inst, [&](Xbyak::Xmm result, Xbyak::Xmm xmm_b) {
        const Xbyak::Xmm zero = ctx.reg_alloc.ScratchXmm();
        code.xorps(zero, zero);
        code.punpcklqdq(result, xmm_b); 
        code.haddpd(result, zero);
    });
}

template<typename FPT>
static void EmitRecipEstimate(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOpFallback(code, ctx, inst, [](VectorArray<FPT>& result, const VectorArray<FPT>& operand, FP::FPCR fpcr, FP::FPSR& fpsr) {
        for (size_t i = 0; i < result.size(); i++) {
            result[i] = FP::FPRecipEstimate<FPT>(operand[i], fpcr, fpsr);
        }
    });
}

void EmitX64::EmitFPVectorRecipEstimate32(EmitContext& ctx, IR::Inst* inst) {
    EmitRecipEstimate<u32>(code, ctx, inst);
}

void EmitX64::EmitFPVectorRecipEstimate64(EmitContext& ctx, IR::Inst* inst) {
    EmitRecipEstimate<u64>(code, ctx, inst);
}

template<typename FPT>
static void EmitRecipStepFused(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpFallback(code, ctx, inst, [](VectorArray<FPT>& result, const VectorArray<FPT>& op1, const VectorArray<FPT>& op2, FP::FPCR fpcr, FP::FPSR& fpsr) {
        for (size_t i = 0; i < result.size(); i++) {
            result[i] = FP::FPRecipStepFused<FPT>(op1[i], op2[i], fpcr, fpsr);
        }
    });
}

void EmitX64::EmitFPVectorRecipStepFused32(EmitContext& ctx, IR::Inst* inst) {
    EmitRecipStepFused<u32>(code, ctx, inst);
}

void EmitX64::EmitFPVectorRecipStepFused64(EmitContext& ctx, IR::Inst* inst) {
    EmitRecipStepFused<u64>(code, ctx, inst);
}

template<typename FPT>
static void EmitRSqrtEstimate(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    EmitTwoOpFallback(code, ctx, inst, [](VectorArray<FPT>& result, const VectorArray<FPT>& operand, FP::FPCR fpcr, FP::FPSR& fpsr) {
        for (size_t i = 0; i < result.size(); i++) {
            result[i] = FP::FPRSqrtEstimate<FPT>(operand[i], fpcr, fpsr);
        }
    });
}

void EmitX64::EmitFPVectorRSqrtEstimate32(EmitContext& ctx, IR::Inst* inst) {
    EmitRSqrtEstimate<u32>(code, ctx, inst);
}

void EmitX64::EmitFPVectorRSqrtEstimate64(EmitContext& ctx, IR::Inst* inst) {
    EmitRSqrtEstimate<u64>(code, ctx, inst);
}

template<typename FPT>
static void EmitRSqrtStepFused(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpFallback(code, ctx, inst, [](VectorArray<FPT>& result, const VectorArray<FPT>& op1, const VectorArray<FPT>& op2, FP::FPCR fpcr, FP::FPSR& fpsr) {
        for (size_t i = 0; i < result.size(); i++) {
            result[i] = FP::FPRSqrtStepFused<FPT>(op1[i], op2[i], fpcr, fpsr);
        }
    });
}

void EmitX64::EmitFPVectorRSqrtStepFused32(EmitContext& ctx, IR::Inst* inst) {
    EmitRSqrtStepFused<u32>(code, ctx, inst);
}

void EmitX64::EmitFPVectorRSqrtStepFused64(EmitContext& ctx, IR::Inst* inst) {
    EmitRSqrtStepFused<u64>(code, ctx, inst);
}

void EmitX64::EmitFPVectorS32ToSingle(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm xmm = ctx.reg_alloc.UseScratchXmm(args[0]);

    code.cvtdq2ps(xmm, xmm);

    ctx.reg_alloc.DefineValue(inst, xmm);
}

void EmitX64::EmitFPVectorS64ToDouble(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm xmm = ctx.reg_alloc.UseScratchXmm(args[0]);

    if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX512VL) && code.DoesCpuSupport(Xbyak::util::Cpu::tAVX512DQ)) {
        code.vcvtqq2pd(xmm, xmm);
    } else if (code.DoesCpuSupport(Xbyak::util::Cpu::tSSE41)) {
        const Xbyak::Xmm xmm_tmp = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr();

        // First quadword
        code.movq(tmp, xmm);
        code.cvtsi2sd(xmm, tmp);

        // Second quadword
        code.pextrq(tmp, xmm, 1);
        code.cvtsi2sd(xmm_tmp, tmp);

        // Combine
        code.unpcklpd(xmm, xmm_tmp);
    } else {
        const Xbyak::Xmm high_xmm = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Xmm xmm_tmp = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr();

        // First quadword
        code.movhlps(high_xmm, xmm);
        code.movq(tmp, xmm);
        code.cvtsi2sd(xmm, tmp);

        // Second quadword
        code.movq(tmp, high_xmm);
        code.cvtsi2sd(xmm_tmp, tmp);

        // Combine
        code.unpcklpd(xmm, xmm_tmp);
    }

    ctx.reg_alloc.DefineValue(inst, xmm);
}

void EmitX64::EmitFPVectorSub32(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<32, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::subps);
}

void EmitX64::EmitFPVectorSub64(EmitContext& ctx, IR::Inst* inst) {
    EmitThreeOpVectorOperation<64, DefaultIndexer>(code, ctx, inst, &Xbyak::CodeGenerator::subpd);
}

template<size_t fsize, bool unsigned_>
void EmitFPVectorToFixed(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst) {
    using FPT = mp::unsigned_integer_of_size<fsize>;

    const size_t fbits = inst->GetArg(1).GetU8();
    const auto rounding = static_cast<FP::RoundingMode>(inst->GetArg(2).GetU8());

    if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX) && rounding != FP::RoundingMode::ToNearest_TieAwayFromZero) {
        const Xbyak::Xmm src = ctx.reg_alloc.UseScratchXmm(args[0]);
        const Xbyak::Xmm scratch = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Xmm lower_limit_mask = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Xmm upper_limit_mask = ctx.reg_alloc.ScratchXmm();

        const int round_imm = [&]{
            switch (rounding) {
            case FP::RoundingMode::ToNearest_TieEven:
                return 0b00;
            case FP::RoundingMode::TowardsPlusInfinity:
                return 0b10;
            case FP::RoundingMode::TowardsMinusInfinity:
                return 0b01;
            case FP::RoundingMode::TowardsZero:
                return 0b11;
            default:
                UNREACHABLE();
            }
            return 0;
        }();

        if (fbits != 0) {
            const u64 scale_factor_scalar = static_cast<u64>((fbits + FPInfo<FPT>::exponent_bias) << FPInfo<FPT>::explicit_mantissa_width);
            FCODE(vbroadcasts)(scratch, code.MConst(ptr, scale_factor_scalar));
            FCODE(vmulp)(src, src, scratch);
        }

        FCODE(vroundp)(src, src, round_imm);

        // Values should be in [fp_lower_limit, fp_upper_limit).
        constexpr u64 fp_lower_limit = unsigned_
                                     ? (fsize == 32 ? 0x00000000u : 0x0000000000000000u)
                                     : (fsize == 32 ? 0xcf000000u : 0xc3e0000000000000u);
        constexpr u64 fp_upper_limit = unsigned_
                                     ? (fsize == 32 ? 0x4f800000u : 0x43f0000000000000u)
                                     : (fsize == 32 ? 0x4f000000u : 0x43e0000000000000u);

        //                        lower    upper
        //                        limit    limit
        //                        mask     mask
        //
        // src contains NaN       false    false
        //
        // src < lower            true     false
        //
        // lower <= src < upper   false    false
        //
        // src >= upper           false    true
        //

        if (fp_lower_limit == 0) {
            FCODE(vxorp)(scratch, scratch, scratch);
            FCODE(vcmpnge_uqp)(lower_limit_mask, scratch, src);
        } else {
            FCODE(vbroadcasts)(scratch, code.MConst(ptr, fp_lower_limit));
            FCODE(vcmplt_oqp)(lower_limit_mask, scratch, src);
        }

        FCODE(vbroadcasts)(scratch, code.MConst(ptr, fp_upper_limit));
        FCODE(vcmpge_oqp)(upper_limit_mask, src, scratch);

        FCODE(vandnp)(src, upper_limit_mask, src);
        FCODE(vandnp)(src, lower_limit_mask, src);

        if (fp_lower_limit != 0) {
        }

        return;
    }

    using fbits_list = mp::vllift<std::make_index_sequence<fsize>>;
    using rounding_list = mp::list<
        std::integral_constant<FP::RoundingMode, FP::RoundingMode::ToNearest_TieEven>,
        std::integral_constant<FP::RoundingMode, FP::RoundingMode::TowardsPlusInfinity>,
        std::integral_constant<FP::RoundingMode, FP::RoundingMode::TowardsMinusInfinity>,
        std::integral_constant<FP::RoundingMode, FP::RoundingMode::TowardsZero>,
        std::integral_constant<FP::RoundingMode, FP::RoundingMode::ToNearest_TieAwayFromZero>
    >;

    using key_type = std::tuple<size_t, FP::RoundingMode>;
    using value_type = void(*)(VectorArray<FPT>&, const VectorArray<FPT>&, FP::FPCR, FP::FPSR&);

    static const auto lut = mp::GenerateLookupTableFromList<key_type, value_type>(
        [](auto arg) {
            return std::pair<key_type, value_type>{
                mp::to_tuple<decltype(arg)>,
                static_cast<value_type>(
                    [](VectorArray<FPT>& output, const VectorArray<FPT>& input, FP::FPCR fpcr, FP::FPSR& fpsr) {
                        constexpr size_t fbits = std::get<0>(mp::to_tuple<decltype(arg)>);
                        constexpr FP::RoundingMode rounding_mode = std::get<1>(mp::to_tuple<decltype(arg)>);

                        for (size_t i = 0; i < output.size(); ++i) {
                            output[i] = static_cast<FPT>(FP::FPToFixed<FPT>(fsize, input[i], fbits, unsigned_, fpcr, rounding_mode, fpsr));
                        }
                    }
                )
            };
        },
        mp::cartesian_product<fbits_list, rounding_list>{}
    );

    EmitTwoOpFallback(code, ctx, inst, lut.at(std::make_tuple(fbits, rounding)));
}

void EmitX64::EmitFPVectorToSignedFixed32(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorToFixed<32, false>(code, ctx, inst);
}

void EmitX64::EmitFPVectorToSignedFixed64(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorToFixed<64, false>(code, ctx, inst);
}

void EmitX64::EmitFPVectorToUnsignedFixed32(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorToFixed<32, true>(code, ctx, inst);
}

void EmitX64::EmitFPVectorToUnsignedFixed64(EmitContext& ctx, IR::Inst* inst) {
    EmitFPVectorToFixed<64, true>(code, ctx, inst);
}

void EmitX64::EmitFPVectorU32ToSingle(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm xmm = ctx.reg_alloc.UseScratchXmm(args[0]);

    if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX512DQ) && code.DoesCpuSupport(Xbyak::util::Cpu::tAVX512VL)) {
        code.vcvtudq2ps(xmm, xmm);
    } else {
        const Xbyak::Address mem_4B000000 = code.MConst(xword, 0x4B0000004B000000, 0x4B0000004B000000);
        const Xbyak::Address mem_53000000 = code.MConst(xword, 0x5300000053000000, 0x5300000053000000);
        const Xbyak::Address mem_D3000080 = code.MConst(xword, 0xD3000080D3000080, 0xD3000080D3000080);

        const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();

        if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX)) {
            code.vpblendw(tmp, xmm, mem_4B000000, 0b10101010);
            code.vpsrld(xmm, xmm, 16);
            code.vpblendw(xmm, xmm, mem_53000000, 0b10101010);
            code.vaddps(xmm, xmm, mem_D3000080);
            code.vaddps(xmm, tmp, xmm);
        } else {
            const Xbyak::Address mem_0xFFFF = code.MConst(xword, 0x0000FFFF0000FFFF, 0x0000FFFF0000FFFF);

            code.movdqa(tmp, mem_0xFFFF);

            code.pand(tmp, xmm);
            code.por(tmp, mem_4B000000);
            code.psrld(xmm, 16);
            code.por(xmm, mem_53000000);
            code.addps(xmm, mem_D3000080);
            code.addps(xmm, tmp);
        }
    }

    if (ctx.FPSCR_RMode() == FP::RoundingMode::TowardsMinusInfinity) {
        code.pand(xmm, code.MConst(xword, 0x7FFFFFFF7FFFFFFF, 0x7FFFFFFF7FFFFFFF));
    }

    ctx.reg_alloc.DefineValue(inst, xmm);
}

void EmitX64::EmitFPVectorU64ToDouble(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Xmm xmm = ctx.reg_alloc.UseScratchXmm(args[0]);

    if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX512DQ) && code.DoesCpuSupport(Xbyak::util::Cpu::tAVX512VL)) {
        code.vcvtuqq2pd(xmm, xmm);
    } else {
        const Xbyak::Address unpack = code.MConst(xword, 0x4530000043300000, 0);
        const Xbyak::Address subtrahend = code.MConst(xword, 0x4330000000000000, 0x4530000000000000);

        const Xbyak::Xmm unpack_reg = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Xmm subtrahend_reg = ctx.reg_alloc.ScratchXmm();
        const Xbyak::Xmm tmp1 = ctx.reg_alloc.ScratchXmm();

        if (code.DoesCpuSupport(Xbyak::util::Cpu::tAVX)) {
            code.vmovapd(unpack_reg, unpack);
            code.vmovapd(subtrahend_reg, subtrahend);

            code.vunpcklps(tmp1, xmm, unpack_reg);
            code.vsubpd(tmp1, tmp1, subtrahend_reg);

            code.vpermilps(xmm, xmm, 0b01001110);

            code.vunpcklps(xmm, xmm, unpack_reg);
            code.vsubpd(xmm, xmm, subtrahend_reg);

            code.vhaddpd(xmm, tmp1, xmm);
        } else {
            const Xbyak::Xmm tmp2 = ctx.reg_alloc.ScratchXmm();

            code.movapd(unpack_reg, unpack);
            code.movapd(subtrahend_reg, subtrahend);

            code.pshufd(tmp1, xmm, 0b01001110);

            code.punpckldq(xmm, unpack_reg);
            code.subpd(xmm, subtrahend_reg);
            code.pshufd(tmp2, xmm, 0b01001110);
            code.addpd(xmm, tmp2);

            code.punpckldq(tmp1, unpack_reg);
            code.subpd(tmp1, subtrahend_reg);

            code.pshufd(unpack_reg, tmp1, 0b01001110);
            code.addpd(unpack_reg, tmp1);

            code.unpcklpd(xmm, unpack_reg);
        }
    }

    if (ctx.FPSCR_RMode() == FP::RoundingMode::TowardsMinusInfinity) {
        code.pand(xmm, code.MConst(xword, 0x7FFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF));
    }

    ctx.reg_alloc.DefineValue(inst, xmm);
}

} // namespace Dynarmic::BackendX64
