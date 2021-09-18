#include "Parser.h"

#include <Common/Global.h>

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace std;

Parser::Parser() : enableIter1restriction(false) {}

Parser::Parser(bool enableIter1restriction)
    : enableIter1restriction(enableIter1restriction) {}

ProgramAST* Parser::Parse(std::vector<std::string> tokens) {
  if (tokens.empty()) {
    vector<ProcedureAST*> emptyList;
    return new ProgramAST(emptyList);
  }

  // set up instance variables
  this->tokens = tokens;
  tokenIterator = (this->tokens).begin();

  token = *tokenIterator;
  prevStmtNo = 0;

  return program();
}

ProgramAST* Parser::program() {
  vector<ProcedureAST*> procedures;
  while (!noMoreToken()) {
    ProcedureAST* procedureAST = procedure();
    procedures.push_back(procedureAST);
  }

  if (procedures.size() == 0) {
    throw runtime_error(
        "[Parser] a SIMPLE program must have at least 1 procedure.");
  }

  // TODO(gf): rm this after iter1
  if (enableIter1restriction && procedures.size() > 1) {
    throw runtime_error(
        "[Parser] No more than 1 procedure is allowed in a SIMPLE program in "
        "iteration 1.");
  }

  ProgramAST* temp = new ProgramAST(procedures);
  return temp;
}

ProcedureAST* Parser::procedure() {
  consumeToken("procedure");
  NAME procName = name();
  consumeToken("{");
  vector<StmtAST*> stmtList = stmtLst();
  consumeToken("}");
  return new ProcedureAST(procName, stmtList);
}

vector<StmtAST*> Parser::stmtLst() {
  vector<StmtAST*> stmtList;
  while (!expectToken("}")) {
    StmtAST* stmtAST = stmt();
    stmtList.push_back(stmtAST);
  }

  if (stmtList.size() == 0) {
    throw runtime_error("[Parser] a stmtLst must have at least 1 stmt.");
  }

  return stmtList;
}

StmtAST* Parser::stmt() {
  incrementStmtNo();

  if (expectToken("read")) {
    return readStmt();
  } else if (expectToken("print")) {
    return printStmt();
  } else if (expectToken("call")) {
    return callStmt();
  } else if (expectToken("while")) {
    return whileStmt();
  } else if (expectToken("if")) {
    return ifStmt();
  } else {  // assignment stmt
    return assignStmt();
  }
}

ReadStmtAST* Parser::readStmt() {
  consumeToken("read");
  NAME varName = name();
  consumeToken(";");
  return new ReadStmtAST(this->prevStmtNo, varName);
}

PrintStmtAST* Parser::printStmt() {
  consumeToken("print");
  NAME varName = name();
  consumeToken(";");
  return new PrintStmtAST(this->prevStmtNo, varName);
}

CallStmtAST* Parser::callStmt() {
  // TODO(gf): rm this after iter1
  if (enableIter1restriction) {
    throw runtime_error(
        "[Parser] Call Stmt is NOT allowed in a SIMPLE program in "
        "iteration 1.");
  }

  consumeToken("call");
  NAME procName = name();
  consumeToken(";");
  return new CallStmtAST(this->prevStmtNo, procName);
}

WhileStmtAST* Parser::whileStmt() {
  // need to rmb curr stmtNo cos calling stmtLst will increase prevStmtNo
  STMT_NO stmtNo = this->prevStmtNo;

  consumeToken("while");
  consumeToken("(");
  CondExprAST* condExprAST = condExpr();
  consumeToken(")");

  consumeToken("{");
  vector<StmtAST*> stmtList = stmtLst();
  consumeToken("}");

  return new WhileStmtAST(stmtNo, condExprAST, stmtList);
}

IfStmtAST* Parser::ifStmt() {
  STMT_NO stmtNo = this->prevStmtNo;

  consumeToken("if");
  consumeToken("(");
  CondExprAST* condExprAST = condExpr();
  consumeToken(")");

  consumeToken("then");
  consumeToken("{");
  vector<StmtAST*> thenBlock = stmtLst();
  consumeToken("}");

  consumeToken("else");
  consumeToken("{");
  vector<StmtAST*> elseBlock = stmtLst();
  consumeToken("}");

  return new IfStmtAST(stmtNo, condExprAST, thenBlock, elseBlock);
}

AssignStmtAST* Parser::assignStmt() {
  NAME varName = name();
  consumeToken("=");
  ArithAST* exprAST = expr();
  consumeToken(";");
  return new AssignStmtAST(this->prevStmtNo, varName, exprAST);
}

