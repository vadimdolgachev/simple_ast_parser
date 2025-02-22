#ifndef PARSER_H
#define PARSER_H

#include <memory>

#include "Lexer.h"
#include "ast/BaseNode.h"
#include "ast/IfStatement.h"

class Parser {
public:
    explicit Parser(std::unique_ptr<Lexer> lexer);

    explicit operator bool() const;

    [[nodiscard]] std::unique_ptr<BaseNode> nextNode();

private:
    [[nodiscard]] std::unique_ptr<ExpressionNode> parseExpr();
    [[nodiscard]] std::unique_ptr<ExpressionNode> parseComparisonExpr();
    [[nodiscard]] std::unique_ptr<ExpressionNode> parseAdditiveExpr();
    [[nodiscard]] std::unique_ptr<ExpressionNode> parseBoolLogic();
    [[nodiscard]] std::unique_ptr<ExpressionNode> parseFactor();
    [[nodiscard]] std::unique_ptr<ExpressionNode> parseTerm();
    [[nodiscard]] std::unique_ptr<ExpressionNode> tryParseUnaryOp();
    [[nodiscard]] std::unique_ptr<ExpressionNode> tryParsePrefixOp();
    [[nodiscard]] std::unique_ptr<ExpressionNode> tryParseIdentifier();
    [[nodiscard]] std::unique_ptr<BooleanNode> parseBoolean() const;
    [[nodiscard]] std::unique_ptr<ExpressionNode> tryParseLiteral() const;
    [[nodiscard]] std::unique_ptr<IdentNode> parseIdent() const;
    [[nodiscard]] std::unique_ptr<StatementNode> parseAssignment(std::string identName);
    [[nodiscard]] std::unique_ptr<NumberNode> parseNumber() const;
    [[nodiscard]] std::unique_ptr<StringNode> parseString() const;
    [[nodiscard]] std::unique_ptr<ExpressionNode> parseFunctionCall(std::unique_ptr<IdentNode> ident);
    [[nodiscard]] std::unique_ptr<StatementNode> parseFunctionDef();
    [[nodiscard]] std::unique_ptr<BaseNode> parseIfStatement();
    [[nodiscard]] std::unique_ptr<BlockNode> parseCurlyBracketBlock();
    [[nodiscard]] IfStatement::CondBranch parseCondBranch();
    [[nodiscard]] std::unique_ptr<BaseNode> tryParseAssignment(bool needConsumeSemicolon = true);
    [[nodiscard]] std::unique_ptr<BaseNode> parseForStatement();
    [[nodiscard]] std::unique_ptr<BaseNode> parseWhileStatement();
    [[nodiscard]] std::unique_ptr<BaseNode> parseDoWhileStatement();
    [[nodiscard]] std::unique_ptr<BlockNode> parseBlock();
    void consumeSemicolon() const;
    std::string makeErrorMsg(const std::string &msg) const;

    std::unique_ptr<Lexer> lexer;
};

#endif // PARSER_H
