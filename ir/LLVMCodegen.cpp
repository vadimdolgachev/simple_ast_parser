//
// Created by vadim on 06.10.24.
//

#include <iostream>

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/IR/Instructions.h>

#include "ast/FunctionNode.h"
#include "ast/IdentNode.h"
#include "ast/AssignmentNode.h"
#include "ast/NumberNode.h"
#include "ast/FunctionCallNode.h"
#include "ast/BinOpNode.h"
#include "ast/UnaryOpNode.h"
#include "ast/ForLoopNode.h"
#include "ast/IfStatement.h"
#include "ast/BooleanNode.h"
#include "ast/ProtoFunctionStatement.h"
#include "ast/StringNode.h"
#include "ast/TypeNode.h"

#include "LLVMCodegen.h"

#include "ast/ReturnNode.h"

namespace {
    llvm::Function *getModuleFunction(const std::string &name,
                                      const std::unique_ptr<llvm::IRBuilder<>> &builder,
                                      const std::unique_ptr<llvm::Module> &module,
                                      ModuleContext &mc) {
        // First, see if the function has already been added to the current module.
        if (auto *const function = module->getFunction(name)) {
            return function;
        }

        // If not, check whether we can codegen the declaration from some existing
        // prototype.
        if (const auto proto = mc.functions.find(name); proto != mc.functions.end()) {
            auto *const fun = LLVMCodegen::generate(proto->second.get(),
                                                    builder,
                                                    module,
                                                    mc);
            return llvm::dyn_cast<llvm::Function>(fun);
        }

        // If no existing prototype exists, return null.
        return nullptr;
    }

    llvm::Type *generateType(const std::unique_ptr<PrimitiveType> &typeNode,
                             llvm::LLVMContext &context) {
        llvm::Type *llvmType = nullptr;
        switch (typeNode->type) {
                using enum PrimitiveTypeKind;
            case Boolean: {
                llvmType = llvm::Type::getInt1Ty(context);
                break;
            }
            case Byte: {
                llvmType = llvm::Type::getInt8Ty(context);
                break;
            }
            case Char: {
                llvmType = llvm::Type::getInt8Ty(context);
                break;
            }
            case Double: {
                llvmType = llvm::Type::getDoubleTy(context);
                break;
            }
            case Integer: {
                llvmType = llvm::Type::getInt32Ty(context);
                break;
            }
            case Void: {
                llvmType = llvm::Type::getVoidTy(context);
                break;
            }
            case Str: {
                llvmType = llvm::PointerType::get(llvm::Type::getInt8Ty(context), 0);
                break;
            }
            default: {
                throw std::logic_error("Unknown type");
            }
        }
        if (typeNode->isPointer) {
            llvmType = llvm::PointerType::get(llvmType, 0);
        }

        return llvmType;
    }

    llvm::Type *getResultType(llvm::Type *lhsType, llvm::Type *rhsType, llvm::LLVMContext &context) {
        if (lhsType == rhsType) {
            return lhsType;
        }

        if (lhsType->isDoubleTy() || rhsType->isDoubleTy()) {
            return llvm::Type::getDoubleTy(context);
        }

        if (lhsType->isIntegerTy() && rhsType->isIntegerTy()) {
            const unsigned lhsBits = lhsType->getIntegerBitWidth();
            const unsigned rhsBits = rhsType->getIntegerBitWidth();
            return lhsBits > rhsBits ? lhsType : rhsType;
        }

        return nullptr;
    }

    std::string typeToString(const llvm::Type *type) {
        std::string typeStr;
        llvm::raw_string_ostream rso(typeStr);
        type->print(rso);
        return rso.str();
    }

