// Xenos microcode to MSL. Opcode semantics follow Xenia's SPIR-V translator (BSD-3-Clause).
#include "msl_translator.h"
#include <cctype>
#include <cstdlib>
#include <fmt/format.h>

using namespace xe::gpu;
using namespace xe::gpu::ucode;

namespace rt::gpu
{
    static const char* kComponent = "xyzw";

    std::string MslTranslator::Translate(Shader& shader, ShaderInfo& info)
    {
        info = {};
        info.isPixel = shader.type() == xenos::ShaderType::kPixel;
        info_ = &info;
        Shader::Translation* translation = shader.GetOrCreateTranslation(0);
        if (!TranslateAnalyzedShader(*translation))
            return {};
        const auto& bytes = translation->translated_binary();
        std::string body(bytes.begin(), bytes.end());
        if (!info.isPixel && positionWrites_ == 1 && !branches_)
        {
            info.positionSlot = position_.slot;
            info.positionOffset = position_.offset;
            info.positionStride = position_.stride;
        }
        // Interpolants arrive in r0-r15; any register the code names may be one it reads (a relative
        // index may name any).
        if (info.isPixel)
        {
            if (body.find("st.r[(") != std::string::npos)
                info.inputsRead = 0xFFFF;
            for (size_t at = body.find("st.r["); at != std::string::npos; at = body.find("st.r[", at + 1))
                if (isdigit(static_cast<unsigned char>(body[at + 5])))
                    if (const int n = atoi(body.c_str() + at + 5); n < 16)
                        info.inputsRead |= 1u << n;
        }
        return body;
    }

    void MslTranslator::Line(const std::string& text)
    {
        body_.append(size_t(indent_) * 4, ' ');
        body_ += text;
        body_ += '\n';
    }

    void MslTranslator::EmitTranslationError(const char* message, bool)
    {
        info_->errors += message;
        info_->errors += '\n';
    }

    std::string FragmentEntry(const ShaderInfo& info)
    {
        std::string s = "struct XeFragmentOut\n{\n";
        for (uint32_t i = 0; i < 4; i++)
        {
            if (info.colorTargets & (1u << i))
                s += fmt::format("    float4 c{} [[color({})]];\n", i, i);
        }
        if (info.writesDepth)
            s += "    float depth [[depth(any)]];\n";
        if (!info.colorTargets && !info.writesDepth)
            s += "    float4 unused [[color(0)]];\n";
        s += "};\n\n";

        s += "fragment XeFragmentOut xe_fragment(XeVertexOut in [[stage_in]], bool frontFacing [[front_facing]], "
             "constant XeDraw& d [[buffer(0)]], constant float4* c [[buffer(1)]], constant uint* boolLoop [[buffer(2)]], "
             "constant float* texScale [[buffer(3)]]";
        std::string textureArgs;
        uint32_t binding = 0;
        for (uint32_t t = 0; t < 32; t++)
        {
            if (!(info.textureSlots & (1u << t)))
                continue;
            const char* type = info.textureDimensions[t] == uint32_t(xenos::FetchOpDimension::k3DOrStacked) ? "texture3d" :
                info.textureDimensions[t] == uint32_t(xenos::FetchOpDimension::kCube) ? "texturecube" : "texture2d";
            s += fmt::format(", {}<float> t{} [[texture({})]], sampler s{} [[sampler({})]]", type, t, binding, t, binding);
            textureArgs += fmt::format(", t{}, s{}", t, t);
            binding++;
        }
        s += ")\n{\n";
        s += "    XeState st = {};\n";
        for (uint32_t i = 0; i < 16; i++)
            s += fmt::format("    if (xe_has_o{}) st.r[{}] = in.o{};\n", i, i, i);
        s += "    XePixelIO io;\n    io.fragCoord = float4(in.position.xy / d.pixelScale, in.position.zw);\n"
             "    io.frontFacing = frontFacing;\n    io.texScale = texScale;\n";
        s += "    xe_ps_main(st, c, boolLoop, io" + textureArgs + ");\n";
        s += "    if (st.killed)\n        discard_fragment();\n";
        // Alpha test: the compare function's bits are less (1), equal (2), greater (4). Always (7) compiles
        // to nothing, so pipelines without the test keep the GPU's fast paths for shaders that never discard.
        s += "    if (xe_alpha_func != 7u)\n    {\n"
             "        float alpha = st.color[0].w;\n"
             "        uint pass = alpha < d.alphaRef ? 1u : (alpha == d.alphaRef ? 2u : 4u);\n"
             "        if ((xe_alpha_func & pass) == 0u)\n            discard_fragment();\n    }\n";
        s += "    XeFragmentOut out;\n";
        for (uint32_t i = 0; i < 4; i++)
        {
            if (info.colorTargets & (1u << i))
                s += fmt::format("    out.c{} = st.color[{}];\n", i, i);
        }
        if (info.writesDepth)
            s += "    out.depth = st.depth;\n";
        if (!info.colorTargets && !info.writesDepth)
            s += "    out.unused = float4(0.0);\n";
        s += "    return out;\n}\n";
        return s;
    }

