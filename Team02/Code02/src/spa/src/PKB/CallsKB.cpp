#include "CallsKB.h"
#include "Common/Global.h"

using namespace std;

CallsKB::CallsKB(Table* procTable) {
  this->procTable = procTable;
  stmtToCallee = unordered_map<STMT_NO, PROC_IDX>({});
  table = unordered_map<PROC_IDX, unordered_set<PROC_IDX>>({});
  invTable = unordered_map<PROC_IDX, unordered_set<PROC_IDX>>({});
  tableT = unordered_map<PROC_IDX, unordered_set<PROC_IDX>>({});
  invTableT = unordered_map<PROC_IDX, unordered_set<PROC_IDX>>({});
}

void CallsKB::addCalls(STMT_NO s, PROC_NAME caller, PROC_NAME callee) {
  PROC_IDX p1 = procTable->insert(caller);
  PROC_IDX p2 = procTable->insert(callee);
  stmtToCallee.insert({ s, p2 });
  try {  // caller already exists in table
    table.at(p1).insert(p2);
  }
  catch (const out_of_range& e) {
    unordered_set<PROC_IDX> newSet({});
    newSet.insert(p2);
    table.insert({ p1, newSet });
  }
  try {  // callee already exists in table
    invTable.at(p2).insert(p1);
  }
  catch (const out_of_range& e) {
    unordered_set<PROC_IDX> newSet({});
    newSet.insert(p1);
    invTable.insert({ p2, newSet });
  }
}

// indices guaranteed to be valid
void CallsKB::addCallsT(PROC_NAME caller, PROC_NAME callee) {
    PROC_IDX p1 = procTable->getIndex(caller);
    PROC_IDX p2 = procTable->getIndex(callee);
    try {
        tableT.at(p1).insert(p2);
    }
    catch (const out_of_range& e) {
        unordered_set<PROC_IDX> newSet({});
        newSet.insert(p2);
        tableT.insert({ p1, newSet });
    }
    try {
        invTableT.at(p2).insert(p1);
    }
    catch (const out_of_range& e) {
        unordered_set<PROC_IDX> newSet({});
        newSet.insert(p1);
        invTableT.insert({ p2, newSet });
    }
}

bool CallsKB::isCalls(PROC_NAME caller, PROC_NAME callee) {
  try {
    unordered_set<PROC_IDX> setAtCaller = table.at(procTable->getIndex(
        caller));
    return setAtCaller.count(procTable->getIndex(callee));
  }
  catch (const out_of_range& e) {
    return false;
  }
}

bool CallsKB::isCallsT(PROC_NAME caller, PROC_NAME callee) {
  try {
    unordered_set<PROC_IDX> setTAtCaller = tableT.at(procTable->getIndex(
        caller));
    return setTAtCaller.count(procTable->getIndex(callee));
  }
  catch (const out_of_range& e) {
    return false;
  }
}

unordered_set<PROC_IDX> CallsKB::getProcsCalledBy(PROC_NAME proc) {
  try {
    return table.at(procTable->getIndex(proc));
  }
  catch (const out_of_range& e) {
    unordered_set<int> emptySet({});
    return emptySet;
  }
}

unordered_set<PROC_IDX> CallsKB::getProcsCalledTBy(PROC_NAME proc) {
  try {
    return tableT.at(procTable->getIndex(proc));
  }
  catch (const out_of_range& e) {
    unordered_set<int> emptySet({});
    return emptySet;
  }
}

unordered_set<PROC_IDX> CallsKB::getCallerProcs(PROC_NAME proc) {
  try {
    return invTable.at(procTable->getIndex(proc));
  }
  catch (const out_of_range& e) {
    unordered_set<int> emptySet({});
    return emptySet;
  }
}

unordered_set<PROC_IDX> CallsKB::getCallerTProcs(PROC_NAME proc) {
  try {
    return invTableT.at(procTable->getIndex(proc));
  }
  catch (const out_of_range& e) {
    unordered_set<int> emptySet({});
    return emptySet;
  }
}

vector<pair<PROC_IDX, vector<PROC_IDX>>> CallsKB::getAllCallsPairs() {
  vector<pair<PROC_IDX, vector<PROC_IDX>>> result;
  for (auto it = table.begin(); it != table.end(); it++) {
    PROC_IDX p = it->first;
    unordered_set<PROC_IDX> allProcs = it->second;
    vector<PROC_IDX> allProcsVector = vector(allProcs.begin(), allProcs.end());
    result.push_back(pair(p, allProcsVector));
  }
  return result;
}

vector<pair<PROC_IDX, vector<PROC_IDX>>> CallsKB::getAllCallsTPairs() {
  vector<pair<PROC_IDX, vector<PROC_IDX>>> result;
  for (auto it = tableT.begin(); it != tableT.end(); it++) {
    PROC_IDX p = it->first;
    unordered_set<PROC_IDX> allProcs = it->second;
    vector<PROC_IDX> allProcsVector = vector(allProcs.begin(), allProcs.end());
    result.push_back(pair(p, allProcsVector));
  }
  return result;
}


PROC_IDX CallsKB::getProcCalledByCallStmt(int callStmtNum) {
  try {
    return stmtToCallee.at(callStmtNum);
  }
  catch (const out_of_range& e) {
    return -1;
  }
}

unordered_set<STMT_NO> CallsKB::getAllCallStmts() {
  unordered_set<STMT_NO> res;
  for (auto it = stmtToCallee.begin(); it != stmtToCallee.end(); it++) {
    res.insert(it->first);
  }
  return res;
}

bool CallsKB::isCallStmt(STMT_NO s) {
  unordered_set<STMT_NO> allCallStmts = getAllCallStmts();
  return allCallStmts.count(s) > 0;
}
