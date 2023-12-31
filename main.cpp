#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>
#include <unordered_map>

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include "KaleidoscopeJIT.h"

namespace {
    std::unique_ptr<llvm::LLVMContext> llvmContext;
    std::unique_ptr<llvm::Module> llvmModule;
    std::unique_ptr<llvm::IRBuilder<>> llvmIRBuilder;
    std::unique_ptr<llvm::orc::KaleidoscopeJIT> llvmJit;
    std::unordered_map<std::string, llvm::Value *> namedValues;
    std::unique_ptr<llvm::FunctionPassManager> functionPassManager;
    std::unique_ptr<llvm::FunctionAnalysisManager> functionAnalysisManager;
    std::unique_ptr<llvm::ModuleAnalysisManager> moduleAnalysisManager;
    std::unique_ptr<llvm::PassInstrumentationCallbacks> passInstsCallbacks;
    std::unique_ptr<llvm::StandardInstrumentations> standardInsts;
    const llvm::ExitOnError ExitOnError;

    void initLlvmModules() {
        llvmContext = std::make_unique<llvm::LLVMContext>();
        llvmModule = std::make_unique<llvm::Module>("my cool jit", *llvmContext);
        llvmModule->setDataLayout(llvmJit->getDataLayout());

        llvmIRBuilder = std::make_unique<llvm::IRBuilder<>>(*llvmContext);

        functionPassManager = std::make_unique<llvm::FunctionPassManager>();
        functionAnalysisManager = std::make_unique<llvm::FunctionAnalysisManager>();
        moduleAnalysisManager = std::make_unique<llvm::ModuleAnalysisManager>();
        passInstsCallbacks = std::make_unique<llvm::PassInstrumentationCallbacks>();
        standardInsts = std::make_unique<llvm::StandardInstrumentations>(*llvmContext, /*DebugLogging*/ true);
        standardInsts->registerCallbacks(*passInstsCallbacks, moduleAnalysisManager.get());

        // Add transform passes.
        // Do simple "peephole" optimizations and bit-twiddling optzns.
        functionPassManager->addPass(llvm::InstCombinePass());
        // Reassociate expressions.
        functionPassManager->addPass(llvm::ReassociatePass());
        // Eliminate Common SubExpressions.
        functionPassManager->addPass(llvm::GVNPass());
        // Simplify the control flow graph (deleting unreachable blocks, etc).
        functionPassManager->addPass(llvm::SimplifyCFGPass());

        // Register analysis passes used in these transform passes.
        functionAnalysisManager->registerPass([&] { return llvm::AAManager(); });
        functionAnalysisManager->registerPass([&] { return llvm::AssumptionAnalysis(); });
        functionAnalysisManager->registerPass([&] { return llvm::DominatorTreeAnalysis(); });
        functionAnalysisManager->registerPass([&] { return llvm::LoopAnalysis(); });
        functionAnalysisManager->registerPass([&] { return llvm::MemoryDependenceAnalysis(); });
        functionAnalysisManager->registerPass([&] { return llvm::MemorySSAAnalysis(); });
        functionAnalysisManager->registerPass([&] { return llvm::OptimizationRemarkEmitterAnalysis(); });
        functionAnalysisManager->registerPass([&] {
            return llvm::OuterAnalysisManagerProxy<llvm::ModuleAnalysisManager, llvm::Function>(*moduleAnalysisManager);
        });
        functionAnalysisManager->registerPass(
                [&] { return llvm::PassInstrumentationAnalysis(passInstsCallbacks.get()); });
        functionAnalysisManager->registerPass([&] { return llvm::TargetIRAnalysis(); });
        functionAnalysisManager->registerPass([&] { return llvm::TargetLibraryAnalysis(); });
        moduleAnalysisManager->registerPass([&] { return llvm::ProfileSummaryAnalysis(); });
    }

    class BaseAstNode {
    public:
        virtual ~BaseAstNode() = default;

        [[nodiscard]] virtual llvm::Value *codegen() const = 0;

        [[nodiscard]] virtual std::string toString() const = 0;
    };

    class StatementAst : public BaseAstNode {
    public:
        ~StatementAst() override = default;
    };

    class ExprAst : public BaseAstNode {
    public:
        ~ExprAst() override = default;
    };

    class NumberAst final : public ExprAst {
    public:
        explicit NumberAst(const double v) :
                value(v) {
        }

        [[nodiscard]] std::string toString() const override {
            return "number=" + std::to_string(value);
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            assert(llvmContext != nullptr);
            return llvm::ConstantFP::get(*llvmContext, llvm::APFloat(value));
        }

        double value;
    };

    class VariableAccessAst final : public ExprAst {
    public:
        explicit VariableAccessAst(std::string name) : name(std::move(name)) {
        }

        [[nodiscard]] std::string toString() const override {
            return "var=" + name;
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            return namedValues[name];
        }

        const std::string name;
    };

    class BinOpAst final : public ExprAst {
    public:
        BinOpAst(const char binOp,
                 std::unique_ptr<ExprAst> lhs,
                 std::unique_ptr<ExprAst> rhs) :
                binOp(binOp),
                lhs(std::move(lhs)),
                rhs(std::move(rhs)) {
        }

