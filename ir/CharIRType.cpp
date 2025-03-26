//
// Created by vadim on 25.03.25.
//

#include "CharIRType.h"

CharIRType::CharIRType(const bool isPointer):
    NumericIRType(isPointer, true, false) {}

llvm::Type *CharIRType::getBaseLLVMType(llvm::LLVMContext &context) const {
    return llvm::Type::getInt8Ty(context);
}
