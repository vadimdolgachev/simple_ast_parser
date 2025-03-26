//
// Created by vadim on 25.03.25.
//

#ifndef CHARIRTYPE_H
#define CHARIRTYPE_H

#include "NumericIRType.h"

class CharIRType final : public NumericIRType {
public:
    explicit CharIRType(bool isPointer = false);

protected:
    [[nodiscard]] llvm::Type *getBaseLLVMType(llvm::LLVMContext &context) const override;
};


#endif //CHARIRTYPE_H