    llvm::Value *tryCastValue(const std::unique_ptr<llvm::IRBuilder<>> &builder,
                              llvm::Value *const value,
                              llvm::Type *const destType) {
        if (value->getType() == destType) {
            return value;
        }

        const llvm::Type *const srcType = value->getType();

        auto getCastOp = [&]() -> std::optional<llvm::Instruction::CastOps> {
            if (destType->isIntegerTy(1) && !value->getType()->isIntegerTy(1)) {
                return std::nullopt;
            }

            // Floating -> Integer
            if (srcType->isFloatingPointTy() && destType->isIntegerTy()) {
                return llvm::Instruction::FPToSI;
            }

            // Integer -> Floating
            if (srcType->isIntegerTy() && destType->isFloatingPointTy()) {
                return llvm::Instruction::SIToFP;
            }

            // Integer extension/truncation
            if (srcType->isIntegerTy() && destType->isIntegerTy()) {
                const unsigned srcBits = srcType->getIntegerBitWidth();
                const unsigned destBits = destType->getIntegerBitWidth();

                if (srcBits == 1) {
                    return llvm::Instruction::ZExt;
                }
                return destBits > srcBits
                           ? llvm::Instruction::SExt
                           : llvm::Instruction::Trunc;
            }

            return std::nullopt;
        };

        if (const auto castOp = getCastOp()) {
            return builder->CreateCast(*castOp, value, destType);
        }

        throw std::logic_error("Unsupported cast from " +
                               typeToString(srcType) + " to " +
                               typeToString(destType));
    }

    llvm::Value *createAdd(const std::unique_ptr<llvm::IRBuilder<>> &builder, llvm::Value *lhs, llvm::Value *rhs,
                           const llvm::Type *type) {
        return type->isFloatingPointTy()
                   ? builder->CreateFAdd(lhs, rhs, "fadd_tmp")
                   : builder->CreateAdd(lhs, rhs, "iadd_tmp");
    }

    llvm::Value *createSub(const std::unique_ptr<llvm::IRBuilder<>> &builder, llvm::Value *lhs, llvm::Value *rhs,
                           const llvm::Type *type) {
        return type->isFloatingPointTy()
                   ? builder->CreateFSub(lhs, rhs, "fsub_tmp")
                   : builder->CreateSub(lhs, rhs, "isub_tmp");
    }

    llvm::Value *createMul(const std::unique_ptr<llvm::IRBuilder<>> &builder, llvm::Value *lhs, llvm::Value *rhs,
                           const llvm::Type *type) {
        return type->isFloatingPointTy()
                   ? builder->CreateFMul(lhs, rhs, "fmul_tmp")
                   : builder->CreateMul(lhs, rhs, "imul_tmp");
    }

    llvm::Value *createDiv(const std::unique_ptr<llvm::IRBuilder<>> &builder, llvm::Value *lhs, llvm::Value *rhs,
                           const llvm::Type *type) {
        return type->isFloatingPointTy()
                   ? builder->CreateFDiv(lhs, rhs, "fdiv_tmp")
                   : builder->CreateSDiv(lhs, rhs, "sdiv_tmp");
    }

    llvm::Value *createCompare(const std::unique_ptr<llvm::IRBuilder<>> &builder,
                               const TokenType op,
                               llvm::Value *lhs,
                               llvm::Value *rhs) {
        llvm::CmpInst::Predicate pred;

        if (lhs->getType()->isFloatingPointTy()) {
            switch (op) {
                case TokenType::LeftAngleBracket:
                    pred = llvm::CmpInst::FCMP_OLT;
                    break;
                case TokenType::LeftAngleBracketEqual:
                    pred = llvm::CmpInst::FCMP_OLE;
                    break;
                case TokenType::RightAngleBracket:
                    pred = llvm::CmpInst::FCMP_OGT;
                    break;
                case TokenType::RightAngleBracketEqual:
                    pred = llvm::CmpInst::FCMP_OGE;
                    break;
                case TokenType::Equal:
                    pred = llvm::CmpInst::FCMP_OEQ;
                    break;
                case TokenType::NotEqual:
                    pred = llvm::CmpInst::FCMP_ONE;
                    break;
                default:
                    throw std::logic_error("Unsupported float comparison");
            }
            return builder->CreateFCmp(pred, lhs, rhs, "fcmp");
        }
        switch (op) {
            case TokenType::LeftAngleBracket:
                pred = llvm::CmpInst::ICMP_SLT;
                break;
            case TokenType::LeftAngleBracketEqual:
                pred = llvm::CmpInst::ICMP_SLT;
                break;
            case TokenType::RightAngleBracket:
                pred = llvm::CmpInst::ICMP_SGT;
                break;
            case TokenType::RightAngleBracketEqual:
                pred = llvm::CmpInst::ICMP_SGE;
                break;
            case TokenType::Equal:
                pred = llvm::CmpInst::ICMP_EQ;
                break;
            case TokenType::NotEqual:
                pred = llvm::CmpInst::ICMP_NE;
                break;
            default:
                throw std::logic_error("Unsupported integer comparison");
        }
        return builder->CreateICmp(pred, lhs, rhs, "icmp");
    }