    void MslTranslator::StartTranslation()
    {
        body_.clear();
        indent_ = 2;
        inExecCondition_ = false;
        std::fill(std::begin(fetched_), std::end(fetched_), Fetched{});
        position_ = {};
        writtenRegisters_ = 0;
        positionWrites_ = 0;
        branches_ = false;
    }

    std::vector<uint8_t> MslTranslator::CompleteTranslation()
    {
        const bool pixel = is_pixel_shader();
        std::string s;
        s += fmt::format("// Translated from Xenos microcode ({} shader).\n", pixel ? "pixel" : "vertex");
        if (pixel)
        {
            s += "static void xe_ps_main(thread XeState& st, constant float4* c, constant uint* boolLoop, XePixelIO io";
            for (uint32_t t = 0; t < 32; t++)
            {
                if (info_->textureSlots & (1u << t))
                {
                    const char* type = info_->textureDimensions[t] == uint32_t(xenos::FetchOpDimension::k3DOrStacked) ? "texture3d" :
                        info_->textureDimensions[t] == uint32_t(xenos::FetchOpDimension::kCube) ? "texturecube" : "texture2d";
                    s += fmt::format(", {}<float> t{}, sampler s{}", type, t, t);
                }
            }
            s += ")\n{\n";
        }
        else
        {
            s += "static void xe_vs_main(thread XeState& st, constant float4* c, constant uint* boolLoop, constant uint* fetch, device const uint* mem)\n{\n";
        }
        s += "    int pc = 0;\n    while (true)\n    {\n        switch (pc)\n        {\n        case 0:\n";
        s += body_;
        s += "        default:\n            return;\n        }\n    }\n}\n";
        return { s.begin(), s.end() };
    }

    void MslTranslator::ProcessControlFlowInstructionBegin(uint32_t cfIndex)
    {
        // Every control flow instruction is a possible jump target.
        if (cfIndex)
        {
            indent_ = 1;
            Line(fmt::format("    case {}:", cfIndex));
            indent_ = 2;
        }
    }

    std::string MslTranslator::Condition(uint32_t boolIndex, bool condition, bool predicated)
    {
        if (predicated)
            return condition ? "st.p0" : "!st.p0";
        std::string test = fmt::format("((boolLoop[{}] >> {}) & 1u)", boolIndex >> 5, boolIndex & 31);
        return condition ? test + " != 0u" : test + " == 0u";
    }