        [[nodiscard]] std::string toString() const override {
            const bool isLhsBinOp = dynamic_cast<BinOpAst *>(lhs.get()) != nullptr;
            const bool isRhsBinOp = dynamic_cast<BinOpAst *>(rhs.get()) != nullptr;
            return std::string("op=").append(1, binOp).append(", lhs=")
                    .append(isLhsBinOp ? "(" : "")
                    .append(lhs->toString())
                    .append(isLhsBinOp ? ")" : "")
                    .append(", rhs=")
                    .append(isRhsBinOp ? "(" : "")
                    .append(rhs->toString()).append(isRhsBinOp ? ")" : "");
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            assert(llvmContext != nullptr);
            auto *lhsValue = lhs->codegen();
            auto *rhsValue = rhs->codegen();
            if (lhsValue == nullptr || rhsValue == nullptr) {
                return nullptr;
            }
            if (lhsValue->getType()->isPointerTy()) {
                lhsValue = llvmIRBuilder->CreateLoad(llvm::Type::getDoubleTy(*llvmContext), lhsValue);
            }
            if (rhsValue->getType()->isPointerTy()) {
                rhsValue = llvmIRBuilder->CreateLoad(llvm::Type::getDoubleTy(*llvmContext), rhsValue);
            }

            switch (binOp) {
                case '+':
                    return llvmIRBuilder->CreateFAdd(lhsValue, rhsValue, "add_tmp");
                case '-':
                    return llvmIRBuilder->CreateFSub(lhsValue, rhsValue, "sub_tmp");
                case '*':
                    return llvmIRBuilder->CreateFMul(lhsValue, rhsValue, "mul_tmp");
                case '/':
                    return llvmIRBuilder->CreateFDiv(lhsValue, rhsValue, "div_tmp");
                case '<':
                    lhsValue = llvmIRBuilder->CreateFCmpULT(lhsValue, rhsValue, "cmp_tmp");
                    // Convert bool 0/1 to double 0.0 or 1.0
                    return llvmIRBuilder->CreateUIToFP(lhsValue, llvm::Type::getDoubleTy(*llvmContext), "bool_tmp");
            }
            return nullptr;
        }

        const char binOp;
        const std::unique_ptr<ExprAst> lhs;
        const std::unique_ptr<ExprAst> rhs;
    };

    class VariableDefinitionAst final : public StatementAst {
    public:
        VariableDefinitionAst(std::string name,
                              std::unique_ptr<ExprAst> rvalue) :
                name(std::move(name)),
                rvalue(std::move(rvalue)) {
        }

        [[nodiscard]] std::string toString() const override {
            return "var definition name=" + name + ", rvalue=" + rvalue->toString();
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            assert(llvmContext != nullptr);
            if (llvmIRBuilder->GetInsertBlock() == nullptr) {
                auto *const variable = new llvm::GlobalVariable(
                        *llvmModule,
                        llvmIRBuilder->getDoubleTy(),
                        false,
                        llvm::GlobalValue::CommonLinkage,
                        nullptr,
                        name
                );
                variable->setInitializer(reinterpret_cast<llvm::ConstantFP *>(rvalue->codegen()));
                return variable;
            }
            auto *const variable = new llvm::AllocaInst(llvmIRBuilder->getDoubleTy(), 0, name,
                                                        llvmIRBuilder->GetInsertBlock());
            llvmIRBuilder->CreateStore(rvalue->codegen(), variable);
            return variable;
        }

        const std::string name;
        const std::unique_ptr<ExprAst> rvalue;
    };

    struct ProtoFunctionAst final : public StatementAst {
        ProtoFunctionAst(std::string name, std::vector<std::string> args) :
                name(std::move(name)),
                args(std::move(args)) {
        }

        [[nodiscard]] std::string toString() const override {
            return "proto func:" + name;
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            assert(llvmContext != nullptr);
            std::vector<llvm::Type *> functionParams(args.size(), llvm::Type::getDoubleTy(*llvmContext));
            auto *const functionType = llvm::FunctionType::get(llvm::Type::getDoubleTy(*llvmContext), functionParams,
                                                               false);
            auto *const function = llvm::Function::Create(functionType,
                                                          llvm::Function::ExternalLinkage,
                                                          name,
                                                          llvmModule.get());
            for (auto *it = function->arg_begin(); it != function->arg_end(); ++it) {
                const auto index = std::distance(function->arg_begin(), it);
                it->setName(args[index]);
            }
            return function;
        }

        std::string name;
        std::vector<std::string> args;
    };

    std::unordered_map<std::string, std::unique_ptr<ProtoFunctionAst>> functionProtos;

    llvm::Function *getFunction(const std::string &Name) {
        // First, see if the function has already been added to the current module.
        if (auto *const function = llvmModule->getFunction(Name)) {
            return function;
        }

        // If not, check whether we can codegen the declaration from some existing
        // prototype.
        if (auto iterator = functionProtos.find(Name); iterator != functionProtos.end()) {
            return reinterpret_cast<llvm::Function *>(iterator->second->codegen());
        }

        // If no existing prototype exists, return null.
        return nullptr;
    }

    llvm::Value *codegenExpressions(const std::list<std::unique_ptr<BaseAstNode>> &expressions) {
        for (auto it = expressions.begin(); it != expressions.end(); ++it) {
            auto *const ir = (*it)->codegen();
            if (auto *const var = dynamic_cast<VariableDefinitionAst *>(it->get()); var != nullptr) {
                namedValues[var->name] = ir;
            }
            if (*it == expressions.back() && ir != nullptr) {
                return ir;
            }
        }
        return nullptr;
    }

    struct FunctionAst final : public StatementAst {
        FunctionAst(std::unique_ptr<ProtoFunctionAst> proto,
                    std::list<std::unique_ptr<BaseAstNode>> body) :
                proto(std::move(proto)),
                body(std::move(body)) {
        }