    void generateBasicBlock(const BlockNode::Statements &statements,
                            const std::unique_ptr<llvm::IRBuilder<>> &builder,
                            const std::unique_ptr<llvm::Module> &module,
                            ModuleContext &mc,
                            const std::optional<std::function<void(llvm::BasicBlock *)>> &prologue = std::nullopt,
                            llvm::Function *const parentFunction = nullptr) {
        auto *const basicBlock = llvm::BasicBlock::Create(module->getContext(),
                                                          "entry",
                                                          parentFunction);
        mc.symTable.enterScope();

        llvm::IRBuilderBase::InsertPointGuard guard(*builder);
        builder->SetInsertPoint(basicBlock);
        if (prologue.has_value()) {
            (*prologue)(basicBlock);
        }

        bool hasTerminator = false;
        for (const auto &stmt: statements) {
            llvm::Value *val = LLVMCodegen::generate(stmt.get(), builder, module, mc);
            if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(val)) {
                if (inst->isTerminator()) {
                    hasTerminator = true;
                    break;
                }
            }
        }

        if (!hasTerminator) {
            llvm::IRBuilder tmpBuilder(basicBlock);
            if (parentFunction->getReturnType()->isVoidTy()) {
                tmpBuilder.CreateRetVoid();
            } else {
                tmpBuilder.CreateUnreachable();
                throw std::logic_error("Warning: Missing return in non-void function '"
                                       + std::string(parentFunction->getName())
                                       + "'");
            }
        }

        mc.symTable.exitScope();
    }


    llvm::GlobalVariable *genGlobalDeclaration(const DeclarationNode *node,
                                               llvm::Type *type,
                                               llvm::Value *init,
                                               const std::unique_ptr<llvm::Module> &module,
                                               ModuleContext &mc) {
        llvm::Constant *constInit = nullptr;
        if (init) {
            constInit = llvm::dyn_cast<llvm::Constant>(init);
            if (!constInit) {
                throw std::logic_error("Global variable initializer must be constant: " + node->ident->name);
            }
        }

        auto *gVar = new llvm::GlobalVariable(*module,
                                              type,
                                              true,
                                              llvm::GlobalValue::InternalLinkage,
                                              constInit,
                                              node->ident->name);

        gVar->setAlignment(llvm::MaybeAlign(8));
        gVar->setDSOLocal(true);

        mc.gValues[node->ident->name] = gVar;
        return gVar;
    }

    llvm::AllocaInst *genLocalDeclaration(const DeclarationNode *node,
                                          llvm::Type *type,
                                          llvm::Value *init,
                                          const std::unique_ptr<llvm::IRBuilder<>> &builder,
                                          ModuleContext &mc) {
        llvm::IRBuilderBase::InsertPointGuard guard(*builder);
        auto *const entryBB = &builder->GetInsertBlock()->getParent()->getEntryBlock();
        builder->SetInsertPoint(entryBB, entryBB->getFirstInsertionPt());

        auto *alloca = builder->CreateAlloca(type, nullptr, node->ident->name);

        if (init) {
            auto *const casted = tryCastValue(builder, init, type);
            if (!casted) {
                throw std::logic_error("Type mismatch in initialization of: " + node->ident->name);
            }
            builder->CreateStore(casted, alloca);
        }

        if (mc.symTable.lookup(node->ident->name)) {
            throw std::logic_error("Redeclaration of variable: " + node->ident->name);
        }

        mc.symTable.insert(node->ident->name, alloca);
        return alloca;
    }

    void processFunctionParameters(llvm::Function *func,
                                   llvm::BasicBlock *basicBlock,
                                   const FunctionNode *node,
                                   const std::unique_ptr<llvm::IRBuilder<>> &builder,
                                   const std::unique_ptr<llvm::Module> &module,
                                   ModuleContext &mc) {
        builder->SetInsertPoint(basicBlock);

        for (auto &arg: func->args()) {
            auto *const paramType = generateType(node->proto->params[arg.getArgNo()].type,
                                                 module->getContext());
            auto *const alloca = builder->CreateAlloca(paramType,
                                                       nullptr,
                                                       arg.getName());

            builder->CreateStore(&arg, alloca);

            if (mc.symTable.lookup(std::string(arg.getName()))) {
                throw std::logic_error("Duplicate parameter name: " + std::string(arg.getName()));
            }
            mc.symTable.insert(std::string(arg.getName()), alloca);
        }
    }
} // namespace

