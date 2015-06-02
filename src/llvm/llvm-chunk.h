// Copyright 2015 ISP RAS. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_LLVM_CHUNK_H_
#define V8_LLVM_CHUNK_H_

#include "llvm-headers.h"

// TODO(llvm): get rid of the ugly ".."
#include "../hydrogen.h"
#include "../hydrogen-instructions.h"
#include "../handles.h"
#include "../lithium.h"
#include "mcjit-memory-manager.h"

#include <memory>

namespace v8 {
namespace internal {

// TODO(llvm): move this class to a separate file. Or, better, 2 files
class LLVMGranularity FINAL {
 public:
  static LLVMGranularity& getInstance() {
    static LLVMGranularity instance;
    return instance;
  }

  // TODO(llvm):
//  ~LLVMGranularity() {
//    llvm::llvm_shutdown();
//  }

  LLVMContext& context() { return context_; }
  MCJITMemoryManager* memory_manager_ref() { return memory_manager_ref_; }

  std::unique_ptr<llvm::Module> CreateModule(std::string name = "") {
    if ("" == name) {
      name = GenerateName();
    }
    return llvm::make_unique<llvm::Module>(name, context_);
  }

  void AddModule(std::unique_ptr<llvm::Module> module) {
    if (!engine_) {
      std::unique_ptr<MCJITMemoryManager>manager = MCJITMemoryManager::Create();
      memory_manager_ref_ = manager.get(); // non-owning!
      llvm::TargetOptions options;
      // rbp based frame so the runtime can walk the stack as before
      options.NoFramePointerElim = true;
      llvm::ExecutionEngine* raw = llvm::EngineBuilder(std::move(module))
        .setMCJITMemoryManager(std::move(manager))
        .setErrorStr(&err_str_)
        .setEngineKind(llvm::EngineKind::JIT)
        .setTargetOptions(options)
        .setOptLevel(llvm::CodeGenOpt::Aggressive) // backend opt level
        .create();
      engine_ = std::unique_ptr<llvm::ExecutionEngine>(raw);
      CHECK(engine_);
    } else {
      engine_->addModule(std::move(module));
    }
    // Finalize each time after adding a new module
    // (assuming the added module is constructed and won't change)
    engine_->finalizeObject();
  }

  void OptimizeFunciton(llvm::Module* module, llvm::Function* function) {
    // TODO(llvm): 1). Instead of using -O3 optimizations, add the
    // appropriate passes manually
    // TODO(llvm): 2). I didn't manage to make use of new PassManagers.
    // llvm::legacy:: things should probably be removed with time.
    // But for now even the llvm optimizer (llvm/tools/opt/opt.cpp) uses them.
    // TODO(llvm): 3). (Probably could be resolved easily when 2. is done)
    // for now we set up the passes for each module (and each function).
    // It would be much nicer if we could just set the passes once
    // and then in OptimizeFunciton() and OptimizeModule() simply run them.
    llvm::legacy::FunctionPassManager pass_manager(module);
    pass_manager_builder_.populateFunctionPassManager(pass_manager);
    pass_manager.doInitialization();
    pass_manager.run(*function);
    pass_manager.doFinalization();
  }

  void OptimizeModule(llvm::Module* module) {
    // TODO(llvm): see OptimizeFunciton TODOs (ditto)
    llvm::legacy::PassManager pass_manager;
    pass_manager_builder_.populateModulePassManager(pass_manager);
    pass_manager.run(*module);
  }

  uint64_t GetFunctionAddress(int id) {
    DCHECK(engine_);
    return engine_->getFunctionAddress(std::to_string(id));
  }

  void Err() {
    std::cerr << err_str_ << std::endl;
  }
 private:
  LLVMContext context_;
  llvm::PassManagerBuilder pass_manager_builder_;
  std::unique_ptr<llvm::ExecutionEngine> engine_;
  int count_;
  MCJITMemoryManager* memory_manager_ref_; // non-owning ptr
  std::string err_str_;

  LLVMGranularity()
      : context_(),
        pass_manager_builder_(),
        engine_(nullptr),
        count_(0),
        memory_manager_ref_(nullptr),
        err_str_() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    pass_manager_builder_.OptLevel = 3; // -O3
  }

  std::string GenerateName() {
    return std::to_string(count_++);
  }

  DISALLOW_COPY_AND_ASSIGN(LLVMGranularity);
};

class LLVMChunk FINAL : public LowChunk {
 public:
  virtual ~LLVMChunk();
  LLVMChunk(CompilationInfo* info, HGraph* graph)
    : LowChunk(info, graph),
      llvm_function_id_(-1) {}

  static LLVMChunk* NewChunk(HGraph *graph);

  Handle<Code> Codegen() override;

  void set_llvm_function_id(int id) { llvm_function_id_ = id; }
  int llvm_function_id() { return llvm_function_id_; }