        [[nodiscard]] std::string toString() const override {
            return proto->toString();
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            assert(llvmContext != nullptr);
            // Transfer ownership of the prototype to the functionProtos map, but keep a
            // reference to it for use below.
            const auto &p = *proto;
            functionProtos[p.name] = std::make_unique<ProtoFunctionAst>(proto->name,
                                                                        proto->args);
            auto *const function = getFunction(p.name);
            if (function == nullptr) {
                return nullptr;
            }

            // Create a new basic block to start insertion into.
            auto *const basicBlock = llvm::BasicBlock::Create(*llvmContext, "entry", function);
            llvmIRBuilder->SetInsertPoint(basicBlock);

            // Record the function arguments in the namedValues map.
            namedValues.clear();
            for (auto &arg: function->args()) {
                namedValues[std::string(arg.getName())] = &arg;
            }

            if (auto *const returnValue = ::codegenExpressions(body)) {
                llvmIRBuilder->CreateRet(returnValue);
                verifyFunction(*function);
                return function;
            }

            // Error reading body, remove function.
            function->eraseFromParent();
            return nullptr;
        }

        const std::unique_ptr<ProtoFunctionAst> proto;
        const std::list<std::unique_ptr<BaseAstNode>> body;
    };

    class CallFunctionExpr final : public ExprAst {
    public:
        CallFunctionExpr(std::string callee, std::vector<std::unique_ptr<ExprAst>> args) :
                callee(std::move(callee)),
                args(std::move(args)) {
        }

        [[nodiscard]] std::string toString() const override {
            std::stringstream ss;
            ss << "call func: " << callee << "(";
            for (const auto &arg: args) {
                const bool isBinOp = dynamic_cast<BinOpAst *>(arg.get()) != nullptr;
                if (isBinOp) {
                    ss << "(";
                }
                ss << arg->toString() << ",";
                if (isBinOp) {
                    ss << ")";
                }
            }
            ss << ")";
            return ss.str();
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            assert(llvmContext != nullptr);
            // Look up the name in the global module table.
            auto *calleeFunc = getFunction(callee);
            if (calleeFunc == nullptr) {
                return nullptr;
            }

            // If argument mismatch error.
            if (calleeFunc->arg_size() != args.size()) {
                return nullptr;
            }

            std::vector<llvm::Value *> argsFunc;
            for (const auto &arg: args) {
                argsFunc.push_back(arg->codegen());
                if (!argsFunc.back()) {
                    return nullptr;
                }
            }

            return llvmIRBuilder->CreateCall(calleeFunc, argsFunc, "calltmp");
        }

        const std::string callee;
        const std::vector<std::unique_ptr<ExprAst>> args;
    };

    class IfStatement final : public StatementAst {
    public:
        IfStatement(std::unique_ptr<ExprAst> cond,
                    std::list<std::unique_ptr<BaseAstNode>> thenBranch,
                    std::optional<std::list<std::unique_ptr<BaseAstNode>>> elseBranch) :
                cond(std::move(cond)),
                thenBranch(std::move(thenBranch)),
                elseBranch(std::move(elseBranch)) {
        }

        [[nodiscard]] std::string toString() const override {
            return "if expr";
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            auto *condValue = cond->codegen();
            if (condValue == nullptr) {
                return nullptr;
            }
            condValue = llvmIRBuilder->CreateFCmpONE(condValue,
                                                     llvm::ConstantFP::get(*llvmContext,
                                                                           llvm::APFloat(0.0)),
                                                     "if_cond");
            auto *const insertBlock = llvmIRBuilder->GetInsertBlock();
            if (insertBlock == nullptr) {
                return nullptr;
            }
            auto *const function = insertBlock->getParent();
            auto *thenBasicBlock = llvm::BasicBlock::Create(*llvmContext, "thenBasicBlock", function);
            auto *elseBasicBlock = llvm::BasicBlock::Create(*llvmContext, "elseBasicBlock");
            auto *const finishBasicBlock = llvm::BasicBlock::Create(*llvmContext, "finishBasicBlock");

            // if condition
            llvmIRBuilder->CreateCondBr(condValue, thenBasicBlock, elseBasicBlock);

            // then base block
            llvmIRBuilder->SetInsertPoint(thenBasicBlock);
            auto *const thenValue = codegenExpressions(thenBranch);
            if (thenValue == nullptr) {
                return nullptr;
            }
            llvmIRBuilder->CreateBr(finishBasicBlock);
            thenBasicBlock = llvmIRBuilder->GetInsertBlock();

            // else base block
            function->insert(function->end(), elseBasicBlock);
            llvmIRBuilder->SetInsertPoint(elseBasicBlock);
            auto *const elseValue = elseBranch.has_value() ? codegenExpressions(elseBranch.value()) : nullptr;
            llvmIRBuilder->CreateBr(finishBasicBlock);
            elseBasicBlock = llvmIRBuilder->GetInsertBlock();

            // merge base block
            function->insert(function->end(), finishBasicBlock);
            llvmIRBuilder->SetInsertPoint(finishBasicBlock);

            // phi node
            auto *const phiNode =
                    llvmIRBuilder->CreatePHI(llvm::Type::getDoubleTy(*llvmContext), 2, "if_tmp");
            phiNode->addIncoming(thenValue, thenBasicBlock);
            phiNode->addIncoming(
                    elseValue ? elseValue : llvm::ConstantFP::getNullValue(llvm::Type::getDoubleTy(*llvmContext)),
                    elseBasicBlock);
            return phiNode;
        }

        const std::unique_ptr<ExprAst> cond;
        const std::list<std::unique_ptr<BaseAstNode>> thenBranch;
        const std::optional<std::list<std::unique_ptr<BaseAstNode>>> elseBranch;
    };