// =======================================
//  expr
// =======================================

ArithAST* Parser::buildExprAST(
    ArithAST* leftNode,
    vector<pair<string, ArithAST*>>& listSignAndTerm) const {
  if (listSignAndTerm.empty()) {
    return leftNode;
  }

  for (vector<pair<string, ArithAST*>>::iterator it = listSignAndTerm.begin();
       it != listSignAndTerm.end(); ++it) {
    string sign = it->first;
    ArithAST* rightNode = it->second;
    ArithAST* newNode = new ArithAST(sign, leftNode, rightNode);
    leftNode = newNode;
  }

  return leftNode;
}

ArithAST* Parser::expr() {
  ArithAST* leftNode = term();
  vector<pair<string, ArithAST*>> listSignAndTerm = exprPrime();
  return buildExprAST(leftNode, listSignAndTerm);
}

vector<pair<string, ArithAST*>> Parser::exprPrime() {
  if (expectToken("+")) {
    consumeToken("+");
    ArithAST* exprAST = term();
    vector<pair<string, ArithAST*>> rest = exprPrime();
    rest.insert(rest.begin(), make_pair("+", exprAST));
    return rest;
  } else if (expectToken("-")) {
    consumeToken("-");
    ArithAST* exprAST = term();
    vector<pair<string, ArithAST*>> rest = exprPrime();
    rest.insert(rest.begin(), make_pair("-", exprAST));
    return rest;
  } else {
    vector<pair<string, ArithAST*>> res;
    return res;
  }
}

ArithAST* Parser::term() {
  ArithAST* leftNode = factor();
  vector<pair<string, ArithAST*>> listSignAndFactor = termPrime();
  return buildExprAST(leftNode, listSignAndFactor);
}

vector<pair<string, ArithAST*>> Parser::termPrime() {
  if (expectToken("*")) {
    consumeToken("*");
    ArithAST* factorAST = factor();
    vector<pair<string, ArithAST*>> rest = termPrime();
    rest.insert(rest.begin(), make_pair("*", factorAST));
    return rest;
  } else if (expectToken("/")) {
    consumeToken("/");
    ArithAST* factorAST = factor();
    vector<pair<string, ArithAST*>> rest = termPrime();
    rest.insert(rest.begin(), make_pair("/", factorAST));
    return rest;
  } else if (expectToken("%")) {
    consumeToken("%");
    ArithAST* factorAST = factor();
    vector<pair<string, ArithAST*>> rest = termPrime();
    rest.insert(rest.begin(), make_pair("%", factorAST));
    return rest;
  } else {
    vector<pair<string, ArithAST*>> res;
    return res;
  }
}

FactorAST* Parser::factor() {
  if (expectToken("(")) {
    consumeToken("(");
    ArithAST* exprAST = expr();
    consumeToken(")");
    return new FactorAST(exprAST);
  } else if (isName()) {
    NAME varName = name();
    return new FactorAST(varName);
  } else if (isNumber()) {
    int constValue = number();
    return new FactorAST(constValue);
  } else {
    errorExpected("Left_Paren or Name or Number");
    return new FactorAST("dummy_value");  // won't reach this line
  }
}

// =======================================
//  cond expr
// =======================================

CondExprAST* Parser::condExpr() {
  // cases:
  // ! ( cond_expr )
  // ( cond_expr ) && ( cond_expr )
  // ( cond_expr ) || ( cond_expr )
  // rel_expr
  if (expectToken("!")) {
    consumeToken("!");
    consumeToken("(");
    CondExprAST* left = condExpr();
    consumeToken(")");
    return new CondExprAST("!", left);

  } else if (expectToken("(") && isRelExprInParens()) {
    // open paren could be the starting of either cond_expr or rel_factor
    //
    // if it is a rel_expr until the close paren, then this is either
    // ( (cond_expr) && (cond_expr) )
    // or
    // ( (cond_expr) || (cond_expr) )
    //
    // otherwise it is an expr in parens, and this is the case
    // ( rel_expr )
    // e.g. ((x + z + i) > a)

    consumeToken("(");
    CondExprAST* left = condExpr();
    consumeToken(")");

    string sign;
    if (expectToken("&&")) {
      consumeToken("&&");
      sign = "&&";
    } else if (expectToken("||")) {
      consumeToken("||");
      sign = "||";
    } else {
      errorExpected("'&&' or '||'");
    }

    consumeToken("(");
    CondExprAST* right = condExpr();
    consumeToken(")");

    return new CondExprAST(sign, left, right);

  } else {
    RelExprAST* relExprAST = relExpr();
    return new CondExprAST(relExprAST);
  }
}