LLVMCodegen::LLVMCodegen(const std::unique_ptr<llvm::IRBuilder<>> &builder,
                         const std::unique_ptr<llvm::Module> &module,
                         ModuleContext &mc):
    builder(builder),
    module(module),
    mc(mc) {}

void LLVMCodegen::visit(const IdentNode *node) {
    if (const auto gv = mc.gValues.find(node->name); gv != mc.gValues.end()) {
        value_ = builder->CreateLoad(gv->second->getValueType(),
                                     gv->second,
                                     node->name + ".global");
    } else {
        auto *const alloc = mc.symTable.lookup(node->name);
        if (alloc == nullptr) {
            throw std::runtime_error(std::format("Unknown variable name: {}", node->name));
        }
        value_ = builder->CreateLoad(alloc->getAllocatedType(), alloc, node->name);
    }
}

void LLVMCodegen::visit(const FunctionNode *const node) {
    auto *const func = llvm::dyn_cast<llvm::Function>(
            generate(node->proto.get(), builder, module, mc));

    if (!func) {
        throw std::logic_error("Function prototype generation failed for: " + node->proto->name);
    }

    generateBasicBlock(node->body->statements,
                       builder,
                       module,
                       mc,
                       [&](llvm::BasicBlock *basicBlock) {
                           processFunctionParameters(func, basicBlock, node, builder, module, mc);
                       },
                       func);

    std::string verifyError;
    llvm::raw_string_ostream os(verifyError);
    if (llvm::verifyFunction(*func, &os)) {
        throw std::logic_error("Function verification failed:\n" + os.str());
    }

    value_ = func;
}

void LLVMCodegen::visit(const NumberNode *node) {
    if (node->isFloat) {
        value_ = llvm::ConstantFP::get(llvm::Type::getDoubleTy(module->getContext()),
                                       llvm::APFloat(node->value));
    } else {
        value_ = llvm::ConstantInt::get(llvm::Type::getInt32Ty(module->getContext()),
                                        llvm::APInt(32, static_cast<uint64_t>(node->value),
                                                    true));
    }
}

void LLVMCodegen::visit(const StringNode *node) {
    auto *strConstant = llvm::ConstantDataArray::getString(module->getContext(), node->str);
    auto *var = new llvm::GlobalVariable(*module,
                                         strConstant->getType(),
                                         true,
                                         llvm::GlobalValue::ExternalLinkage,
                                         strConstant,
                                         "str");

    value_ = builder->CreateInBoundsGEP(strConstant->getType(),
                                        var,
                                        {builder->getInt32(0), builder->getInt32(0)});
}

void LLVMCodegen::visit(const BooleanNode *node) {
    value_ = llvm::ConstantInt::getBool(builder->getInt1Ty(), node->value);
}