 private:
  int llvm_function_id_;
};

class LLVMChunkBuilder FINAL : public LowChunkBuilderBase {
 public:
  LLVMChunkBuilder(CompilationInfo* info, HGraph* graph)
      : LowChunkBuilderBase(info, graph),
        current_instruction_(nullptr),
        current_block_(nullptr),
        next_block_(nullptr),
        module_(nullptr),
        function_(nullptr),
        llvm_ir_builder_(nullptr) {}
  ~LLVMChunkBuilder() {}

  LLVMChunk* chunk() const { return static_cast<LLVMChunk*>(chunk_); };
  LLVMChunkBuilder& Build();
  // LLVM requires that each phi input's label be a basic block
  // immediately preceding the given BB.
  // Hydrogen does not impose such a constraint.
  // For that reason our phis are not LLVM-compliant right after phi resolution.
  LLVMChunkBuilder& NormalizePhis();
  LLVMChunkBuilder& Optimize(); // invoke llvm transformation passes for the function
  LLVMChunk* Create();

  LLVMEnvironment* CreateEnvironment(
      HEnvironment* hydrogen_env, int* argument_index_accumulator,
      ZoneList<HValue*>* objects_to_materialize);

  llvm::BasicBlock* DeoptimizeIf(HInstruction* instr, // TODO (llvm): usage?
                    Deoptimizer::DeoptReason deopt_reason);

  // Declare methods that deal with the individual node types.
#define DECLARE_DO(type) void Do##type(H##type* node);
  HYDROGEN_CONCRETE_INSTRUCTION_LIST(DECLARE_DO)
#undef DECLARE_DO

 private:
  static const int kSmiShift = kSmiTagSize + kSmiShiftSize;

  static llvm::CmpInst::Predicate TokenToPredicate(Token::Value op,
                                                   bool is_unsigned);

  void DoBasicBlock(HBasicBlock* block, HBasicBlock* next_block);
  void VisitInstruction(HInstruction* current);
  void DoPhi(HPhi* phi);
  void ResolvePhis();
  void ResolvePhis(HBasicBlock* block);
  // if the llvm counterpart of the block does not exist, create it
  llvm::BasicBlock* Use(HBasicBlock* block);
  llvm::Value* Use(HValue* value);
  llvm::Value* SmiToInteger32(HValue* value);
  llvm::Value* Integer32ToSmi(HValue* value);
  // Is the value (not) a smi?
  llvm::Value* SmiCheck(HValue* value, bool negate = false);
  llvm::Value* CallVoid(Address target);
  void DoBadThing(llvm::Value* compare, Address target, HBasicBlock* block);

  // TODO(llvm): probably pull these up to LowChunkBuilderBase
  HInstruction* current_instruction_;
  HBasicBlock* current_block_;
  HBasicBlock* next_block_;
  // module_ ownership is later passed to the execution engine (MCJIT)
  std::unique_ptr<llvm::Module> module_;
  // Non-owning pointer to the function inside llvm module.
  // Not to be used for fetching the actual native code,
  // since the corresponding methods are deprecated.
  llvm::Function* function_;
  std::unique_ptr<llvm::IRBuilder<>> llvm_ir_builder_;
};

class LLVMEnvironment {
 public:
  LLVMEnvironment(Handle<JSFunction> closure,
               FrameType frame_type,
               BailoutId ast_id,
               int parameter_count,
               int argument_count,
               int value_count,
               LLVMEnvironment* outer,
               HEnterInlined* entry,
               Zone* zone)
      : closure_(closure),
        frame_type_(frame_type),
        arguments_stack_height_(argument_count),
        deoptimization_index_(Safepoint::kNoDeoptimizationIndex),
        translation_index_(-1),
        ast_id_(ast_id),
        translation_size_(value_count),
        parameter_count_(parameter_count),
        pc_offset_(-1),
        values_(value_count, zone),
        is_tagged_(value_count, zone),
        is_uint32_(value_count, zone),
        object_mapping_(0, zone),
        outer_(outer),
        entry_(entry),
        zone_(zone),
        has_been_used_(false) { }
  // Marker value indicating a de-materialized object.
  static llvm::Value* materialization_marker() { return nullptr; }

 private:
  Handle<JSFunction> closure_;
  FrameType frame_type_;
  int arguments_stack_height_;
  int deoptimization_index_;
  int translation_index_;
  BailoutId ast_id_;
  int translation_size_;
  int parameter_count_;
  int pc_offset_;

  // Value array: [parameters] [locals] [expression stack] [de-materialized].
  //              |>--------- translation_size ---------<|
  ZoneList<llvm::Value*> values_;
  GrowableBitVector is_tagged_;
  GrowableBitVector is_uint32_;

  // Map with encoded information about materialization_marker operands.
  ZoneList<uint32_t> object_mapping_;

  LEnvironment* outer_;
  HEnterInlined* entry_;
  Zone* zone_;
  bool has_been_used_;
};

class LLVMDeoptArtifacts {
 public:
 private:
  ZoneList<LLVMEnvironment*> deoptimizations_;
  TranslationBuffer translations_;
};

}  // namespace internal
}  // namespace v8
#endif  // V8_LLVM_CHUNK_H_
