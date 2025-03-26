//
// Created by vadim on 26.03.25.
//

#include "NumericIRType.h"

NumericIRType::NumericIRType(const bool isPointer,
                             const bool isSigned,
                             const bool isFloat):
    IRType(isPointer),
    isSigned(isSigned),
    isFloat(isFloat) {}

llvm::Type *NumericIRType::getLLVMType(llvm::LLVMContext &context) const {
    auto *const baseType = getBaseLLVMType(context);
    return isPointer ? baseType->getPointerTo() : baseType;
}

bool NumericIRType::isOperationSupported(const TokenType op, const IRType *rhs) const {
    if (dynamic_cast<const NumericIRType *>(rhs)) {
        switch (op) {
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Equal:
            case TokenType::NotEqual:
            case TokenType::LeftAngleBracket:
            case TokenType::LeftAngleBracketEqual:
            case TokenType::RightAngleBracket:
            case TokenType::RightAngleBracketEqual:
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool NumericIRType::isUnaryOperationSupported(const TokenType op) const {
    switch (op) {
        case TokenType::IncrementOperator:
        case TokenType::DecrementOperator:
        case TokenType::Plus:
        case TokenType::Minus:
            return true;
        default:
            return false;
    }
}

llvm::Value *NumericIRType::createBinaryOp(llvm::IRBuilder<> &builder,
                                           const TokenType op,
                                           llvm::Value *lhs,
                                           llvm::Value *rhs,
                                           const std::string &name) const {
    switch (op) {
        case TokenType::Plus:
            return createAdd(builder, lhs, rhs, name);
        case TokenType::Minus:
            return CreateSub(builder, lhs, rhs, name);
        case TokenType::Star:
            return CreateMul(builder, lhs, rhs, name);
        case TokenType::Slash:
            return CreateDiv(builder, lhs, rhs, name);
        case TokenType::LeftAngleBracket:
        case TokenType::LeftAngleBracketEqual:
        case TokenType::RightAngleBracket:
        case TokenType::RightAngleBracketEqual:
        case TokenType::Equal:
        case TokenType::NotEqual:
            return createCompare(builder, getComparePredicate(op), lhs, rhs);
        default:
            throw std::invalid_argument("Unsupported integer operation");
    }
}

llvm::Value * NumericIRType::createUnaryOp(llvm::IRBuilder<> &builder,
        const TokenType op,
        llvm::Value *operand,
        llvm::Value *storage,
        const std::string &name) const {
    auto *const delta =
        llvm::ConstantInt::get(operand->getType(), op == TokenType::IncrementOperator ? 1 : -1);

    auto *const result = createAdd(builder, operand, delta, name);

    if (storage) {
        builder.CreateStore(result, storage);
    }

    return op == TokenType::IncrementOperator ? result : operand;

}

llvm::Value *NumericIRType::createAdd(llvm::IRBuilder<> &builder,
                                      llvm::Value *lhs,
                                      llvm::Value *rhs,
                                      const std::string &name) const {
    if (isFloat) {
        return builder.CreateFAdd(lhs, rhs, name);
    }
    // if (llvm::dyn_cast<llvm::APInt> (lhs) && llvm::dyn_cast<llvm::APInt> (rhs)) {

        // return nullptr;
    // }
    return builder.CreateAdd(lhs, rhs, name);
}

llvm::Value *NumericIRType::CreateSub(llvm::IRBuilder<> &builder,
                                      llvm::Value *lhs,
                                      llvm::Value *rhs,
                                      const std::string &name) const {
    if (isFloat) {
        return builder.CreateFSub(lhs, rhs, name);
    }
    return builder.CreateSub(lhs, rhs, name);
}

llvm::Value *NumericIRType::CreateMul(llvm::IRBuilder<> &builder,
                                      llvm::Value *lhs,
                                      llvm::Value *rhs,
                                      const std::string &name) const {
    if (isFloat) {
        return builder.CreateFMul(lhs, rhs, name);
    }
    return builder.CreateMul(lhs, rhs, name);
}

llvm::Value *NumericIRType::CreateDiv(llvm::IRBuilder<> &builder,
                                      llvm::Value *lhs,
                                      llvm::Value *rhs,
                                      const std::string &name) const {
    if (isSigned) {
        if (isFloat) {
            return builder.CreateFSub(lhs, rhs, name);
        }
        return builder.CreateSDiv(lhs, rhs, name);
    }
    return builder.CreateUDiv(lhs, rhs, name);
}

llvm::CmpInst::Predicate NumericIRType::getComparePredicate(const TokenType op) const {
    llvm::CmpInst::Predicate pred;

    switch (op) {
        case TokenType::LeftAngleBracket:
            if (isSigned) {
                if (isFloat) {
                    pred = llvm::CmpInst::FCMP_OLT;
                } else {
                    pred = llvm::CmpInst::ICMP_SLT;
                }
            } else {
                pred = llvm::CmpInst::ICMP_ULT;
            }
            break;
        case TokenType::LeftAngleBracketEqual:
            if (isSigned) {
                if (isFloat) {
                    pred = llvm::CmpInst::FCMP_OLE;
                } else {
                    pred = llvm::CmpInst::ICMP_SLE;
                }
            } else {
                pred = llvm::CmpInst::ICMP_ULE;
            }
            break;
        case TokenType::RightAngleBracket:
            if (isSigned) {
                if (isFloat) {
                    pred = llvm::CmpInst::FCMP_OGT;
                } else {
                    pred = llvm::CmpInst::ICMP_SGT;
                }
            } else {
                pred = llvm::CmpInst::ICMP_UGT;
            }
            break;
        case TokenType::RightAngleBracketEqual:
            if (isSigned) {
                if (isFloat) {
                    pred = llvm::CmpInst::FCMP_OGE;
                } else {
                    pred = llvm::CmpInst::ICMP_SGE;
                }
            } else {
                pred = llvm::CmpInst::ICMP_UGE;
            }
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
    return pred;
}

llvm::Value *NumericIRType::createCompare(llvm::IRBuilder<> &builder,
                                          const llvm::CmpInst::Predicate pred,
                                          llvm::Value *lhs,
                                          llvm::Value *rhs) const {
    return builder.CreateCmp(pred, lhs, rhs, "cmp");
}