void LLVMCodegen::visit(const BinOpNode *node) {
    auto *lhsValue = generate(node->lhs.get(),
                              builder,
                              module,
                              mc);
    auto *rhsValue = generate(node->rhs.get(),
                              builder,
                              module,
                              mc);
    if (lhsValue == nullptr || rhsValue == nullptr) {
        return;
    }
    if (lhsValue->getType()->isPointerTy() || rhsValue->getType()->isPointerTy()) {
        throw std::logic_error("Unsupported operation");
    }

    auto *resultType = getResultType(lhsValue->getType(), rhsValue->getType(), module->getContext());
    if (resultType == nullptr) {
        throw std::runtime_error("Type mismatch: " +
                                 typeToString(lhsValue->getType()) + " and " +
                                 typeToString(rhsValue->getType()));
    }

    lhsValue = tryCastValue(builder, lhsValue, resultType);
    rhsValue = tryCastValue(builder, rhsValue, resultType);

    switch (node->binOp) {
        case TokenType::Plus:
            value_ = createAdd(builder, lhsValue, rhsValue, resultType);
            return;
        case TokenType::Minus:
            value_ = createSub(builder, lhsValue, rhsValue, resultType);
            return;
        case TokenType::Star:
            value_ = createMul(builder, lhsValue, rhsValue, resultType);
            return;
        case TokenType::Slash:
            value_ = createDiv(builder, lhsValue, rhsValue, resultType);
            return;
        case TokenType::LeftAngleBracket:
        case TokenType::LeftAngleBracketEqual:
        case TokenType::RightAngleBracket:
        case TokenType::RightAngleBracketEqual:
        case TokenType::Equal:
        case TokenType::NotEqual:
            value_ = createCompare(builder, node->binOp, lhsValue, rhsValue);
        default:
            break;
    }
}

void LLVMCodegen::visit(const ProtoFunctionStatement *node) {
    std::vector<llvm::Type *> functionParams;
    functionParams.reserve(node->params.size());
    for (const auto &param: node->params) {
        functionParams.push_back(generateType(param.type, module->getContext()));
    }
    auto *const functionType = llvm::FunctionType::get(generateType(node->returnType, module->getContext()),
                                                       functionParams,
                                                       node->isVarArgs);
    auto *const function = llvm::Function::Create(functionType,
                                                  llvm::Function::ExternalLinkage,
                                                  node->name,
                                                  module.get());
    // function->addFnAttr(llvm::Attribute::NoUnwind);
    // function->addRetAttr(llvm::Attribute::ZExt);
    for (auto *it = function->arg_begin(); it != function->arg_end(); ++it) {
        const auto index = std::distance(function->arg_begin(), it);
        it->setName(node->params[index].ident->name);
    }
    value_ = function;
}

void LLVMCodegen::visit(const AssignmentNode *const node) {
    auto *const init = generate(node->rvalue.get(),
                                builder,
                                module,
                                mc);
    if (builder->GetInsertBlock() == nullptr) {} else {
        if (auto *const var = mc.symTable.lookup(node->name); var != nullptr) {
            builder->CreateStore(
                    tryCastValue(builder, init, var->getAllocatedType()),
                    var);
            mc.symTable.insert(node->name, var);
            value_ = var;
        } else if (const auto gVar = mc.gValues.find(node->name); gVar != mc.gValues.end()) {
            if (gVar->second->isConstant()) {
                throw std::logic_error("Variable: " + node->name + " is constant");
            }
            builder->CreateStore(
                    tryCastValue(builder, init, gVar->second->getValueType()),
                    gVar->second);
            value_ = gVar->second;
        } else {
            throw std::logic_error("Undefined variable: " + node->name);
        }
    }
}

void LLVMCodegen::visit(const FunctionCallNode *const node) {
    auto *calleeFunc = getModuleFunction(node->ident->name,
                                         builder,
                                         module,
                                         mc);
    if (calleeFunc == nullptr) {
        throw std::runtime_error(std::format("Undefined reference: '{}'", node->ident->name));
    }

    // If argument mismatch error.
    if (!calleeFunc->isVarArg() && calleeFunc->arg_size() != node->args.size()) {
        throw std::logic_error("Argument mismatch error");
    }

    std::vector<llvm::Value *> argsFunc;
    argsFunc.reserve(node->args.size());
    const auto *const funcType = calleeFunc->getFunctionType();
    for (size_t i = 0; i < node->args.size(); ++i) {
        auto *argValue = generate(node->args[i].get(),
                                  builder,
                                  module,
                                  mc);
        if (i < funcType->getNumParams()) {
            argValue = tryCastValue(builder, argValue, funcType->getParamType(i));
        }
        argsFunc.push_back(argValue);
    }

    value_ = builder->CreateCall(calleeFunc, argsFunc);
}