    class ForLoopStatement final : public StatementAst {
    public:
        ForLoopStatement(std::unique_ptr<StatementAst> initExpr,
                         std::unique_ptr<ExprAst> nextExpr,
                         std::unique_ptr<ExprAst> endExpr,
                         std::list<std::unique_ptr<BaseAstNode>> body) :
                init(std::move(initExpr)),
                next(std::move(nextExpr)),
                conditional(std::move(endExpr)),
                body(std::move(body)) {
        }

        [[nodiscard]] std::string toString() const override {
            return "for loop";
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            assert(llvmIRBuilder->GetInsertBlock());
            auto *const currFunction = llvmIRBuilder->GetInsertBlock()->getParent();
            auto *const beforeLoopBB = llvmIRBuilder->GetInsertBlock();
            auto *const loopBB = llvm::BasicBlock::Create(*llvmContext,
                                                          "for_loop",
                                                          currFunction);
            llvmIRBuilder->CreateBr(loopBB);
            llvmIRBuilder->SetInsertPoint(loopBB);

            const auto *const initVarAst = dynamic_cast<const VariableDefinitionAst *>(init.get());
            if (initVarAst == nullptr) {
                return nullptr;
            }
            auto *const loopVarValue = llvmIRBuilder->CreatePHI(llvm::Type::getDoubleTy(*llvmContext),
                                                                2,
                                                                initVarAst->name);
            auto *const OldVar = namedValues[initVarAst->name];
            namedValues[initVarAst->name] = loopVarValue;
            auto *const initValue = initVarAst->rvalue->codegen();
            if (initValue == nullptr) {
                return nullptr;
            }
            loopVarValue->addIncoming(initValue, beforeLoopBB);
            if (codegenExpressions(body) == nullptr) {
                return nullptr;
            }

            llvm::Value *nextValue;
            if (next) {
                nextValue = next->codegen();
                if (nextValue == nullptr) {
                    return nullptr;
                }
            } else {
                nextValue = llvmIRBuilder->CreateFAdd(loopVarValue,
                                                      llvm::ConstantFP::get(*llvmContext, llvm::APFloat(1.0)),
                                                      "next_var");
            }

            auto *condExprValue = conditional->codegen();
            if (condExprValue == nullptr) {
                return nullptr;
            }
            condExprValue = llvmIRBuilder->CreateFCmpONE(
                    condExprValue, llvm::ConstantFP::get(*llvmContext, llvm::APFloat(0.0)),
                    "loop_cond");

            auto *const loopEndBB = llvmIRBuilder->GetInsertBlock();
            loopVarValue->addIncoming(nextValue, loopEndBB);

            auto *const afterLoopBB = llvm::BasicBlock::Create(*llvmContext,
                                                               "after_loop",
                                                               currFunction);
            llvmIRBuilder->CreateCondBr(condExprValue, loopBB, afterLoopBB);
            llvmIRBuilder->SetInsertPoint(afterLoopBB);

            if (OldVar != nullptr) {
                namedValues[initVarAst->name] = OldVar;
            } else {
                namedValues.erase(initVarAst->name);
            }
            return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*llvmContext));
        }

        const std::unique_ptr<BaseAstNode> init;
        const std::unique_ptr<ExprAst> next;
        const std::unique_ptr<ExprAst> conditional;
        const std::list<std::unique_ptr<BaseAstNode>> body;
    };

    enum class TokenType : std::uint8_t {
        EosToken,
        NumberToken,
        FunctionDefinitionToken,
        IdentifierToken,
        IfToken,
        ElseToken,
        ForLoopToken,
        IncrementOperatorToken,
        DecrementOperatorToken,
        OtherToken,
    };

    std::unique_ptr<std::istream> stream;
    int lastChar = ' ';
    TokenType currentToken;
    std::string numberValue;
    std::string identifier;

    std::unique_ptr<BaseAstNode> parseAstNodeItem();

    void readNextChar() {
        do {
            lastChar = stream->get();
            if (lastChar == '\n' || !stream->eof()) {
                break;
            }
        } while (*stream);
    }

    int getPeekChar() {
        return stream->peek();
    }

    bool isSignOfNumber(const int ch) {
        return ch == '+' || ch == '-';
    }

    bool isCharOfNumber(const int ch) {
        return isdigit(ch) || ch == '.';
    }

    void parseNumber() {
        numberValue.clear();
        do {
            if (isspace(lastChar)) {
                if (ispunct(getPeekChar())) {
                    break;
                }
                readNextChar();
                continue;
            }

            if ((isSignOfNumber(lastChar) && numberValue.empty()) || isCharOfNumber(lastChar)) {
                numberValue.push_back(static_cast<char>(lastChar));
                // last symbol of number
                if (ispunct(getPeekChar()) && getPeekChar() != '.') {
                    break;
                }
                readNextChar();
            } else {
                break;
            }
        } while (*stream);
    }

    void readNextToken(const bool inExpression = false) {
        do {
            readNextChar();
        } while (isspace(lastChar));

        if (lastChar == ';' && !inExpression) {
            do {
                readNextChar();
            } while (isspace(lastChar) && lastChar != '\n');
        }

        if (lastChar == EOF) {
            currentToken = TokenType::EosToken;
            return;
        }

        currentToken = TokenType::OtherToken;
        // parse number
        if ((isSignOfNumber(lastChar) && !inExpression) || isCharOfNumber(lastChar)) {
            currentToken = TokenType::NumberToken;
            parseNumber();
        } else if (isSignOfNumber(lastChar)) {
            const int peek = getPeekChar();
            if (peek == lastChar) {
                while (!isalnum(getPeekChar())) {
                    readNextChar();
                }
                if (lastChar == '+') {
                    currentToken = TokenType::IncrementOperatorToken;
                } else {
                    currentToken = TokenType::DecrementOperatorToken;
                }
            }
        } else {
            // parse identifiers
            if (isalpha(lastChar)) {
                identifier.clear();
                while (isalnum(lastChar)) {
                    identifier.push_back(lastChar);
                    const char p = getPeekChar();
                    if (!isalnum(p)) {
                        break;
                    }
                    readNextChar();
                }
                if (identifier == "def") {
                    currentToken = TokenType::FunctionDefinitionToken;
                } else if (identifier == "if") {
                    currentToken = TokenType::IfToken;
                } else if (identifier == "else") {
                    currentToken = TokenType::ElseToken;
                } else if (identifier == "for") {
                    currentToken = TokenType::ForLoopToken;
                } else {
                    currentToken = TokenType::IdentifierToken;
                }
            }
        }
    }

    std::tuple<std::unique_ptr<ExprAst>, std::unique_ptr<BaseAstNode>> toExpr(std::unique_ptr<BaseAstNode> node) {
        if (dynamic_cast<ExprAst *>(node.get()) != nullptr) {
            return {std::unique_ptr<ExprAst>(dynamic_cast<ExprAst *>(node.release())), nullptr};
        }
        return {nullptr, std::move(node)};
    }

    std::tuple<std::unique_ptr<StatementAst>, std::unique_ptr<BaseAstNode>>
    toStatement(std::unique_ptr<BaseAstNode> node) {
        if (dynamic_cast<StatementAst *>(node.get()) != nullptr) {
            return {std::unique_ptr<StatementAst>(dynamic_cast<StatementAst *>(node.release())), nullptr};
        }
        return {nullptr, std::move(node)};
    }

    std::unique_ptr<ExprAst> parseNumberExpr(const bool inExpression = false) {
        auto number = std::make_unique<NumberAst>(strtod(numberValue.c_str(), nullptr));
        readNextToken(inExpression);
        return number;
    }

    std::unique_ptr<ExprAst> parseParentheses() {
        if (lastChar != '(') {
            return nullptr;
        }
        readNextToken(); // eat (
        auto expr = parseAstNodeItem();
        if (lastChar != ')') {
            return nullptr;
        }
        readNextToken(); // eat )
        return std::get<0>(toExpr(std::move(expr)));
    }

    std::unique_ptr<BaseAstNode> parseExpr(bool inExpression = false);

    std::unique_ptr<BaseAstNode> parseIdentifier(const bool inExpression = false) {
        const std::string name = identifier;
        readNextToken(inExpression); // eat identifier
        if (lastChar == '=') {
            readNextToken(); // eat =
            auto expr = parseAstNodeItem();
            return std::make_unique<VariableDefinitionAst>(name, std::get<0>(toExpr(std::move(expr))));
        }
        if (lastChar != '(') {
            return std::make_unique<VariableAccessAst>(name);
        }

        std::vector<std::unique_ptr<ExprAst>> args;
        readNextToken(); // eat '('
        while (true) {
            if (auto arg = parseAstNodeItem()) {
                args.push_back(std::get<0>(toExpr(std::move(arg))));
                if (lastChar == ',') {
                    readNextToken(); // eat ','
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        if (lastChar != ')') {
            return nullptr;
        }
        readNextToken(); // eat ')'
        return std::make_unique<CallFunctionExpr>(name, std::move(args));
    }

    std::list<std::unique_ptr<BaseAstNode>> parseCurlyBrackets() {
        std::list<std::unique_ptr<BaseAstNode>> expressions;
        while (auto node = parseAstNodeItem()) {
            expressions.push_back(std::move(node));
            if (lastChar == '}') {
                break;
            }
            readNextToken();
        }
        return expressions;
    }

    std::unique_ptr<StatementAst> parseIfExpression() {
        readNextToken();
        if (lastChar != '(') {
            return nullptr;
        }
        auto cond = parseParentheses();
        if (lastChar != '{') {
            return nullptr;
        }
        readNextToken();
        std::list<std::unique_ptr<BaseAstNode>> thenBranch = parseCurlyBrackets();
        readNextToken();
        std::optional<std::list<std::unique_ptr<BaseAstNode>>> elseBranch;
        if (currentToken == TokenType::ElseToken) {
            readNextToken();
            if (lastChar != '{') {
                return nullptr;
            }
            readNextToken();
            elseBranch = parseCurlyBrackets();
        }
        return std::make_unique<IfStatement>(std::move(cond), std::move(thenBranch), std::move(elseBranch));
    }

    std::unique_ptr<StatementAst> parseForLoopExpression() {
        readNextToken();
        if (lastChar != '(') {
            return nullptr;
        }
        readNextToken();
        auto loopInit = parseIdentifier();
        if (loopInit == nullptr) {
            return nullptr;
        }
        readNextToken(true);
        auto loopFinish = parseAstNodeItem();
        if (loopFinish == nullptr) {
            return nullptr;
        }
        readNextToken(true);
        auto loopNext = parseAstNodeItem();
        if (loopNext == nullptr) {
            return nullptr;
        }
        readNextToken();
        if (lastChar != '{') {
            return nullptr;
        }
        readNextToken();
        auto loopBody = parseCurlyBrackets();

        auto forLoopExpr = std::make_unique<ForLoopStatement>(std::get<0>(toStatement(std::move(loopInit))),
                                                              std::get<0>(toExpr(std::move(loopNext))),
                                                              std::get<0>(toExpr(std::move(loopFinish))),
                                                              std::move(loopBody));
        return forLoopExpr;
    }

    class UnaryOpAst final : public ExprAst {
    public:
        UnaryOpAst(const TokenType operatorType, std::unique_ptr<ExprAst> expr) :
                operatorType(operatorType),
                expr(std::move(expr)) {
        }

        [[nodiscard]] std::string toString() const override {
            return "unary operator";
        }

        [[nodiscard]] llvm::Value *codegen() const override {
            if (operatorType == TokenType::IncrementOperatorToken) {
                return llvmIRBuilder->CreateFAdd(expr->codegen(),
                                                 llvm::ConstantFP::get(*llvmContext, llvm::APFloat(1.0)),
                                                 "increment");
            }
            return nullptr;
        }

        const TokenType operatorType;
        const std::unique_ptr<ExprAst> expr;
    };

    std::unique_ptr<ExprAst> parseUnaryExpression() {
        const auto operatorType = currentToken;
        readNextToken(true);
        auto expr = parseExpr(true);
        return std::make_unique<UnaryOpAst>(operatorType, std::get<0>(toExpr(std::move(expr))));
    }

    std::unique_ptr<StatementAst> parseStatement() {
        if (currentToken == TokenType::IfToken) {
            return parseIfExpression();
        }
        if (currentToken == TokenType::ForLoopToken) {
            return parseForLoopExpression();
        }
        return nullptr;
    }

    std::unique_ptr<BaseAstNode> parseExpr(const bool inExpression) {
        if (currentToken == TokenType::NumberToken) {
            return parseNumberExpr(inExpression);
        }
        if (currentToken == TokenType::IdentifierToken) {
            return parseIdentifier(inExpression);
        }
        if (currentToken == TokenType::IncrementOperatorToken
            || currentToken == TokenType::DecrementOperatorToken) {
            return parseUnaryExpression();
        }
        if (lastChar == '(') {
            return parseParentheses();
        }
        return nullptr;
    }

    int getBinOpPrecedence(const char binOp) {
        int binOpPrec = -1;
        if (binOp == '+' || binOp == '-') {
            binOpPrec = 1;
        } else if (binOp == '/' || binOp == '*') {
            binOpPrec = 2;
        } else if (binOp == '<' || binOp == '>') {
            binOpPrec = 0;
        }
        return binOpPrec;
    }

    std::unique_ptr<ExprAst> parseBinOp(const int expPrec,
                                        std::unique_ptr<ExprAst> lhs) {
        while (true) {
            const char binOp = static_cast<char>(lastChar);
            const int curBinOpPrec = getBinOpPrecedence(binOp);
            if (curBinOpPrec < expPrec) {
                return lhs;
            }

            readNextToken(true); // read rhs
            auto rhs = parseExpr(true);
            if (rhs == nullptr) {
                return nullptr;
            }

            const char nextBinOp = static_cast<char>(lastChar);
            if (const int nextBinOpPrec = getBinOpPrecedence(nextBinOp); curBinOpPrec < nextBinOpPrec) {
                if (rhs = parseBinOp(curBinOpPrec, std::get<0>(toExpr(std::move(rhs)))); rhs == nullptr) {
                    return nullptr;
                }
            }

            lhs = std::make_unique<BinOpAst>(binOp, std::move(lhs), std::get<0>(toExpr(std::move(rhs))));
        }
    }

    std::unique_ptr<BaseAstNode> parseAstNodeItem() {
        if (auto node = parseExpr(true)) {
            auto [expr, srcNode] = toExpr(std::move(node));
            if (expr) {
                return parseBinOp(0, std::move(expr));
            }
            return std::move(srcNode);
        }
        if (auto statement = parseStatement()) {
            return statement;
        }
        return nullptr;
    }

    void print(const llvm::Value *const llvmIR) {
        llvm::outs() << "IR: ";
        llvmIR->print(llvm::outs(), true);
        llvm::outs() << '\n';
    }

    void print(const BaseAstNode *const nodeAst) {
        if (auto const *const llvmIR = nodeAst->codegen()) {
            print(llvmIR);
        }
        std::list<BinOpAst *> values;
        const auto *ptr = dynamic_cast<const BinOpAst *>(nodeAst);
        do {
            if (!values.empty()) {
                ptr = values.front();
                values.pop_front();
            }
            if (ptr == nullptr) {
                continue;
            }
            if (auto *const rhs = dynamic_cast<BinOpAst *>(ptr->rhs.get())) {
                values.push_back(rhs);
            }
            if (auto *const lhs = dynamic_cast<BinOpAst *>(ptr->lhs.get())) {
                values.push_back(lhs);
            }
            std::cout << ">" << ptr->toString() << "\n";
        } while (!values.empty());
    }

    std::unique_ptr<ProtoFunctionAst> parseProto() {
        const std::string name = identifier;
        readNextToken(); // eat callee
        if (lastChar != '(') {
            return nullptr;
        }
        readNextToken(); // eat (
        std::vector<std::string> args;
        while (*stream) {
            if (currentToken != TokenType::IdentifierToken) {
                break;
            }
            if (auto arg = parseIdentifier()) {
                const auto *const var = dynamic_cast<const VariableAccessAst *>(arg.get());
                args.push_back(var->name);
                if (lastChar == ',') {
                    readNextToken(); // eat next arg
                }
            } else {
                break;
            }
        }
        if (lastChar != ')') {
            return nullptr;
        }
        readNextToken(); // eat )
        return std::make_unique<ProtoFunctionAst>(name, args);
    }

    std::unique_ptr<FunctionAst> parseFunctionDefinition() {
        readNextToken(); // eat def
        auto proto = parseProto();
        if (lastChar != '{') {
            return nullptr;
        }
        readNextToken();
        std::list<std::unique_ptr<BaseAstNode>> body = parseCurlyBrackets();
        return std::make_unique<FunctionAst>(std::move(proto), std::move(body));
    }

    std::unique_ptr<FunctionAst> parseTopLevelExpr(const char *const functionName) {
        std::list<std::unique_ptr<BaseAstNode>> body;
        while (auto expr = parseAstNodeItem()) {
            if (expr == nullptr) {
                break;
            }
            body.push_back(std::move(expr));
            readNextToken();
        }
        auto proto = std::make_unique<ProtoFunctionAst>(functionName,
                                                        std::vector<std::string>());
        return std::make_unique<FunctionAst>(std::move(proto), std::move(body));
    }

    double print(const double param) {
        printf("print: %f\n", param);
        return param;
    }

    void mainHandler() {
        readNextToken();
        do {
            if (currentToken == TokenType::FunctionDefinitionToken) {
                auto definition = parseFunctionDefinition();
                if (definition != nullptr) {
                    print(definition.get());
                }
                ExitOnError(llvmJit->addModule(
                        llvm::orc::ThreadSafeModule(std::move(llvmModule), std::move(llvmContext)), nullptr));
                initLlvmModules();
                readNextToken();
            } else {
                if (const auto function = parseTopLevelExpr("_start")) {
                    const auto *const llvmIR = function->codegen();
                    if (llvmIR != nullptr) {
                        print(llvmIR);
                        auto resourceTracker = llvmJit->getMainJITDylib().createResourceTracker();
                        auto threadSafeModule = llvm::orc::ThreadSafeModule(std::move(llvmModule),
                                                                            std::move(llvmContext));
                        ExitOnError(llvmJit->addModule(std::move(threadSafeModule), resourceTracker));
                        initLlvmModules();
                        const auto startSymbol = ExitOnError(llvmJit->lookup("_start"));
                        using FuncType = double (*)();
                        auto *const startFunc = startSymbol.getAddress().toPtr<FuncType>();
                        std::cout << "result=" << startFunc() << "\n";
                        ExitOnError(resourceTracker->remove());
                    }
                }
                readNextToken();
            }
        } while (currentToken != TokenType::EosToken);
    }

    void defineEmbeddedFunctions() {
        llvm::orc::MangleAndInterner mangle(llvmJit->getMainJITDylib().getExecutionSession(),
                                            llvmJit->getDataLayout());
        llvm::orc::SymbolMap symbols;

        constexpr const char *const name = "print";
        auto printProto = std::make_unique<ProtoFunctionAst>(name, std::vector<std::string>{"param"});
        functionProtos[name] = std::move(printProto);
        symbols[mangle(name)] = {
                llvm::orc::ExecutorAddr::fromPtr<double(double)>(&print),
                llvm::JITSymbolFlags()
        };

        ExitOnError(llvmJit->getMainJITDylib().define(absoluteSymbols(symbols)));
    }

    void testParseBinExpression();

    void testParseNumber();

    void testFunctionDefinition();

    void testIdentifier();

    void testVarDefinition();

    void testIfExpression();
} // namespace

int main() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    llvmJit = ExitOnError(llvm::orc::KaleidoscopeJIT::Create());

    initLlvmModules();

    testParseBinExpression();
    testParseNumber();
    testFunctionDefinition();
    testIdentifier();
    testVarDefinition();
    testIfExpression();

    defineEmbeddedFunctions();

    stream = std::make_unique<std::istringstream>(R"(
    for (i=0; i < 10; ++i) {
        print(i);
    }
    )");
    //    stream->basic_ios::rdbuf(std::cin.rdbuf());
    mainHandler();
    return 0;
}

namespace {
    inline std::string makeTestFailMsg(const std::uint32_t line) {
        return std::string("test failed, line=").append(std::to_string(line));
    }

    void testVarDefinition() {
        stream = std::make_unique<std::istringstream>("varName=2*(1-2);");
        readNextToken();
        const auto varExprAst = parseIdentifier();
        if (varExprAst == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        const auto *const var = dynamic_cast<VariableDefinitionAst *>(varExprAst.get());
        if (var->name != "varName") {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        print(varExprAst.get());
        const auto *const binOp = dynamic_cast<BinOpAst *>(var->rvalue.get());
        if (binOp == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
    }

    void testFunctionDefinition() {
        stream = std::make_unique<std::istringstream>("def test(id1, id2, id3) {varPtr=(1+2+id1) * (2+1+id2);}");
        readNextToken();
        if (currentToken != TokenType::FunctionDefinitionToken) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        const auto func = parseFunctionDefinition();
        if (func == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        print(func.get());
        const auto *const funcPtr = dynamic_cast<FunctionAst *>(func.get());
        if (func == nullptr || funcPtr->proto->name != "test" || funcPtr->proto->args.size() != 3) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        const auto *const varPtr = dynamic_cast<VariableDefinitionAst *>(funcPtr->body.front().get());
        if (varPtr == nullptr || varPtr->name != "varPtr" || varPtr->rvalue == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        functionProtos.clear();
        namedValues.clear();
        ExitOnError(llvmJit->addModule(
                llvm::orc::ThreadSafeModule(std::move(llvmModule), std::move(llvmContext)), nullptr));
        initLlvmModules();
    }

    void testParseNumber() {
        {
            stream = std::make_unique<std::istringstream>(" -123.123;");
            readNextToken();
            const auto expr = parseExpr();
            if (expr == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const numberAst = dynamic_cast<const NumberAst *>(expr.get());
            if (numberAst->value != -123.123) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            print(expr.get());
        }
    }

    void testParseBinExpression() {
        {
            stream = std::make_unique<std::istringstream>("-1-21.2;");
            readNextToken();
            if (currentToken != TokenType::NumberToken) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto expr = parseAstNodeItem();
            if (expr == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const binOp = dynamic_cast<BinOpAst *>(expr.get());
            if (binOp == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            if (binOp->binOp != '-') {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const lhsNumber = dynamic_cast<NumberAst *>(binOp->lhs.get());
            if (lhsNumber == nullptr || lhsNumber->value != -1) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const rhsNumber = dynamic_cast<NumberAst *>(binOp->rhs.get());
            if (rhsNumber == nullptr || rhsNumber->value != 21.2) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            print(expr.get());
        }
        {
            stream = std::make_unique<std::istringstream>("(2*(1+2));");
            readNextToken();
            const auto expr = parseAstNodeItem();
            if (expr == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const binOp = dynamic_cast<BinOpAst *>(expr.get());
            {
                const auto *const lhsNumber = dynamic_cast<NumberAst *>(binOp->lhs.get());
                if (lhsNumber == nullptr || lhsNumber->value != 2) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
            }
            {
                const auto *const binOpRhs = dynamic_cast<BinOpAst *>(binOp->rhs.get());
                if (binOpRhs == nullptr) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
                const auto *const lhs = dynamic_cast<NumberAst *>(binOpRhs->lhs.get());
                if (lhs == nullptr || lhs->value != 1) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
                const auto *const rhs = dynamic_cast<NumberAst *>(binOpRhs->rhs.get());
                if (rhs == nullptr || rhs->value != 2) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
            }
            print(expr.get());
        }
        {
            stream = std::make_unique<std::istringstream>("+1 *  (   2    +3.0);");
            readNextToken();
            const auto expr = parseAstNodeItem();
            if (expr == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const binOp = dynamic_cast<BinOpAst *>(expr.get());
            {
                const auto *const lhsNumber = dynamic_cast<NumberAst *>(binOp->lhs.get());
                if (lhsNumber == nullptr || lhsNumber->value != 1) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
            }
            {
                const auto *const binOpRhs = dynamic_cast<BinOpAst *>(binOp->rhs.get());
                if (binOpRhs == nullptr) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
                const auto *const lhs = dynamic_cast<NumberAst *>(binOpRhs->lhs.get());
                if (lhs == nullptr || lhs->value != 2) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
                const auto *const rhs = dynamic_cast<NumberAst *>(binOpRhs->rhs.get());
                if (rhs == nullptr || rhs->value != 3.0) {
                    throw std::logic_error(makeTestFailMsg(__LINE__));
                }
            }
            print(expr.get());
        }
    }

    void testIdentifier() {
        {
            stream = std::make_unique<std::istringstream>("v+1;");
            readNextToken();
            const auto expr = parseAstNodeItem();
            if (expr == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const binOp = dynamic_cast<BinOpAst *>(expr.get());
            if (binOp == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const lhs = dynamic_cast<VariableAccessAst *>(binOp->lhs.get());
            if (lhs == nullptr || lhs->name != "v") {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const rhs = dynamic_cast<NumberAst *>(binOp->rhs.get());
            if (rhs == nullptr || rhs->value != 1.0) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
        }
        {
            stream = std::make_unique<std::istringstream>("foo(1, 12.1, id1, -1.2, (1+2));");
            readNextToken();
            const auto expr = parseExpr(true);
            if (expr == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            print(expr.get());
            const auto *const callFunc = dynamic_cast<CallFunctionExpr *>(expr.get());
            if (callFunc->callee != "foo") {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            if (callFunc->args.size() != 5) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            if (auto *const number = dynamic_cast<NumberAst *>(callFunc->args[0].get()); number->value != 1) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            if (auto *const number = dynamic_cast<NumberAst *>(callFunc->args[1].get()); number->value != 12.1) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            if (auto *const var = dynamic_cast<VariableAccessAst *>(callFunc->args[2].get()); var->name != "id1") {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            if (auto *const number = dynamic_cast<NumberAst *>(callFunc->args[3].get()); number->value != -1.2) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            if (auto *const binOp = dynamic_cast<BinOpAst *>(callFunc->args[4].get()); binOp == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
        }
    }

    void testIfExpression() {
        stream = std::make_unique<std::istringstream>(R"(
            if (1) {
                print(1);
            } else {
                print(0);
            }
        )");
        readNextToken();
        const auto ifStatement = parseAstNodeItem();
        if (ifStatement == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        const auto *const ifExprPtr = dynamic_cast<IfStatement *>(ifStatement.get());
        if (ifExprPtr == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        if (ifExprPtr->cond == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        const auto *const numberAstPtr = dynamic_cast<NumberAst *>(ifExprPtr->cond.get());
        if (numberAstPtr == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }

        if (ifExprPtr->thenBranch.empty()) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }
        const auto *const thenFuncAstPtr = dynamic_cast<CallFunctionExpr *>(ifExprPtr->thenBranch.back().get());
        if (thenFuncAstPtr == nullptr) {
            throw std::logic_error(makeTestFailMsg(__LINE__));
        }

        if (ifExprPtr->elseBranch) {
            if (ifExprPtr->elseBranch.value().empty()) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
            const auto *const elseFuncAstPtr = dynamic_cast<CallFunctionExpr *>(
                    ifExprPtr->elseBranch.value().back().get());
            if (elseFuncAstPtr == nullptr) {
                throw std::logic_error(makeTestFailMsg(__LINE__));
            }
        }
    }
} // namespace
