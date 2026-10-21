// Translates Xenos shader microcode to Metal Shading Language source.
// Instruction semantics follow Xenia's translators (BSD-3-Clause, see THIRD_PARTY_NOTICES.md).
#pragma once
#include <cstdint>
#include <string>
#include "xenia/gpu/shader_translator.h"

namespace rt::gpu
{
    // What the generated code needs bound at draw time.
    struct ShaderInfo
    {
        bool isPixel = false;
        uint32_t textureSlots = 0;        // bit n: texture fetch constant n is sampled
        uint32_t textureDimensions[32]{}; // xenos::FetchOpDimension per slot
        uint32_t colorTargets = 0;        // bit n: oCn written (pixel shaders)
        bool writesDepth = false;
        bool usesKill = false;
        uint32_t interpolators = 0;       // bit n: on written (vertex shaders)
        std::string errors;               // untranslatable instructions, one per line
    };

    // MSL for the `xe_fragment` entry point that calls a translated pixel shader. Textures bind in
    // ascending fetch slot order at [[texture(n)]] / [[sampler(n)]].
    std::string FragmentEntry(const ShaderInfo& info);

    class MslTranslator : public xe::gpu::ShaderTranslator
    {
    public:
        // Returns MSL defining `xe_vs_main` or `xe_ps_main` (see renderer.mm for the entry points).
        std::string Translate(xe::gpu::Shader& shader, ShaderInfo& info);

    protected:
        void EmitTranslationError(const char* message, bool isFatal) override;
        void StartTranslation() override;
        std::vector<uint8_t> CompleteTranslation() override;
        void ProcessControlFlowInstructionBegin(uint32_t cfIndex) override;
        void ProcessExecInstructionBegin(const xe::gpu::ParsedExecInstruction& instr) override;
        void ProcessExecInstructionEnd(const xe::gpu::ParsedExecInstruction& instr) override;
        void ProcessLoopStartInstruction(const xe::gpu::ParsedLoopStartInstruction& instr) override;
        void ProcessLoopEndInstruction(const xe::gpu::ParsedLoopEndInstruction& instr) override;
        void ProcessCallInstruction(const xe::gpu::ParsedCallInstruction& instr) override;
        void ProcessReturnInstruction(const xe::gpu::ParsedReturnInstruction& instr) override;
        void ProcessJumpInstruction(const xe::gpu::ParsedJumpInstruction& instr) override;
        void ProcessVertexFetchInstruction(const xe::gpu::ParsedVertexFetchInstruction& instr) override;
        void ProcessTextureFetchInstruction(const xe::gpu::ParsedTextureFetchInstruction& instr) override;
        void ProcessAluInstruction(const xe::gpu::ParsedAluInstruction& instr, uint8_t memexportWritten) override;

    private:
        std::string Operand(const xe::gpu::InstructionOperand& op);
        std::string ScalarComponent(const xe::gpu::InstructionOperand& op, uint32_t component);
        void StoreResult(const xe::gpu::InstructionResult& result, const std::string& value);
        void Line(const std::string& text);
        std::string Condition(uint32_t boolIndex, bool condition, bool predicated);

        std::string body_;
        int indent_ = 0;
        bool inExecCondition_ = false;
        ShaderInfo* info_ = nullptr;
    };
}