void LLVMCodegen::visit(const IfStatement *node) {
    auto *condValue = generate(node->ifBranch.cond.get(), builder, module, mc);
    if (condValue == nullptr) {
        return;
    }
    condValue = builder->CreateFCmpONE(condValue,
                                       llvm::ConstantFP::get(module->getContext(),
                                                             llvm::APFloat(0.0)),
                                       "if_cond");
    auto *const insertBlock = builder->GetInsertBlock();
    if (insertBlock == nullptr) {
        return;
    }
    auto *const function = insertBlock->getParent();
    auto *thenBasicBlock = llvm::BasicBlock::Create(module->getContext(), "thenBasicBlock", function);
    auto *elseBasicBlock = llvm::BasicBlock::Create(module->getContext(), "elseBasicBlock");
    auto *const finishBasicBlock = llvm::BasicBlock::Create(module->getContext(), "finishBasicBlock");

    // if condition
    builder->CreateCondBr(condValue, thenBasicBlock, elseBasicBlock);

    throw std::runtime_error("not implemented");

    // // then base block
    // generateBasicBlock(node->ifBranch.then->statements,
    //                    thenBasicBlock,
    //                    builder,
    //                    module,
    //                    mc);
    // if (thenValue == nullptr) {
    //     return;
    // }
    // builder->CreateBr(finishBasicBlock);
    // thenBasicBlock = builder->GetInsertBlock();
    //
    // // else base block
    // function->insert(function->end(), elseBasicBlock);
    // auto *const elseValue = node->elseBranch.has_value()
    //                             ? generateStatements(node->elseBranch.value()->statements,
    //                                                  elseBasicBlock,
    //                                                  builder,
    //                                                  module,
    //                                                  mc)
    //                             : nullptr;
    // builder->CreateBr(finishBasicBlock);
    // elseBasicBlock = builder->GetInsertBlock();
    //
    // // merge base block
    // function->insert(function->end(), finishBasicBlock);
    // builder->SetInsertPoint(finishBasicBlock);
    //
    // // phi node
    // auto *const phiNode =
    //         builder->CreatePHI(llvm::Type::getDoubleTy(module->getContext()), 2, "if_tmp");
    // phiNode->addIncoming(thenValue, thenBasicBlock);
    // phiNode->addIncoming(
    //         elseValue ? elseValue : llvm::ConstantFP::getNullValue(llvm::Type::getDoubleTy(module->getContext())),
    //         elseBasicBlock);
    // value_ = phiNode;
}

