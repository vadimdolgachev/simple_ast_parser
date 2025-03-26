//
// Created by vadim on 25.03.25.
//

#include "ByteIRType.h"

bool ByteIRType::isOperationSupported(const TokenType op, const IRType *rhs) const {
    if (!NumericIRType::isOperationSupported(op, rhs)) {
        return false;
    }
    if (dynamic_cast<const NumericIRType *>(rhs)) {
        switch (op) {
            case TokenType::BitwiseAnd:
            case TokenType::BitwiseOr:
            case TokenType::BitwiseXor:
                // case TokenType::ShiftLeft:
                // case TokenType::ShiftRight:
                return true;
            default:
                return false;
        }
    }
    return false;
}

llvm::Value *ByteIRType::createBinaryOp(llvm::IRBuilder<> &builder,
                                        const TokenType op,
                                        llvm::Value *lhs,
                                        llvm::Value *rhs,
                                        const std::string &name) const {
    switch (op) {
        case TokenType::BitwiseAnd:
            return builder.CreateAnd(lhs, rhs, name);
        case TokenType::BitwiseOr:
            return builder.CreateOr(lhs, rhs, name);
        case TokenType::BitwiseXor:
            return builder.CreateXor(lhs, rhs, name);
        // case TokenType::ShiftLeft:
        // return builder.CreateShl(lhs, rhs, name);
        // case TokenType::ShiftRight:
        // return builder.CreateLShr(lhs, rhs, name);
        default:
            return NumericIRType::createBinaryOp(builder, op, lhs, rhs, name);
    }
}