    void MslTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr)
    {
        inExecCondition_ = instr.type != ParsedExecInstruction::Type::kUnconditional;
        if (inExecCondition_)
        {
            Line(fmt::format("if ({})", Condition(instr.bool_constant_index, instr.condition, instr.type == ParsedExecInstruction::Type::kPredicated)));
            Line("{");
            indent_++;
        }
    }

    void MslTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr)
    {
        if (inExecCondition_)
        {
            indent_--;
            Line("}");
            inExecCondition_ = false;
        }
        if (instr.is_end)
            Line("return;");
    }

    void MslTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr)
    {
        branches_ = true;
        // Loop constant: count in bits 0:7, start in 8:15, step (signed) in 16:23.
        Line("{");
        indent_++;
        Line(fmt::format("uint loopConstant = boolLoop[8 + {}];", instr.loop_constant_index));
        Line("st.loopDepth++;");
        Line("st.loopCounter[st.loopDepth] = int(loopConstant & 0xFFu);");
        if (!instr.is_repeat)
            Line("st.aL[st.loopDepth] = int((loopConstant >> 8) & 0xFFu);");
        else
            Line("st.aL[st.loopDepth] = st.aL[st.loopDepth - 1];");
        Line(fmt::format("if (st.loopCounter[st.loopDepth] == 0) {{ st.loopDepth--; pc = {}; continue; }}", instr.loop_skip_address));
        indent_--;
        Line("}");
    }

    void MslTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr)
    {
        Line("{");
        indent_++;
        Line(fmt::format("uint loopConstant = boolLoop[8 + {}];", instr.loop_constant_index));
        Line("st.loopCounter[st.loopDepth]--;");
        Line("st.aL[st.loopDepth] += int(char((loopConstant >> 16) & 0xFFu));");
        std::string again = "st.loopCounter[st.loopDepth] != 0";
        if (instr.is_predicated_break)
            again += instr.predicate_condition ? " && !st.p0" : " && st.p0";
        Line(fmt::format("if ({}) {{ pc = {}; continue; }}", again, instr.loop_body_address));
        Line("st.loopDepth--;");
        indent_--;
        Line("}");
    }

    void MslTranslator::ProcessCallInstruction(const ParsedCallInstruction& instr)
    {
        branches_ = true;
        std::string call = fmt::format("{{ st.callStack[st.callDepth++] = {}; pc = {}; continue; }}", instr.dword_index + 1, instr.target_address);
        if (instr.type == ParsedCallInstruction::Type::kUnconditional)
            Line(call);
        else
            Line(fmt::format("if ({}) {}", Condition(instr.bool_constant_index, instr.condition, instr.type == ParsedCallInstruction::Type::kPredicated), call));
    }

    void MslTranslator::ProcessReturnInstruction(const ParsedReturnInstruction&)
    {
        Line("if (st.callDepth == 0) return;");
        Line("pc = st.callStack[--st.callDepth]; continue;");
    }

    void MslTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr)
    {
        branches_ = true;
        std::string jump = fmt::format("{{ pc = {}; continue; }}", instr.target_address);
        if (instr.type == ParsedJumpInstruction::Type::kUnconditional)
            Line(jump);
        else
            Line(fmt::format("if ({}) {}", Condition(instr.bool_constant_index, instr.condition, instr.type == ParsedJumpInstruction::Type::kPredicated), jump));
    }

    // --- Operands and results ------------------------------------------------------------------

    static std::string Storage(const InstructionOperand& op)
    {
        std::string index;
        switch (op.storage_addressing_mode)
        {
        case InstructionStorageAddressingMode::kAbsolute: index = std::to_string(op.storage_index); break;
        case InstructionStorageAddressingMode::kAddressRegisterRelative: index = fmt::format("(st.a0 + {}) & 255", op.storage_index); break;
        case InstructionStorageAddressingMode::kLoopRelative: index = fmt::format("(st.aL[st.loopDepth] + {}) & {}", op.storage_index, op.storage_source == InstructionStorageSource::kRegister ? 63 : 255); break;
        }
        return op.storage_source == InstructionStorageSource::kConstantFloat ? fmt::format("c[{}]", index) : fmt::format("st.r[{}]", index);
    }

    static char SwizzleChar(SwizzleSource s)
    {
        switch (s)
        {
        case SwizzleSource::kX: return 'x';
        case SwizzleSource::kY: return 'y';
        case SwizzleSource::kZ: return 'z';
        case SwizzleSource::kW: return 'w';
        default: return 'x';
        }
    }

    static std::string Modifiers(const InstructionOperand& op, std::string value)
    {
        if (op.is_absolute_value)
            value = "abs(" + value + ")";
        if (op.is_negated)
            value = "(-" + value + ")";
        return value;
    }

    std::string MslTranslator::Operand(const InstructionOperand& op)
    {
        std::string swizzle;
        for (uint32_t i = 0; i < 4; i++)
            swizzle += SwizzleChar(op.GetComponent(i));
        return Modifiers(op, Storage(op) + "." + swizzle);
    }

    std::string MslTranslator::ScalarComponent(const InstructionOperand& op, uint32_t component)
    {
        return Modifiers(op, Storage(op) + "." + SwizzleChar(op.GetComponent(component)));
    }

    // Writes `value` (a float4 expression) through the result's write mask and swizzle.
    void MslTranslator::StoreResult(const InstructionResult& result, const std::string& value)
    {
        uint32_t mask = result.GetUsedWriteMask();
        if (result.storage_target == InstructionStorageTarget::kNone || !mask)
            return;

        if (result.storage_target == InstructionStorageTarget::kRegister)
        {
            if (result.storage_addressing_mode == InstructionStorageAddressingMode::kLoopRelative)
            {
                std::fill(std::begin(fetched_), std::end(fetched_), Fetched{});
                writtenRegisters_ = ~0ull;
            }
            else
            {
                fetched_[result.storage_index & 63] = {};
                writtenRegisters_ |= 1ull << (result.storage_index & 63);
            }
        }
        else if (result.storage_target == InstructionStorageTarget::kPosition)
        {
            positionWrites_++;
            position_ = {};
        }

        std::string target;
        switch (result.storage_target)
        {
        case InstructionStorageTarget::kRegister:
            target = result.storage_addressing_mode == InstructionStorageAddressingMode::kLoopRelative
                ? fmt::format("st.r[(st.aL[st.loopDepth] + {}) & 63]", result.storage_index)
                : fmt::format("st.r[{}]", result.storage_index);
            break;
        case InstructionStorageTarget::kInterpolator:
            target = fmt::format("st.o[{}]", result.storage_index);
            info_->interpolators |= 1u << result.storage_index;
            break;
        case InstructionStorageTarget::kPosition: target = "st.position"; break;
        case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex: target = "st.pointSize"; break;
        case InstructionStorageTarget::kColor:
            target = fmt::format("st.color[{}]", result.storage_index);
            info_->colorTargets |= 1u << result.storage_index;
            break;
        case InstructionStorageTarget::kDepth:
            target = "st.depth";
            info_->writesDepth = true;
            break;
        default:
            return; // memexport is not used by this title
        }

        Line("{");
        indent_++;
        Line(fmt::format("float4 v = {};", value));
        if (result.is_clamped)
            Line("v = saturate(v);");
        for (uint32_t i = 0; i < 4; i++)
        {
            if (!(mask & (1u << i)))
                continue;
            std::string source;
            switch (result.components[i])
            {
            case SwizzleSource::k0: source = "0.0"; break;
            case SwizzleSource::k1: source = "1.0"; break;
            default: source = std::string("v.") + SwizzleChar(result.components[i]); break;
            }
            if (result.storage_target == InstructionStorageTarget::kDepth)
            {
                if (i == 0)
                    Line(fmt::format("{} = {};", target, source));
            }
            else
                Line(fmt::format("{}.{} = {};", target, kComponent[i], source));
        }
        indent_--;
        Line("}");
    }

    // --- ALU -----------------------------------------------------------------------------------

    void MslTranslator::ProcessAluInstruction(const ParsedAluInstruction& instr, uint8_t)
    {
        if (instr.IsNop())
            return;

        std::vector<std::string> pre; // emitted inside the predicate check, before the results
        const auto& v = instr.vector_operands;
        const auto& s = instr.scalar_operands;
        std::string vector, scalar;
        bool writesPredicate = false;

        auto opA = [&](uint32_t i) { return Operand(v[i]); };
        switch (instr.vector_opcode)
        {
        case AluVectorOpcode::kAdd: vector = fmt::format("{} + {}", opA(0), opA(1)); break;
        case AluVectorOpcode::kMul: vector = fmt::format("xe_mul({}, {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kMax: vector = fmt::format("xe_max({}, {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kMin: vector = fmt::format("xe_min({}, {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kSeq: vector = fmt::format("select(float4(0.0), float4(1.0), {} == {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kSgt: vector = fmt::format("select(float4(0.0), float4(1.0), {} > {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kSge: vector = fmt::format("select(float4(0.0), float4(1.0), {} >= {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kSne: vector = fmt::format("select(float4(0.0), float4(1.0), {} != {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kFrc: vector = fmt::format("fract({})", opA(0)); break;
        case AluVectorOpcode::kTrunc: vector = fmt::format("trunc({})", opA(0)); break;
        case AluVectorOpcode::kFloor: vector = fmt::format("floor({})", opA(0)); break;
        case AluVectorOpcode::kMad: vector = fmt::format("xe_mul({}, {}) + {}", opA(0), opA(1), opA(2)); break;
        case AluVectorOpcode::kCndEq: vector = fmt::format("select({2}, {1}, {0} == 0.0)", opA(0), opA(1), opA(2)); break;
        case AluVectorOpcode::kCndGe: vector = fmt::format("select({2}, {1}, {0} >= 0.0)", opA(0), opA(1), opA(2)); break;
        case AluVectorOpcode::kCndGt: vector = fmt::format("select({2}, {1}, {0} > 0.0)", opA(0), opA(1), opA(2)); break;
        case AluVectorOpcode::kDp4: vector = fmt::format("float4(xe_dot4({}, {}))", opA(0), opA(1)); break;
        case AluVectorOpcode::kDp3: vector = fmt::format("float4(xe_dot3({}, {}))", opA(0), opA(1)); break;
        case AluVectorOpcode::kDp2Add: vector = fmt::format("float4(xe_dot2add({}, {}, {}))", opA(0), opA(1), opA(2)); break;
        case AluVectorOpcode::kCube: vector = fmt::format("xe_cube({}, {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kMax4: vector = fmt::format("float4(xe_max4({}))", opA(0)); break;
        case AluVectorOpcode::kSetpEqPush:
        case AluVectorOpcode::kSetpNePush:
        case AluVectorOpcode::kSetpGtPush:
        case AluVectorOpcode::kSetpGePush:
        {
            const char* op = instr.vector_opcode == AluVectorOpcode::kSetpEqPush ? "==" :
                instr.vector_opcode == AluVectorOpcode::kSetpNePush ? "!=" :
                instr.vector_opcode == AluVectorOpcode::kSetpGtPush ? ">" : ">=";
            pre.push_back(fmt::format("float4 pushA = {}; float4 pushB = {};", opA(0), opA(1)));
            pre.push_back(fmt::format("bool pushP = pushA.w == 0.0 && pushB.w {} 0.0;", op));
            vector = fmt::format("float4(((pushA.x == 0.0 && pushB.x {} 0.0) ? -1.0 : pushA.x) + 1.0)", op);
            writesPredicate = true;
            break;
        }
        case AluVectorOpcode::kKillEq:
        case AluVectorOpcode::kKillGt:
        case AluVectorOpcode::kKillGe:
        case AluVectorOpcode::kKillNe:
        {
            const char* op = instr.vector_opcode == AluVectorOpcode::kKillEq ? "==" :
                instr.vector_opcode == AluVectorOpcode::kKillGt ? ">" :
                instr.vector_opcode == AluVectorOpcode::kKillGe ? ">=" : "!=";
            pre.push_back(fmt::format("if (any({} {} {})) st.killed = true;", opA(0), op, opA(1)));
            vector = "float4(0.0)";
            break;
        }
        case AluVectorOpcode::kDst: vector = fmt::format("xe_dst({}, {})", opA(0), opA(1)); break;
        case AluVectorOpcode::kMaxA:
            pre.push_back(fmt::format("int newA0 = int(clamp(floor({}.w + 0.5), -256.0, 255.0));", opA(0)));
            vector = fmt::format("xe_max({}, {})", opA(0), opA(1));
            break;
        default:
            EmitTranslationError(fmt::format("unhandled vector opcode {}", uint32_t(instr.vector_opcode)).c_str(), false);
            vector = "float4(0.0)";
            break;
        }

        auto sa = [&]() { return ScalarComponent(s[0], 0); };
        auto sb = [&]() { return ScalarComponent(s[0], 1); };
        bool scalarWritesA0 = false;
        switch (instr.scalar_opcode)
        {
        case AluScalarOpcode::kAdds: scalar = fmt::format("{} + {}", sa(), sb()); break;
        case AluScalarOpcode::kAddsPrev: scalar = fmt::format("{} + st.ps", sa()); break;
        case AluScalarOpcode::kMuls: scalar = fmt::format("xe_mul({}, {})", sa(), sb()); break;
        case AluScalarOpcode::kMulsPrev: scalar = fmt::format("xe_mul({}, st.ps)", sa()); break;
        case AluScalarOpcode::kMulsPrev2: scalar = fmt::format("xe_mul_prev2({}, {}, st.ps)", sa(), sb()); break;
        case AluScalarOpcode::kMaxs: scalar = fmt::format("xe_max({}, {})", sa(), sb()); break;
        case AluScalarOpcode::kMins: scalar = fmt::format("xe_min({}, {})", sa(), sb()); break;
        case AluScalarOpcode::kSeqs: scalar = fmt::format("({} == 0.0 ? 1.0 : 0.0)", sa()); break;
        case AluScalarOpcode::kSgts: scalar = fmt::format("({} > 0.0 ? 1.0 : 0.0)", sa()); break;
        case AluScalarOpcode::kSges: scalar = fmt::format("({} >= 0.0 ? 1.0 : 0.0)", sa()); break;
        case AluScalarOpcode::kSnes: scalar = fmt::format("({} != 0.0 ? 1.0 : 0.0)", sa()); break;
        case AluScalarOpcode::kFrcs: scalar = fmt::format("fract({})", sa()); break;
        case AluScalarOpcode::kTruncs: scalar = fmt::format("trunc({})", sa()); break;
        case AluScalarOpcode::kFloors: scalar = fmt::format("floor({})", sa()); break;
        case AluScalarOpcode::kExp: scalar = fmt::format("exp2({})", sa()); break;
        case AluScalarOpcode::kLogc: scalar = fmt::format("xe_clamp_neg_inf(log2({}))", sa()); break;
        case AluScalarOpcode::kLog: scalar = fmt::format("log2({})", sa()); break;
        case AluScalarOpcode::kRcpc: scalar = fmt::format("xe_clamp_inf(1.0 / {})", sa()); break;
        case AluScalarOpcode::kRcpf: scalar = fmt::format("xe_inf_to_zero(1.0 / {})", sa()); break;
        case AluScalarOpcode::kRcp: scalar = fmt::format("(1.0 / {})", sa()); break;
        case AluScalarOpcode::kRsqc: scalar = fmt::format("xe_clamp_inf(rsqrt({}))", sa()); break;
        case AluScalarOpcode::kRsqf: scalar = fmt::format("xe_inf_to_zero(rsqrt({}))", sa()); break;
        case AluScalarOpcode::kRsq: scalar = fmt::format("rsqrt({})", sa()); break;
        case AluScalarOpcode::kMaxAs:
            pre.push_back(fmt::format("int newA0s = int(clamp(floor({} + 0.5), -256.0, 255.0));", sa()));
            scalar = fmt::format("xe_max({}, {})", sa(), sb());
            scalarWritesA0 = true;
            break;
        case AluScalarOpcode::kMaxAsf:
            pre.push_back(fmt::format("int newA0s = int(clamp(floor({}), -256.0, 255.0));", sa()));
            scalar = fmt::format("xe_max({}, {})", sa(), sb());
            scalarWritesA0 = true;
            break;
        case AluScalarOpcode::kSubs: scalar = fmt::format("{} - {}", sa(), sb()); break;
        case AluScalarOpcode::kSubsPrev: scalar = fmt::format("{} - st.ps", sa()); break;
        case AluScalarOpcode::kSetpEq:
        case AluScalarOpcode::kSetpNe:
        case AluScalarOpcode::kSetpGt:
        case AluScalarOpcode::kSetpGe:
        {
            const char* op = instr.scalar_opcode == AluScalarOpcode::kSetpEq ? "==" :
                instr.scalar_opcode == AluScalarOpcode::kSetpNe ? "!=" :
                instr.scalar_opcode == AluScalarOpcode::kSetpGt ? ">" : ">=";
            pre.push_back(fmt::format("bool setP = {} {} 0.0;", sa(), op));
            scalar = "(setP ? 0.0 : 1.0)";
            writesPredicate = true;
            break;
        }
        case AluScalarOpcode::kSetpInv:
            pre.push_back(fmt::format("float setA = {}; bool setP = setA == 1.0;", sa()));
            scalar = "(setP ? 0.0 : (setA == 0.0 ? 1.0 : setA))";
            writesPredicate = true;
            break;
        case AluScalarOpcode::kSetpPop:
            pre.push_back(fmt::format("float setA = {} - 1.0; bool setP = setA <= 0.0;", sa()));
            scalar = "(setP ? 0.0 : setA)";
            writesPredicate = true;
            break;
        case AluScalarOpcode::kSetpClr:
            pre.push_back("bool setP = false;");
            scalar = "FLT_MAX";
            writesPredicate = true;
            break;
        case AluScalarOpcode::kSetpRstr:
            pre.push_back(fmt::format("float setA = {}; bool setP = setA == 0.0;", sa()));
            scalar = "(setP ? 0.0 : setA)";
            writesPredicate = true;
            break;
        case AluScalarOpcode::kKillsEq:
        case AluScalarOpcode::kKillsGt:
        case AluScalarOpcode::kKillsGe:
        case AluScalarOpcode::kKillsNe:
        case AluScalarOpcode::kKillsOne:
        {
            const char* test = instr.scalar_opcode == AluScalarOpcode::kKillsEq ? "== 0.0" :
                instr.scalar_opcode == AluScalarOpcode::kKillsGt ? "> 0.0" :
                instr.scalar_opcode == AluScalarOpcode::kKillsGe ? ">= 0.0" :
                instr.scalar_opcode == AluScalarOpcode::kKillsNe ? "!= 0.0" : "== 1.0";
            pre.push_back(fmt::format("if ({} {}) st.killed = true;", sa(), test));
            scalar = "0.0";
            break;
        }
        case AluScalarOpcode::kSqrt: scalar = fmt::format("sqrt({})", sa()); break;
        case AluScalarOpcode::kMulsc0:
        case AluScalarOpcode::kMulsc1: scalar = fmt::format("xe_mul({}, {})", ScalarComponent(s[0], 0), ScalarComponent(s[1], 0)); break;
        case AluScalarOpcode::kAddsc0:
        case AluScalarOpcode::kAddsc1: scalar = fmt::format("{} + {}", ScalarComponent(s[0], 0), ScalarComponent(s[1], 0)); break;
        case AluScalarOpcode::kSubsc0:
        case AluScalarOpcode::kSubsc1: scalar = fmt::format("{} - {}", ScalarComponent(s[0], 0), ScalarComponent(s[1], 0)); break;
        case AluScalarOpcode::kSin: scalar = fmt::format("sin({})", sa()); break;
        case AluScalarOpcode::kCos: scalar = fmt::format("cos({})", sa()); break;
        case AluScalarOpcode::kRetainPrev: scalar = "st.ps"; break;
        default:
            EmitTranslationError(fmt::format("unhandled scalar opcode {}", uint32_t(instr.scalar_opcode)).c_str(), false);
            scalar = "0.0";
            break;
        }

        Line("{");
        indent_++;
        if (instr.is_predicated)
        {
            Line(fmt::format("if ({})", instr.predicate_condition ? "st.p0" : "!st.p0"));
            Line("{");
            indent_++;
        }
        for (const std::string& line : pre)
            Line(line);
        // Both halves read their operands before either result is written.
        Line(fmt::format("float4 vr = {};", vector));
        Line(fmt::format("float sr = {};", scalar));
        if (instr.vector_opcode == AluVectorOpcode::kMaxA)
            Line("st.a0 = newA0;");
        if (scalarWritesA0)
            Line("st.a0 = newA0s;");
        if (writesPredicate)
            Line(instr.vector_opcode >= AluVectorOpcode::kSetpEqPush && instr.vector_opcode <= AluVectorOpcode::kSetpGePush ? "st.p0 = pushP;" : "st.p0 = setP;");
        const Fetched passThrough = PassThroughPosition(instr);
        StoreResult(instr.vector_and_constant_result, "vr");
        if (instr.vector_and_constant_result.storage_target == InstructionStorageTarget::kPosition)
            position_ = passThrough;
        StoreResult(instr.scalar_result, "float4(sr)");
        if (instr.scalar_opcode != AluScalarOpcode::kRetainPrev)
            Line("st.ps = sr;");
        if (instr.is_predicated)
        {
            indent_--;
            Line("}");
        }
        indent_--;
        Line("}");
    }

    // The vertex fetch, if it reads float x and y at the vertex index (r0.x on entry) into xy of a register.
    MslTranslator::Fetched MslTranslator::PositionFetch(const ParsedVertexFetchInstruction& instr) const
    {
        const auto& a = instr.attributes;
        const auto& index = instr.operands[0];
        const auto& r = instr.result;
        const bool floats = a.data_format == xenos::VertexFormat::k_32_32_FLOAT || a.data_format == xenos::VertexFormat::k_32_32_32_FLOAT ||
                            a.data_format == xenos::VertexFormat::k_32_32_32_32_FLOAT;
        if (instr.is_mini_fetch || instr.is_predicated || inExecCondition_ || !floats || a.exp_adjust || a.offset < 0 ||
            index.storage_source != InstructionStorageSource::kRegister || index.storage_index != 0 ||
            index.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute || index.is_negated ||
            index.is_absolute_value || index.GetComponent(0) != SwizzleSource::kX || (writtenRegisters_ & 1) ||
            r.storage_target != InstructionStorageTarget::kRegister || r.storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
            r.is_clamped || (r.GetUsedWriteMask() & 3) != 3 || r.components[0] != SwizzleSource::kX || r.components[1] != SwizzleSource::kY)
            return {};
        return { instr.operands[1].storage_index, uint32_t(a.offset), a.stride };
    }

    // `max oPos, rN, rN` (a move) from a register holding such a fetch.
    MslTranslator::Fetched MslTranslator::PassThroughPosition(const ParsedAluInstruction& instr) const
    {
        const auto& r = instr.vector_and_constant_result;
        if (instr.vector_opcode != AluVectorOpcode::kMax || instr.is_predicated || inExecCondition_ ||
            r.storage_target != InstructionStorageTarget::kPosition || r.is_clamped || (r.GetUsedWriteMask() & 3) != 3 ||
            r.components[0] != SwizzleSource::kX || r.components[1] != SwizzleSource::kY)
            return {};
        const auto& a = instr.vector_operands[0];
        const auto& b = instr.vector_operands[1];
        for (const auto* op : { &a, &b })
        {
            if (op->storage_source != InstructionStorageSource::kRegister || op->storage_addressing_mode != InstructionStorageAddressingMode::kAbsolute ||
                op->is_negated || op->is_absolute_value || op->GetComponent(0) != SwizzleSource::kX || op->GetComponent(1) != SwizzleSource::kY)
                return {};
        }
        return a.storage_index == b.storage_index ? fetched_[a.storage_index & 63] : Fetched{};
    }

    // --- Fetch ---------------------------------------------------------------------------------

    void MslTranslator::ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& instr)
    {
        Line("{");
        indent_++;
        if (instr.is_predicated)
        {
            Line(fmt::format("if ({})", instr.predicate_condition ? "st.p0" : "!st.p0"));
            Line("{");
            indent_++;
        }
        const auto& a = instr.attributes;
        if (!instr.is_mini_fetch)
        {
            // Address of element `index`: base + index * stride, in dwords; kept for vfetch_mini.
            const auto& index = instr.operands[0];
            uint32_t slot = instr.operands[1].storage_index;
            Line(fmt::format("st.fetchConstant = {};", slot));
            Line(fmt::format("{{ float i = {}; st.fetchIndex = int({}); }}", ScalarComponent(index, 0), a.is_index_rounded ? "floor(i + 0.5)" : "floor(i)"));
            Line(fmt::format("st.fetchStride = {};", a.stride));
        }
        Line(fmt::format("float4 f = xe_vfetch(mem, fetch, st.fetchConstant, uint(st.fetchIndex * st.fetchStride + {}), {}, {}, {}, {}, {});",
            a.offset, uint32_t(a.data_format), a.is_signed ? "true" : "false", a.is_integer ? "true" : "false",
            uint32_t(a.signed_rf_mode), a.exp_adjust));
        StoreResult(instr.result, "f");
        if (const Fetched fetched = PositionFetch(instr); fetched.stride)
            fetched_[instr.result.storage_index] = fetched;
        if (instr.is_predicated)
        {
            indent_--;
            Line("}");
        }
        indent_--;
        Line("}");
    }

    void MslTranslator::ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& instr)
    {
        if (!instr.has_result())
            return;
        if (!is_pixel_shader())
        {
            EmitTranslationError("vertex texture fetch is not supported", false);
            return;
        }
        if (instr.opcode != FetchOpcode::kTextureFetch)
        {
            // getTextureWeights, setTextureLod and friends are unused by this title.
            EmitTranslationError(fmt::format("unhandled texture opcode {}", instr.opcode_name).c_str(), false);
            return;
        }
        uint32_t slot = instr.operands[1].storage_index;
        info_->textureSlots |= 1u << slot;
        info_->textureDimensions[slot] = uint32_t(instr.dimension);

        std::string coord = Operand(instr.operands[0]);
        std::string sample;
        const auto& a = instr.attributes;
        std::string offset = fmt::format("float3({}, {}, {})", a.offset_x, a.offset_y, a.offset_z);
        switch (instr.dimension)
        {
        case xenos::FetchOpDimension::k1D:
        case xenos::FetchOpDimension::k2D:
        {
            // Texel units are guest texels; a scaled resolve copy has more host texels.
            std::string size = fmt::format("(float2(t{}.get_width(), t{}.get_height()) / io.texScale[{}])", slot, slot, slot);
            std::string uv = a.unnormalized_coordinates ? fmt::format("(({}.xy + {}.xy) / {})", coord, offset, size)
                                                        : fmt::format("({}.xy + {}.xy / {})", coord, offset, size);
            sample = a.lod_bias != 0.0f ? fmt::format("t{}.sample(s{}, {}, bias({}))", slot, slot, uv, a.lod_bias)
                                        : fmt::format("t{}.sample(s{}, {})", slot, slot, uv);
            break;
        }
        case xenos::FetchOpDimension::k3DOrStacked:
            sample = fmt::format("t{}.sample(s{}, {}.xyz)", slot, slot, coord);
            break;
        case xenos::FetchOpDimension::kCube:
            // Xenos cube coordinates come from the cube instruction: (t, s, 2 * major axis, face).
            sample = fmt::format("t{}.sample(s{}, xe_cube_direction({}))", slot, slot, coord);
            break;
        }
        if (instr.is_predicated)
        {
            Line(fmt::format("if ({})", instr.predicate_condition ? "st.p0" : "!st.p0"));
            Line("{");
            indent_++;
        }
        StoreResult(instr.result, sample);
        if (instr.is_predicated)
        {
            indent_--;
            Line("}");
        }
    }
}
