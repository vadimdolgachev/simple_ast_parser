//
// Created by vadim on 25.03.25.
//

#ifndef BYTEIRTYPE_H
#define BYTEIRTYPE_H

#include "NumericIRType.h"

class ByteIRType final : public NumericIRType {
public:
    explicit ByteIRType(const bool isPointer = false) :
        NumericIRType(isPointer, false, false) {}

    [[nodiscard]] bool isOperationSupported(TokenType op,
                                            const IRType *rhs) const override;

    llvm::Value *createBinaryOp(llvm::IRBuilder<> &builder,
                                TokenType op,
                                llvm::Value *lhs,
                                llvm::Value *rhs,
                                const std::string &name) const override;

    llvm::Value *createValue(const BaseNode *node, llvm::IRBuilder<> &builder, llvm::Module &module) override;

protected:
    [[nodiscard]] llvm::Type *getBaseLLVMType(llvm::LLVMContext &context) const override;
};


#endif //BYTEIRTYPE_H
