#include "Parser.h"

#include "Util.h"
#include "ast/BinOpNode.h"
#include "ast/FunctionCallNode.h"
#include "ast/FunctionNode.h"
#include "ast/UnaryOpNode.h"
#include "ast/IdentNode.h"
#include "ast/AssignmentNode.h"
#include "ast/BooleanNode.h"
#include "ast/ForLoopNode.h"
#include "ast/IfStatement.h"
#include "ast/StringNode.h"
#include "ast/NumberNode.h"
#include "ast/LoopCondNode.h"
#include "ast/BlockNode.h"
#include "ast/ProtoFunctionStatement.h"

namespace {
    bool isSign(const TokenType token) {
        return token == TokenType::Plus || token == TokenType::Minus;
    }
} // namespace

Parser::Parser(std::unique_ptr<Lexer> lexer_) :
    lexer(std::move(lexer_)) {
    lexer->nextToken();
}

bool Parser::hasNextNode() const {
    return lexer->hasNextToken();
}

std::unique_ptr<BaseNode> Parser::nextNode() {
    // Assignment
    if (auto assignment = tryParseAssignment()) {
        return assignment;
    }
    const auto token = lexer->currToken().type;
    if (token == TokenType::FunctionDefinition) {
        lexer->nextToken();
        return parseFunctionDef();
    }
    // Statements
    if (token == TokenType::If) {
        lexer->nextToken();
        return parseIfStatement();
    }
    if (token == TokenType::ForLoop) {
        lexer->nextToken();
        return parseForStatement();
    }
    if (token == TokenType::WhileLoop) {
        lexer->nextToken();
        return parseWhileStatement();
    }
    if (token == TokenType::DoLoop) {
        lexer->nextToken();
        return parseDoWhileStatement();
    }
    // Expressions
    auto result = parseExpr();
    consumeSemicolon();
    return result;
}

IfStatement::CondBranch Parser::parseCondBranch() {
    auto condition = parseExpr();
    auto thenBranch = parseBlock();
    return {std::move(condition), std::move(thenBranch)};
}

std::unique_ptr<BaseNode> Parser::tryParseAssignment(const bool needConsumeSemicolon) {
    if (const auto &token = lexer->currToken(); token.type == TokenType::Identifier) {
        lexer->nextToken();
        if (lexer->currToken().type == TokenType::Assignment && token.value.has_value()) {
            lexer->nextToken();
            auto result = parseAssignment(token.value.value());
            if (needConsumeSemicolon) {
                consumeSemicolon();
            }
            return result;
        }
        // Rollback
        lexer->prevToken();
    }
    return nullptr;
}

std::unique_ptr<BaseNode> Parser::parseForStatement() {
    if (lexer->currToken().type != TokenType::LeftParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected '(' after 'for'"));
    }
    lexer->nextToken(); // '('
    auto initExpr = tryParseAssignment(false);
    if (lexer->currToken().type != TokenType::Semicolon) {
        throw std::runtime_error(makeErrorMsg("Expected ';' after init statement"));
    }
    lexer->nextToken(); // ';'
    auto condition = parseExpr();
    if (lexer->currToken().type != TokenType::Semicolon) {
        throw std::runtime_error(makeErrorMsg("Expected ';' after condition"));
    }
    lexer->nextToken(); // ';'
    auto nextExpr = parseExpr();
    if (lexer->currToken().type != TokenType::RightParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected ')'"));
    }
    lexer->nextToken(); // ')'
    auto forBody = parseBlock();
    return std::make_unique<ForLoopNode>(
            std::move(initExpr),
            std::move(nextExpr),
            std::move(condition),
            std::move(forBody));
}

std::unique_ptr<BaseNode> Parser::parseWhileStatement() {
    if (lexer->currToken().type != TokenType::LeftParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected '(' after 'while'"));
    }
    lexer->nextToken(); // '('
    auto condition = parseExpr();
    if (lexer->currToken().type != TokenType::RightParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected ')' after condition"));
    }
    lexer->nextToken(); // ')'
    auto body = parseBlock();
    return std::make_unique<LoopCondNode>(std::move(condition), std::move(body), LoopCondNode::LoopType::While);
}

