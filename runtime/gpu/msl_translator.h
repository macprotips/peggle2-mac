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
        uint32_t interpolators = 0;       // bit n: on written (vertex shaders)
        uint32_t inputsRead = 0;          // bit n: interpolant n (rn on entry) may be read (pixel shaders)
        // Vertex shaders whose position is a float vertex fetch at the vertex index, unchanged: where that
        // fetch reads (dwords, in fetch constant positionSlot). positionStride is 0 for any other shader.
        uint32_t positionSlot = 0, positionOffset = 0, positionStride = 0;
        std::string errors;               // untranslatable instructions, one per line
    };

    // MSL for the `xe_fragment` entry point that calls a translated pixel shader. Textures bind in
    // ascending fetch slot order at [[texture(n)]] / [[sampler(n)]]. The interpolants passed and the alpha
    // test are function constants (xe_interpolants, xe_alpha_func), set per pipeline.
    std::string FragmentEntry(const ShaderInfo& info);

    class MslTranslator : public xe::gpu::ShaderTranslator
    {
    public:
        // Returns MSL defining `xe_vs_main` or `xe_ps_main` (entry points: kMslVertexEntry in msl_prelude.h,
        // FragmentEntry).
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
        struct Fetched;
        Fetched PositionFetch(const xe::gpu::ParsedVertexFetchInstruction& instr) const;
        Fetched PassThroughPosition(const xe::gpu::ParsedAluInstruction& instr) const;
        void Line(const std::string& text);
        std::string Condition(uint32_t boolIndex, bool condition, bool predicated);

        // Registers holding an unchanged float vertex fetch at the vertex index (positionStride 0: none),
        // and the position's, to fill ShaderInfo's position fields.
        struct Fetched
        {
            uint32_t slot = 0, offset = 0, stride = 0;
        };
        Fetched fetched_[64];
        Fetched position_;
        uint64_t writtenRegisters_ = 0;
        uint32_t positionWrites_ = 0;
        bool branches_ = false; // loops, calls or jumps: instruction order is not execution order

        std::string body_;
        int indent_ = 0;
        bool inExecCondition_ = false;
        ShaderInfo* info_ = nullptr;
    };
}