void LLVMCodegen::visit(const ForLoopNode *node) {
    assert(builder->GetInsertBlock());
    auto *const currFunction = builder->GetInsertBlock()->getParent();
    auto *const beforeLoopBB = builder->GetInsertBlock();
    auto *const loopBB = llvm::BasicBlock::Create(module->getContext(),
                                                  "for_loop",
                                                  currFunction);
    builder->CreateBr(loopBB);
    builder->SetInsertPoint(loopBB);

    const auto *const initVarAst = dynamic_cast<const AssignmentNode *>(node->init.get());
    if (initVarAst == nullptr) {
        return;
    }
    auto *const loopVarValue = builder->CreatePHI(llvm::Type::getDoubleTy(module->getContext()),
                                                  2,
                                                  initVarAst->name);
    auto *const OldVar = mc.symTable.lookup(initVarAst->name);
    // namedValues[initVarAst->name] = loopVarValue;

    auto *const initValue = generate(initVarAst->rvalue.get(),
                                     builder,
                                     module,
                                     mc);
    if (initValue == nullptr) {
        return;
    }
    throw std::runtime_error("not implemented");

    // loopVarValue->addIncoming(initValue, beforeLoopBB);
    // if (generateStatements(node->body->statements,
    //                        loopBB,
    //                        builder,
    //                        module,
    //                        mc) == nullptr) {
    //     return;
    // }
    //
    // llvm::Value *nextValue;
    // if (node->next) {
    //     nextValue = generate(node->next.get(),
    //                          builder,
    //                          module,
    //                          mc);
    //     if (nextValue == nullptr) {
    //         return;
    //     }
    // } else {
    //     nextValue = builder->CreateFAdd(loopVarValue,
    //                                     llvm::ConstantFP::get(module->getContext(), llvm::APFloat(1.0)),
    //                                     "next_var");
    // }
    //
    // auto *condExprValue = generate(node->conditional.get(),
    //                                builder,
    //                                module,
    //                                mc);
    // if (condExprValue == nullptr) {
    //     return;
    // }
    // condExprValue = builder->CreateFCmpONE(
    //         condExprValue, llvm::ConstantFP::get(module->getContext(), llvm::APFloat(0.0)),
    //         "loop_cond");
    //
    // auto *const loopEndBB = builder->GetInsertBlock();
    // loopVarValue->addIncoming(nextValue, loopEndBB);
    //
    // auto *const afterLoopBB = llvm::BasicBlock::Create(module->getContext(),
    //                                                    "after_loop",
    //                                                    currFunction);
    // builder->CreateCondBr(condExprValue, loopBB, afterLoopBB);
    // builder->SetInsertPoint(afterLoopBB);
    //
    // if (OldVar != nullptr) {
    //     mc.symTable.insert(initVarAst->name, OldVar);
    // } else {
    //     // namedValues.erase(initVarAst->name);
    // }
    // value_ = llvm::Constant::getNullValue(llvm::Type::getDoubleTy(module->getContext()));
}

void LLVMCodegen::visit(const UnaryOpNode *node) {
    if (node->operatorType == TokenType::IncrementOperator) {
        value_ = builder->CreateFAdd(generate(node->expr.get(),
                                              builder,
                                              module,
                                              mc),
                                     llvm::ConstantFP::get(module->getContext(), llvm::APFloat(1.0)), "increment");
    } else if (node->operatorType == TokenType::DecrementOperator) {
        value_ = builder->CreateFSub(
                generate(node->expr.get(),
                         builder,
                         module,
                         mc),
                llvm::ConstantFP::get(module->getContext(), llvm::APFloat(1.0)), "decrement");
    }
}

void LLVMCodegen::visit(const LoopCondNode *node) {
    throw std::runtime_error("not implemented");
}

void LLVMCodegen::visit(const BlockNode *node) {
    if (!builder->GetInsertBlock()) {
        throw std::logic_error("Block generation outside of function context");
    }

    auto *const parentFunc = builder->GetInsertBlock()->getParent();
    generateBasicBlock(node->statements,
                       builder,
                       module,
                       mc,
                       std::nullopt,
                       parentFunc);
}

void LLVMCodegen::visit(const DeclarationNode *node) {
    llvm::LLVMContext &ctx = module->getContext();

    auto *varType = generateType(node->type, ctx);
    if (!varType) {
        throw std::logic_error("Unknown type for variable: " + node->ident->name);
    }

    llvm::Value *initValue = nullptr;
    if (node->init.has_value()) {
        initValue = generate(node->init.value().get(), builder, module, mc);
        if (!initValue) {
            throw std::logic_error("Failed to generate initializer for: " + node->ident->name);
        }
    } else {
        if (varType->isAggregateType()) {
            initValue = llvm::ConstantAggregateZero::get(varType);
        } else {
            initValue = llvm::Constant::getNullValue(varType);
        }
    }

    if (builder->GetInsertBlock() == nullptr) {
        value_ = genGlobalDeclaration(node, varType, initValue, module, mc);
    } else {
        value_ = genLocalDeclaration(node, varType, initValue, builder, mc);
    }
}

void LLVMCodegen::visit(const ReturnNode *node) {
    if (node->expr != nullptr) {
        value_ = builder->CreateRet(generate(node->expr.get(), builder, module, mc));
    } else {
        value_ = builder->CreateRetVoid();
    }
}

llvm::Value *LLVMCodegen::value() const {
    return value_;
}