RelExprAST* Parser::relExpr() {
  // cases:
  // >= <= == !=
  // > <
  FactorAST* left = relFactor();

  string sign;
  if (expectToken(">=")) {
    sign = ">=";
  } else if (expectToken("<=")) {
    sign = "<=";
  } else if (expectToken("==")) {
    sign = "==";
  } else if (expectToken("!=")) {
    sign = "!=";
  } else if (expectToken(">")) {
    sign = ">";
  } else if (expectToken("<")) {
    sign = "<";
  } else {
    errorExpected("any one of '>= <= == != > <'");
    FactorAST* emptyAST = new FactorAST(nullptr);
    return new RelExprAST("", emptyAST, emptyAST);
  }

  consumeToken(sign);
  FactorAST* right = relFactor();
  return new RelExprAST(sign, left, right);
}

FactorAST* Parser::relFactor() {
  // the only diff between relFactor() and factor() is that relFactor() don't
  // consume open and close paren
  ArithAST* exprAST = expr();
  return new FactorAST(exprAST);
}

// =======================================
//  name & number methods
// =======================================

NAME Parser::name() {
  if (!isName()) {
    errorExpected("name");
    return "";
  }
  NAME ret = token;
  nextToken();
  return ret;
}

int Parser::number() {
  if (!isNumber()) {
    errorExpected("number");
    return 0;
  }
  string currToken = token;
  nextToken();
  return stringToInt(currToken);
}

// =======================================
//  utility methods
// =======================================

bool Parser::expectToken(string expected) { return token == expected; }

void Parser::consumeToken(string toConsume) {
  if (!expectToken(toConsume)) {
    errorExpected(toConsume);
    return;
  }
  nextToken();
}

void Parser::nextToken() {
  if (noMoreToken()) {
    DMOprintErrMsgAndExit(
        "[Parser] no more token to parse but nextToken() was called");
    return;
  }

  tokenIterator++;

  if (noMoreToken()) {
    // special value to indicate there's no more token
    // speical value should consist of invalid lexical tokens
    // to safely differentiate it from valid ones
    // i.e. mainly to prevent speical value to be interpreted as a Name
    token = "_END_OF_PROGRAM_";
  } else {
    token = *tokenIterator;
  }
}

bool Parser::noMoreToken() { return tokenIterator == tokens.end(); }

void Parser::incrementStmtNo() { this->prevStmtNo++; }

bool Parser::isName() {
  if (token.size() == 0) {
    return false;
  }
  char c = token[0];
  return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
}

bool Parser::isNumber() {
  // assumes that tokenizer works properly, then only a Number can start with a
  // digit, and has only digits
  if (token.size() == 0) {
    return false;
  }
  char c = token[0];
  return '0' <= c && c <= '9';  // '0' in ascii is 48 and '9' is 57
}

bool Parser::isRelExprInParens() {
  // when this fucn is called, the current token should be "("
  if (token != "(") {
    DMOprintErrMsgAndExit(
        "[Parser] isRelExprInParens() asserts token to be ( but got " + token);
    return false;
  }

  auto tokenIteratorCopy = tokenIterator;
  int numOutstandingOpenParen = 0;

  while (tokenIteratorCopy != tokens.end()) {
    if (*tokenIteratorCopy == "(") {
      numOutstandingOpenParen++;
      tokenIteratorCopy++;
    } else if (*tokenIteratorCopy == ")") {
      numOutstandingOpenParen--;
      if (numOutstandingOpenParen == 0) return false;
      tokenIteratorCopy++;
    } else if (*tokenIteratorCopy == ">=") {
      return true;
    } else if (*tokenIteratorCopy == "<=") {
      return true;
    } else if (*tokenIteratorCopy == "==") {
      return true;
    } else if (*tokenIteratorCopy == "!=") {
      return true;
    } else if (*tokenIteratorCopy == ">") {
      return true;
    } else if (*tokenIteratorCopy == "<") {
      return true;
    } else {
      tokenIteratorCopy++;
    }
  }

  // consume all tokens and did not find comparison operators
  return false;
}

int Parser::stringToInt(string s) {
  int i;
  i = stoi(s);
  return i;
}

void Parser::errorExpected(string expected) {
  stringstream exMsg;
  exMsg << "[Parser] Expected token '" << expected
        << "' but encoutered token: '" << token << "'";
  throw runtime_error(exMsg.str());
}