std::unique_ptr<BaseNode> Parser::parseDoWhileStatement() {
    if (lexer->currToken().type != TokenType::LeftCurlyBracket) {
        throw std::runtime_error(makeErrorMsg("Expected '{' after 'do'"));
    }
    auto body = parseCurlyBracketBlock();
    if (lexer->currToken().type != TokenType::WhileLoop) {
        throw std::runtime_error(makeErrorMsg("Expected 'while' keyword"));
    }
    lexer->nextToken();
    if (lexer->currToken().type != TokenType::LeftParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected '(' after 'while'"));
    }
    lexer->nextToken(); // '('
    auto condition = parseExpr();
    if (lexer->currToken().type != TokenType::RightParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected ')' after condition"));
    }
    lexer->nextToken(); // ')'
    return std::make_unique<LoopCondNode>(std::move(condition), std::move(body), LoopCondNode::LoopType::DoWhile);
}

void Parser::consumeSemicolon() const {
    if (lexer->currToken().type != TokenType::Semicolon) {
        throw std::runtime_error(makeErrorMsg("Expected ';' character"));
    }
    lexer->nextToken();
}

std::unique_ptr<BaseNode> Parser::parseIfStatement() {
    auto ifBranch = parseCondBranch();
    std::vector<IfStatement::CondBranch> elseIfBranches;
    std::optional<std::unique_ptr<BlockNode>> elseBranch;
    while (lexer->currToken().type == TokenType::Else) {
        lexer->nextToken(); // else
        if (lexer->currToken().type != TokenType::If) {
            elseBranch = parseBlock();
            break;
        }
        if (lexer->currToken().type != TokenType::If) {
            throw std::runtime_error(makeErrorMsg("If condition does not exist"));
        }
        lexer->nextToken(); // if
        elseIfBranches.emplace_back(parseCondBranch());
    }
    return std::make_unique<IfStatement>(std::move(ifBranch), std::move(elseIfBranches), std::move(elseBranch));
}

std::unique_ptr<BlockNode> Parser::parseCurlyBracketBlock() {
    BlockNode::BlockCode body;
    if (lexer->currToken().type != TokenType::LeftCurlyBracket) {
        throw std::runtime_error(makeErrorMsg("Unexpected token: " + lexer->currToken().toString()));
    }
    lexer->nextToken(); // {
    while (lexer->currToken().type != TokenType::RightCurlyBracket) {
        if (auto node = nextNode(); node != nullptr) {
            body.emplace_back(std::move(node));
        }
    }
    lexer->nextToken(); // }
    return std::make_unique<BlockNode>(std::move(body));
}

std::unique_ptr<ExpressionNode> Parser::parseExpr() {
    if (const auto &token = lexer->currToken();
        isSign(token.type)
        || token.type == TokenType::Number
        || token.type == TokenType::String
        || token.type == TokenType::Boolean
        || token.type == TokenType::LeftParenthesis
        || token.type == TokenType::Identifier
        || token.type == TokenType::IncrementOperator
        || token.type == TokenType::DecrementOperator
        || token.type == TokenType::LogicalNegation) {
        return parseBoolLogic();
    }
    throw std::runtime_error(makeErrorMsg("Unexpected token: " + lexer->currToken().toString()));
}

std::unique_ptr<ExpressionNode> Parser::parseBoolLogic() {
    auto lhs = parseComparisonExpr();
    while (lexer->currToken().type == TokenType::LogicalAnd
           || lexer->currToken().type == TokenType::LogicalOr) {
        const auto op = lexer->currToken().type;
        lexer->nextToken();
        auto rhs = parseExpr();
        lhs = std::make_unique<BinOpNode>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExpressionNode> Parser::parseComparisonExpr() {
    auto lhs = parseAdditiveExpr();
    while (lexer->currToken().type == TokenType::LeftAngleBracket
           || lexer->currToken().type == TokenType::LeftAngleBracketEqual
           || lexer->currToken().type == TokenType::RightAngleBracket
           || lexer->currToken().type == TokenType::RightAngleBracketEqual
           || lexer->currToken().type == TokenType::Equal
           || lexer->currToken().type == TokenType::NotEqual) {
        const auto op = lexer->currToken().type;
        lexer->nextToken();
        auto rhs = parseExpr();
        lhs = std::make_unique<BinOpNode>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExpressionNode> Parser::parseAdditiveExpr() {
    auto lhs = parseTerm();
    while (lexer->currToken().type == TokenType::Plus
           || lexer->currToken().type == TokenType::Minus) {
        const auto op = lexer->currToken().type;
        lexer->nextToken();
        auto rhs = parseExpr();
        lhs = std::make_unique<BinOpNode>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExpressionNode> Parser::parseTerm() {
    auto lhs = parseFactor();
    while (lexer->currToken().type == TokenType::Star
           || lexer->currToken().type == TokenType::Slash) {
        const auto op = lexer->currToken().type;
        lexer->nextToken();
        auto rhs = parseExpr();
        lhs = std::make_unique<BinOpNode>(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
}

std::unique_ptr<ExpressionNode> Parser::tryParseUnaryOp() {
    if (lexer->currToken().type == TokenType::Plus
        || lexer->currToken().type == TokenType::Minus
        || lexer->currToken().type == TokenType::LogicalNegation) {
        const auto type = lexer->currToken().type;
        lexer->nextToken();
        auto val = parseFactor();
        return std::make_unique<UnaryOpNode>(type,
                                             UnaryOpNode::UnaryOpType::Prefix,
                                             std::move(val));
    }
    return nullptr;
}

std::unique_ptr<ExpressionNode> Parser::tryParsePrefixOp() {
    if (lexer->currToken().type == TokenType::DecrementOperator
        || lexer->currToken().type == TokenType::IncrementOperator) {
        const auto type = lexer->currToken().type;
        lexer->nextToken();
        auto val = parseFactor();
        return std::make_unique<UnaryOpNode>(type,
                                             UnaryOpNode::UnaryOpType::Prefix,
                                             std::move(val));
    }
    return nullptr;
}

std::unique_ptr<ExpressionNode> Parser::tryParseIdentifier() {
    if (lexer->currToken().type == TokenType::Identifier) {
        auto ident = parseIdent();
        if (lexer->currToken().type == TokenType::DecrementOperator
            || lexer->currToken().type == TokenType::IncrementOperator) {
            const auto type = lexer->currToken().type;
            lexer->nextToken();
            return std::make_unique<UnaryOpNode>(type,
                                                 UnaryOpNode::UnaryOpType::Postfix,
                                                 std::move(ident));
        }
        if (lexer->currToken().type == TokenType::LeftParenthesis) {
            return parseFunctionCall(std::move(ident));
        }
        return ident;
    }
    return nullptr;
}

std::unique_ptr<BooleanNode> Parser::parseBoolean() const {
    if (lexer->currToken().type != TokenType::Boolean && lexer->currToken().value.has_value()) {
        throw std::runtime_error(makeErrorMsg("Invalid boolean expression"));
    }
    auto result = std::make_unique<BooleanNode>(lexer->currToken().value == "true");
    lexer->nextToken();
    return result;
}

std::unique_ptr<ExpressionNode> Parser::tryParseLiteral() const {
    if (lexer->currToken().type == TokenType::Number
        || (isSign(lexer->currToken().type) && lexer->peekToken().type == TokenType::Number)) {
        return parseNumber();
    }
    if (lexer->currToken().type == TokenType::String) {
        return parseString();
    }
    if (lexer->currToken().type == TokenType::Boolean) {
        return parseBoolean();
    }
    return nullptr;
}

std::unique_ptr<ExpressionNode> Parser::parseFactor() {
    if (lexer->currToken().type == TokenType::LeftParenthesis) {
        lexer->nextToken(); // "("
        auto expr = parseExpr();
        if (lexer->currToken().type != TokenType::RightParenthesis) {
            throw std::runtime_error(makeErrorMsg("Expected ')' character"));
        }
        lexer->nextToken(); // ")"
        return expr;
    }
    if (auto expr = tryParseLiteral()) {
        return expr;
    }
    if (auto expr = tryParseIdentifier()) {
        return expr;
    }
    if (auto expr = tryParseUnaryOp()) {
        return expr;
    }
    if (auto expr = tryParsePrefixOp()) {
        return expr;
    }
    throw std::runtime_error(makeErrorMsg("Unexpected token: " + lexer->currToken().toString()));
}

std::unique_ptr<IdentNode> Parser::parseIdent() const {
    auto ident = std::make_unique<IdentNode>(lexer->currToken().value.value_or(""));
    lexer->nextToken();
    return ident;
}

std::unique_ptr<StatementNode> Parser::parseAssignment(std::string identName) {
    return std::make_unique<AssignmentNode>(std::move(identName), parseExpr());
}

std::unique_ptr<NumberNode> Parser::parseNumber() const {
    auto sign = 1;
    if (isSign(lexer->currToken().type)) {
        sign = lexer->currToken().type == TokenType::Minus ? -1 : 1;
        lexer->nextToken();
    }
    if (!lexer->currToken().value.has_value()) {
        throw std::runtime_error(makeErrorMsg("Unexpected token: " + lexer->currToken().toString()));
    }
    auto number = std::make_unique<NumberNode>(sign * strtod(lexer->currToken().value.value().c_str(), nullptr));
    lexer->nextToken();
    return number;
}

std::unique_ptr<StringNode> Parser::parseString() const {
    auto result = std::make_unique<StringNode>(lexer->currToken().value.value_or(""));
    lexer->nextToken();
    return result;
}

std::unique_ptr<ExpressionNode> Parser::parseFunctionCall(std::unique_ptr<IdentNode> ident) {
    if (lexer->currToken().type != TokenType::LeftParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected '(' character"));
    }
    lexer->nextToken(); // '('
    std::vector<std::unique_ptr<ExpressionNode>> args;
    do {
        if (lexer->currToken().type == TokenType::Comma) {
            lexer->nextToken();
        }
        if (lexer->currToken().type != TokenType::RightParenthesis) {
            args.emplace_back(parseExpr());
        }
    } while (lexer->currToken().type == TokenType::Comma);

    if (lexer->currToken().type != TokenType::RightParenthesis) {
        throw std::runtime_error(makeErrorMsg("Expected ')' character"));
    }
    lexer->nextToken(); // ')'
    return std::make_unique<FunctionCallNode>(std::move(ident), std::move(args));
}

std::unique_ptr<StatementNode> Parser::parseFunctionDef() {
    if (lexer->currToken().type != TokenType::Identifier) {
        throw std::runtime_error(makeErrorMsg("Unexpected token: " + lexer->currToken().toString()));
    }
    auto fnName = parseIdent();
    lexer->nextToken();
    std::vector<std::string> params;
    while (lexer->currToken().type != TokenType::RightParenthesis) {
        params.emplace_back(parseIdent()->name);
        if (lexer->currToken().type != TokenType::Comma && lexer->currToken().type != TokenType::RightParenthesis) {
            throw std::runtime_error(makeErrorMsg("Unexpected token: " + lexer->currToken().toString()));
        }
        if (lexer->currToken().type == TokenType::Comma) {
            lexer->nextToken();
        }
    }
    lexer->nextToken(); // ")"
    auto proto = std::make_unique<ProtoFunctionStatement>(fnName->name, params);
    if (lexer->currToken().type == TokenType::LeftCurlyBracket) {
        return std::make_unique<FunctionNode>(std::move(proto), parseCurlyBracketBlock());
    }
    consumeSemicolon();
    return proto;
}

std::unique_ptr<BlockNode> Parser::parseBlock() {
    std::unique_ptr<BlockNode> block;
    if (lexer->currToken().type == TokenType::LeftCurlyBracket) {
        block = parseCurlyBracketBlock();
    } else {
        block = std::make_unique<BlockNode>(makeVectorUnique<BaseNode>(parseExpr()));
        consumeSemicolon();
    }
    return block;
}

std::string Parser::makeErrorMsg(const std::string &msg) const {
    std::string lines;
    uint32_t startLinePos = 0;
    for (const auto &[ch, pos]: lexer->readText()) {
        if (ch == '\n') {
            startLinePos = pos + 1;
        }
        lines.push_back(ch);
    }
    const auto padding = lexer->currToken().startPosition - startLinePos;
    lines.push_back('\n');
    lines.insert(lines.end(), padding, '-');
    lines.insert(lines.end(), lexer->currToken().endPosition - lexer->currToken().startPosition + 1, '^');
    return std::format("\n{}\n{}", lines, msg);
}