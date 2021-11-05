#include "QueryEvaluator.h"

#include <../../autotester/src/AbstractWrapper.h>

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/Global.h"

using namespace std;
using namespace query;

QueryEvaluator::QueryEvaluator(PKB* pkb, QueryOptimizer* optimizer)
    : nextEvaluator(pkb),
      affectsEvaluator(pkb),
      patternEvaluator(pkb),
      withEvaluator(pkb) {
  this->pkb = pkb;
  this->optimizer = optimizer;
  areAllClausesTrue = true;
  finalQueryResults = {};
  groupQueryResults = {};
  queryResultsSynonyms = {};

  QueryEvaluatorCache* onDemandRsCache = new QueryEvaluatorCache();
  nextEvaluator.attachCache(onDemandRsCache);
}

FinalQueryResults QueryEvaluator::evaluateQuery(
    unordered_map<string, DesignEntity> synonymMap, SelectClause select) {
  this->synonymMap = synonymMap;
  finalQueryResults.clear();

  while (true) {
    optional<GroupDetails> optGroupDetails = optimizer->GetNextGroupDetails();
    if (!optGroupDetails.has_value()) {
      break;
    }

    while (true) {
      SynonymCountsTable synonymCounts = getSynonymCounts();
      optional<ConditionClause> optClause =
          optimizer->GetNextClause(synonymCounts);
      if (!optClause.has_value()) {
        break;
      }

      ConditionClause clause = optClause.value();
      if (clause.conditionClauseType == ConditionClauseType::SUCH_THAT) {
        evaluateSuchThatClause(clause.suchThatClause);
      } else if (clause.conditionClauseType == ConditionClauseType::PATTERN) {
        evaluatePatternClause(clause.patternClause);
      } else {
        evaluateWithClause(clause.withClause);
      }

      if (!areAllClausesTrue) {
        clauseSynonymValuesTable.clear();
        // early termination as soon as any clause is false
        // if select bool, false
        if (select.selectType == SelectType::BOOLEAN) {
          return {{FALSE_SELECT_BOOL_RESULT}};
        } else {
          return {};
        }
      }

      if (AbstractWrapper::GlobalStop) {
        // check if TLE after each clause evaluation
        // return whatever results we can
        return getSelectSynonymFinalResults(select);
      }
      clauseSynonymValuesTable.clear();
    }

    GroupDetails groupDetails = optGroupDetails.value();
    if (groupDetails.isBooleanGroup) {
      groupQueryResults.clear();
      continue;
    }
    // after each grp, keep only IntermediateQueryResult with the select syns
    filterGroupResultsBySelectSynonyms(groupDetails.selectedSynonyms);
    mergeGroupResultsIntoFinalResults();
    // clean up group data
    filterQuerySynonymsBySelectSynonyms(groupDetails.selectedSynonyms);
    groupQueryResults.clear();
  }

  return getSelectSynonymFinalResults(select);
}

/* Evaluate Such That Clauses -------------------------------------------- */
void QueryEvaluator::evaluateSuchThatClause(SuchThatClause clause) {
  auto relationshipType = clause.relationshipType;

  switch (clause.relationshipType) {
    case RelationshipType::FOLLOWS:
    case RelationshipType::FOLLOWS_T:
    case RelationshipType::PARENT:
    case RelationshipType::PARENT_T:
    case RelationshipType::USES_S:
    case RelationshipType::USES_P:
    case RelationshipType::MODIFIES_S:
    case RelationshipType::MODIFIES_P:
    case RelationshipType::CALLS:
    case RelationshipType::CALLS_T:
    case RelationshipType::NEXT:
    case RelationshipType::NEXT_BIP:
      return evaluateSuchThatClauseHelper(clause);
    case RelationshipType::NEXT_T:
    case RelationshipType::NEXT_BIP_T:
    case RelationshipType::AFFECTS:
    case RelationshipType::AFFECTS_T:
      return evaluateSuchThatOnDemandClause(clause);
    default:
      DMOprintErrMsgAndExit(
          "[QueryEvaluator][evaluateSuchThatClause] invalid relationship type");
  }
}

void QueryEvaluator::evaluateSuchThatClauseHelper(SuchThatClause clause) {
  // resolve left and right param one by one
  // resolve = determine its possible values

  // check whether left or right param is in the result table
  // if it is, they are immediately resolve
  // if not, pull their values from PKB

  // if we know the r/s type, wildcard can be treated as an generic syn that
  // doesn't get added into the result table in the end
  // e.g. follows(_, _)
  // then we know the wildcards here can be treated as s1 & s2, and the only
  // diff is that they don't get added into the table in the end

  // first look at left, then right
  // if left is literal, alr resolved
  // if left is syn, check whether it is in the result table
  //       if it is, resolved
  //       if not, pull from PKB
  // if left is wildcard, treat it as a syn and pull all the possible values
  // from PKB

  // then look at right and resolve according to resolved left values
  // if right is literal, check whether it agrees with
  // left param's resolved values
  // if right is syn, get its values based on left param's
  // resolved valuesleft param's resolved values
  // if right is wildcard, treat it as a syn and pull all the possible values
  // from PKB

  auto left = clause.leftParam;
  auto right = clause.rightParam;
  vector<vector<int>> leftRightValuePairs;

  // if both parems are syn and present in the result table, grab them
  if (left.type == ParamType::SYNONYM && right.type == ParamType::SYNONYM &&
      (queryResultsSynonyms.count(left.value) > 0) &&
      (queryResultsSynonyms.count(right.value) > 0)) {
    leftRightValuePairs = resolveBothParamsFromResultTable(clause);
  } else {
    // otherwise resolve left then right param
    unordered_set<int> leftValues = resolveLeftParam(clause);
    leftRightValuePairs = resolveRightParamFromLeftValues(clause, leftValues);
  }

  auto isLiteral = [](ParamType type) {
    return type == ParamType::INTEGER_LITERAL ||
           type == ParamType::NAME_LITERAL;
  };
  bool isBoolClause =
      (isLiteral(left.type) && isLiteral(right.type)) ||
      (isLiteral(left.type) && right.type == ParamType::WILDCARD) ||
      (left.type == ParamType::WILDCARD && isLiteral(right.type)) ||
      (left.type == ParamType::WILDCARD && right.type == ParamType::WILDCARD);

  if (isBoolClause) {
    areAllClausesTrue = !leftRightValuePairs.empty();
    return;
  }

  // transform data if it's (syn, integer) or a (syn, wildcard) combination
  if (left.type == ParamType::SYNONYM && right.type == ParamType::SYNONYM) {
    // do nothing
  } else if (left.type == ParamType::SYNONYM) {
    unordered_set<int> uniqueValues;
    for (auto p : leftRightValuePairs) {
      uniqueValues.insert(p.front());
    }
    leftRightValuePairs = formatRefResults(uniqueValues);
  } else if (right.type == ParamType::SYNONYM) {
    unordered_set<int> uniqueValues;
    for (auto p : leftRightValuePairs) {
      uniqueValues.insert(p.back());
    }
    leftRightValuePairs = formatRefResults(uniqueValues);
  } else {
    DMOprintErrMsgAndExit(
        "shouldn't reach here, this if-else block asserts that at least one "
        "of the params is a syn");
  }

  filterAndAddIncomingResults(leftRightValuePairs, left, right);
}

void QueryEvaluator::evaluateSuchThatOnDemandClause(SuchThatClause clause) {
  auto left = clause.leftParam;
  auto right = clause.rightParam;
  auto relationshipType = clause.relationshipType;

  pair<ParamType, ParamType> paramTypesCombo = make_pair(left.type, right.type);
  vector<pair<ParamType, ParamType>> boolParamTypesCombos = {
      {ParamType::INTEGER_LITERAL, ParamType::INTEGER_LITERAL},
      {ParamType::INTEGER_LITERAL, ParamType::WILDCARD},
      {ParamType::WILDCARD, ParamType::INTEGER_LITERAL},
      {ParamType::WILDCARD, ParamType::WILDCARD}};
  bool isBoolClause =
      find(boolParamTypesCombos.begin(), boolParamTypesCombos.end(),
           paramTypesCombo) != boolParamTypesCombos.end();
  vector<pair<ParamType, ParamType>> pairParamTypesCombos = {
      {ParamType::SYNONYM, ParamType::SYNONYM},
      {ParamType::SYNONYM, ParamType::WILDCARD},
      {ParamType::WILDCARD, ParamType::SYNONYM}};
  bool isPairClause =
      find(pairParamTypesCombos.begin(), pairParamTypesCombos.end(),
           paramTypesCombo) != pairParamTypesCombos.end();

  // evaluate clause that returns a boolean
  if (isBoolClause) {
    areAllClausesTrue = callSubEvaluatorBool(relationshipType, left, right);
    return;
  }

  ClauseIncomingResults incomingResults;
  // the rest of clauses has at least one synonym
  // convert clauses with synonyms to bool clauses if possible
  // by taking INTEGER/NAME_LITERAL results from previous clauses
  auto newParams = getResolvedParamsForOnDemandRs(left, right);
  if (!newParams.empty()) {
    // if there is any results that can be reused from previous clauses
    for (auto newParam : newParams) {
      Param newLeft = get<0>(newParam);
      Param newRight = get<1>(newParam);
      // if not fully converted to literals & wildcards
      if (newLeft.type == ParamType::SYNONYM ||
          newRight.type == ParamType::SYNONYM) {
        ClauseIncomingResults newResults =
            callSubEvaluatorRef(relationshipType, newLeft, newRight);
        incomingResults.insert(incomingResults.end(), newResults.begin(),
                               newResults.end());
        continue;
      }

      // fully converted to literals & wildcards
      bool isClauseTrue =
          callSubEvaluatorBool(relationshipType, newLeft, newRight);
      if (isClauseTrue) {
        ParamPosition paramPosition = get<2>(newParam);
        switch (paramPosition) {
          case ParamPosition::BOTH:
            incomingResults.push_back(
                {stoi(newLeft.value), stoi(newRight.value)});
            break;
          case ParamPosition::LEFT:
            incomingResults.push_back({stoi(newLeft.value)});
            break;
          case ParamPosition::RIGHT:
            incomingResults.push_back({stoi(newRight.value)});
            break;
          default:
            break;
        }
      }
    }
    if (incomingResults.empty()) {
      areAllClausesTrue = false;
      return;
    }
  } else {
    if (isPairClause) {  // 1 syn & 1 wildcard | 2 syn | 2 wildcards
      // evaluate clause that returns a vector of ref pairs
      incomingResults = callSubEvaluatorPair(relationshipType, left, right);
    } else {  // one synonym
      // evaluate clause that returns a vector of single refs
      incomingResults = callSubEvaluatorRef(relationshipType, left, right);
    }
  }

  filterAndAddIncomingResults(incomingResults, left, right);
}

bool QueryEvaluator::callSubEvaluatorBool(RelationshipType relationshipType,
                                          const Param& left,
                                          const Param& right) {
  switch (relationshipType) {
    case RelationshipType::NEXT_T:
    case RelationshipType::NEXT_BIP_T:
      return nextEvaluator.evaluateBoolNextTNextBipT(relationshipType, left,
                                                     right);
    case RelationshipType::AFFECTS:
      return affectsEvaluator.evaluateBoolAffects(left, right);
    case RelationshipType::AFFECTS_T:
      return affectsEvaluator.evaluateBoolAffectsT(left, right);
    default:
      return false;
  }
}

ClauseIncomingResults QueryEvaluator::callSubEvaluatorRef(
    RelationshipType relationshipType, const Param& left, const Param& right) {
  unordered_set<STMT_NO> refResults = {};
  switch (relationshipType) {
    case RelationshipType::NEXT_T:
    case RelationshipType::NEXT_BIP_T:
      refResults =
          nextEvaluator.evaluateNextTNextBipT(relationshipType, left, right);
      break;
    case RelationshipType::AFFECTS:
      refResults = affectsEvaluator.evaluateStmtAffects(left, right);
      break;
    case RelationshipType::AFFECTS_T:
      refResults = affectsEvaluator.evaluateStmtAffectsT(left, right);
      break;
    default:
      return {};
  }
  return formatRefResults(refResults);
}

ClauseIncomingResults QueryEvaluator::callSubEvaluatorPair(
    RelationshipType relationshipType, const Param& left, const Param& right) {
  switch (relationshipType) {
    case RelationshipType::NEXT_T:
    case RelationshipType::NEXT_BIP_T:
      return nextEvaluator.evaluatePairNextTNextBipT(relationshipType, left,
                                                     right);
    case RelationshipType::AFFECTS:
      return affectsEvaluator.evaluatePairAffects();
    case RelationshipType::AFFECTS_T:
      return affectsEvaluator.evaluatePairAffectsT();
    default:
      return {};
  }
}

/* Evaluate Pattern Clauses ---------------------------------------------- */
void QueryEvaluator::evaluatePatternClause(PatternClause clause) {
  Synonym matchSynonym = clause.matchSynonym;
  Param varParam = clause.leftParam;
  PatternExpr patternExpr = clause.patternExpr;
  DesignEntity designEntity = clause.matchSynonym.entity;

  ClauseIncomingResults incomingResults;
  bool isRefClause = varParam.type == ParamType::NAME_LITERAL ||
                     varParam.type == ParamType::WILDCARD;

  if (isRefClause) {
    incomingResults =
        callPatternSubEvaluatorRef(designEntity, varParam, patternExpr);
  } else {
    incomingResults =
        callPatternSubEvaluatorPair(designEntity, varParam, patternExpr);
  }

  Param patternSynonymParam = {ParamType::SYNONYM, matchSynonym.name};
  filterAndAddIncomingResults(incomingResults, patternSynonymParam, varParam);
}

ClauseIncomingResults QueryEvaluator::callPatternSubEvaluatorRef(
    DesignEntity designEntity, const Param& varParam,
    const PatternExpr& patternExpr) {
  unordered_set<int> refResults = {};
  switch (designEntity) {
    case DesignEntity::ASSIGN:
      refResults =
          patternEvaluator.evaluateAssignPattern(varParam, patternExpr);
      break;
    case DesignEntity::IF:
      refResults = patternEvaluator.evaluateIfPattern(varParam);
      break;
    case DesignEntity::WHILE:
      refResults = patternEvaluator.evaluateWhilePattern(varParam);
      break;
    default:
      return {};
  }
  return formatRefResults(refResults);
}

ClauseIncomingResults QueryEvaluator::callPatternSubEvaluatorPair(
    DesignEntity designEntity, const Param& varParam,
    const PatternExpr& patternExpr) {
  switch (designEntity) {
    case DesignEntity::ASSIGN:
      return patternEvaluator.evaluateAssignPairPattern(varParam, patternExpr);
    case DesignEntity::IF:
      return patternEvaluator.evaluateIfPairPattern(varParam);
    case DesignEntity::WHILE:
      return patternEvaluator.evaluateWhilePairPattern(varParam);
    default:
      return {};
  }
}

/* Evaluate With Clauses -------------------------------------------------- */
void QueryEvaluator::evaluateWithClause(WithClause clause) {
  Param left = clause.leftParam;
  Param right = clause.rightParam;

  bool isLeftParamSynonym = false;
  bool isRightParamSynonym = false;
  vector<vector<int>> leftSynoynmValues = {};
  vector<vector<int>> rightSynoynmValues = {};
  vector<vector<int>> leftAndRightSynonymValues = {};

  unordered_set<ParamType> synonymTypes = {
      ParamType::ATTRIBUTE_PROC_NAME, ParamType::ATTRIBUTE_VAR_NAME,
      ParamType::ATTRIBUTE_VALUE, ParamType::ATTRIBUTE_STMT_NUM,
      ParamType::SYNONYM};

  if (synonymTypes.find(left.type) != synonymTypes.end()) {
    isLeftParamSynonym = true;
    unordered_set<STMT_NO> allValues = getAllValuesOfSynonym(left.value);
    for (auto value : allValues) {
      leftSynoynmValues.push_back({value});
    }
  }

  if (synonymTypes.find(right.type) != synonymTypes.end()) {
    isRightParamSynonym = true;
    unordered_set<STMT_NO> allValues = getAllValuesOfSynonym(right.value);
    for (auto value : allValues) {
      rightSynoynmValues.push_back({value});
    }
  }
  if (isLeftParamSynonym && isRightParamSynonym) {
    for (auto leftStmt : leftSynoynmValues) {
      for (auto rightStmt : rightSynoynmValues) {
        leftAndRightSynonymValues.push_back({leftStmt[0], rightStmt[0]});
      }
    }
  }

  if (groupQueryResults.empty()) {
    // initialize results with all possible left and/or right synonym values
    if (isLeftParamSynonym && isRightParamSynonym) {
      initializeQueryResults(leftAndRightSynonymValues,
                             {ParamType::SYNONYM, left.value},
                             {ParamType::SYNONYM, right.value});
    } else if (isLeftParamSynonym) {
      initializeQueryResults(leftSynoynmValues,
                             {ParamType::SYNONYM, left.value}, right);
    } else if (isRightParamSynonym) {
      initializeQueryResults(rightSynoynmValues, left,
                             {ParamType::SYNONYM, right.value});
    }
  } else {
    bool isLeftSynInQueryResults =
        queryResultsSynonyms.find(left.value) != queryResultsSynonyms.end();
    bool isRightSynInQueryResults =
        queryResultsSynonyms.find(right.value) != queryResultsSynonyms.end();

    // add all possible left and/or right synonym values if not yet in results
    if (isLeftParamSynonym && !isLeftSynInQueryResults && isRightParamSynonym &&
        !isRightSynInQueryResults) {
      queryResultsSynonyms.insert(left.value);
      queryResultsSynonyms.insert(right.value);
      crossProduct(leftAndRightSynonymValues, {left.value, right.value});
    } else if (isLeftParamSynonym && !isLeftSynInQueryResults) {
      queryResultsSynonyms.insert(left.value);
      crossProduct(leftSynoynmValues, {left.value});
    } else if (isRightParamSynonym && !isRightSynInQueryResults) {
      queryResultsSynonyms.insert(right.value);
      crossProduct(rightSynoynmValues, {right.value});
    }
  }

  auto evaluatedResults = withEvaluator.evaluateAttributes(
      left, right, synonymMap, groupQueryResults);
  bool isClauseTrue = get<0>(evaluatedResults);
  vector<IntermediateQueryResult> newQueryResults = get<1>(evaluatedResults);
  SynonymValuesTable newSynonymValuesTable = get<2>(evaluatedResults);
  if (newQueryResults != groupQueryResults) {
    clauseSynonymValuesTable = newSynonymValuesTable;
  }

  if (!isClauseTrue) {
    areAllClausesTrue = false;
    return;
  }

  groupQueryResults = newQueryResults;
}

/* Filter And Merge Results For Each Clause ------------------------------- */
void QueryEvaluator::filterAndAddIncomingResults(
    ClauseIncomingResults incomingResults, const Param& left,
    const Param& right) {
  if (incomingResults.empty()) {
    areAllClausesTrue = false;
    return;
  }
  ClauseIncomingResults filteredIncomingResults =
      filterIncomingResults(incomingResults, left, right);

  if (filteredIncomingResults.empty()) {
    areAllClausesTrue = false;
    return;
  }

  clauseSynonymValuesTable.clear();
  if (groupQueryResults.empty()) {
    initializeQueryResults(filteredIncomingResults, left, right);
  } else {
    addIncomingResults(filteredIncomingResults, left, right);
  }
}

void QueryEvaluator::initializeQueryResults(
    ClauseIncomingResults incomingResults, const Param& left,
    const Param& right) {
  if (left.type == ParamType::SYNONYM && right.type == ParamType::SYNONYM) {
    queryResultsSynonyms.insert(left.value);
    queryResultsSynonyms.insert(right.value);

    for (vector<int> incomingResult : incomingResults) {
      groupQueryResults.push_back({{left.value, incomingResult.front()},
                                   {right.value, incomingResult.back()}});
      clauseSynonymValuesTable[left.value].insert(incomingResult.front());
      clauseSynonymValuesTable[right.value].insert(incomingResult.back());
    }
  } else if (left.type == ParamType::SYNONYM) {
    queryResultsSynonyms.insert(left.value);

    for (vector<int> incomingResult : incomingResults) {
      groupQueryResults.push_back({{left.value, incomingResult.front()}});
      clauseSynonymValuesTable[left.value].insert(incomingResult.front());
    }
  } else {
    queryResultsSynonyms.insert(right.value);

    for (vector<int> incomingResult : incomingResults) {
      groupQueryResults.push_back({{right.value, incomingResult.back()}});
      clauseSynonymValuesTable[right.value].insert(incomingResult.back());
    }
  }
}

ClauseIncomingResults QueryEvaluator::filterIncomingResults(
    ClauseIncomingResults incomingResults, const Param& left,
    const Param& right) {
  ClauseIncomingResults finalResults = {};

  if ((left.type == ParamType::SYNONYM) && (right.type == ParamType::SYNONYM)) {
    ClauseIncomingResults filteredResults = {};

    // if both params of a clause are the same synonym
    if (left.value == right.value) {
      for (vector<int> incomingResult : incomingResults) {
        if (incomingResult[0] == incomingResult[1]) {
          filteredResults.push_back(incomingResult);
        }
      }
    } else {
      filteredResults = incomingResults;
    }

    for (vector<int> incomingResult : filteredResults) {
      bool isLeftSynCorrectDesignEntity = checkIsCorrectDesignEntity(
          incomingResult.front(), synonymMap[left.value]);
      bool isRightSynCorrectDesignEntity = checkIsCorrectDesignEntity(
          incomingResult.back(), synonymMap[right.value]);
      if (isLeftSynCorrectDesignEntity && isRightSynCorrectDesignEntity) {
        finalResults.push_back(incomingResult);
      }
    }
    return finalResults;
  }

  if (left.type == ParamType::SYNONYM) {
    for (vector<int> incomingResult : incomingResults) {
      bool isLeftSynCorrectDesignEntity = checkIsCorrectDesignEntity(
          incomingResult.front(), synonymMap[left.value]);
      if (isLeftSynCorrectDesignEntity) {
        finalResults.push_back(incomingResult);
      }
    }
    return finalResults;
  }

  // right.type == ParamType::SYNONYM
  for (vector<int> incomingResult : incomingResults) {
    bool isRightSynCorrectDesignEntity = checkIsCorrectDesignEntity(
        incomingResult.back(), synonymMap[right.value]);
    if (isRightSynCorrectDesignEntity) {
      finalResults.push_back(incomingResult);
    }
  }
  return finalResults;
}

void QueryEvaluator::addIncomingResults(ClauseIncomingResults incomingResults,
                                        const Param& left, const Param& right) {
  bool isLeftParamSynonym = false;
  bool isRightParamSynonym = false;
  bool isLeftSynInQueryResults = false;
  bool isRightSynInQueryResults = false;

  if (left.type == ParamType::SYNONYM) {
    isLeftParamSynonym = true;
    isLeftSynInQueryResults =
        queryResultsSynonyms.find(left.value) != queryResultsSynonyms.end();

    // add new synonym to queryResultsSynonyms
    if (!isLeftSynInQueryResults) {
      queryResultsSynonyms.insert(left.value);
    }
  }
  if (right.type == ParamType::SYNONYM) {
    isRightParamSynonym = true;
    isRightSynInQueryResults =
        queryResultsSynonyms.find(right.value) != queryResultsSynonyms.end();

    // add new synonym to queryResultsSynonyms
    if (!isRightSynInQueryResults) {
      queryResultsSynonyms.insert(right.value);
    }
  }

  if (isLeftParamSynonym && isRightParamSynonym) {
    if (isLeftSynInQueryResults && isRightSynInQueryResults) {
      return filter(incomingResults, {left.value, right.value});
    } else if (isLeftSynInQueryResults || isRightSynInQueryResults) {
      return innerJoin(incomingResults, {left.value, right.value});
    } else {
      return crossProduct(incomingResults, {left.value, right.value});
    }
  }

  // continue the if block above
  if (isLeftParamSynonym) {
    if (isLeftSynInQueryResults) {
      return filter(incomingResults, {left.value});
    }

    return crossProduct(incomingResults, {left.value});

  } else if (isRightParamSynonym) {
    if (isRightSynInQueryResults) {
      return filter(incomingResults, {right.value});
    }
    return crossProduct(incomingResults, {right.value});

  } else {
    // both wild cards, do nothing, whether incomingResults is empty is alr
    // checked before calling this method
    if (incomingResults.empty()) {
      DMOprintErrMsgAndExit("[QE][addIncomingResults] shouldn't reach here");
    }
  }
}

/* Main Algos to Merge Clause Results -------------------------------------- */
void QueryEvaluator::filter(ClauseIncomingResults incomingResults,
                            vector<string> incomingResultsSynonyms) {
  vector<IntermediateQueryResult> newQueryResults = {};

  for (int i = 0; i < groupQueryResults.size(); i++) {
    IntermediateQueryResult queryResult = groupQueryResults[i];

    for (vector<int> incomingResult : incomingResults) {
      filterValidQueryResults(&newQueryResults, queryResult, incomingResult,
                              incomingResultsSynonyms);
    }
  }
  groupQueryResults = newQueryResults;
  if (groupQueryResults.empty()) {
    areAllClausesTrue = false;
    return;
  }
}

void QueryEvaluator::filterValidQueryResults(
    vector<IntermediateQueryResult>* newQueryResults,
    IntermediateQueryResult queryResult, vector<int> incomingResult,
    vector<string> incomingResultsSynonyms) {
  IntermediateQueryResult newQueryResult = queryResult;
  bool isValidQueryResult = true;

  for (int i = 0; i < incomingResult.size(); i++) {
    int value = incomingResult[i];
    string synonymName = incomingResultsSynonyms[i];

    if (queryResult[synonymName] != value) {
      isValidQueryResult = false;
      break;
    }
  }

  if (isValidQueryResult) {
    newQueryResults->push_back(newQueryResult);
    insertClauseSynonymValue(newQueryResult);
  }
}

void QueryEvaluator::innerJoin(ClauseIncomingResults incomingResults,
                               vector<string> incomingResultsSynonyms) {
  vector<IntermediateQueryResult> newQueryResults = {};
  for (int i = 0; i < groupQueryResults.size(); i++) {
    IntermediateQueryResult queryResult = groupQueryResults[i];

    for (vector<int> incomingResult : incomingResults) {
      innerJoinValidQueryResults(&newQueryResults, queryResult, incomingResult,
                                 incomingResultsSynonyms);
    }
  }

  groupQueryResults = newQueryResults;
  if (groupQueryResults.empty()) {
    areAllClausesTrue = false;
    return;
  }
}

void QueryEvaluator::innerJoinValidQueryResults(
    vector<IntermediateQueryResult>* newQueryResults,
    IntermediateQueryResult queryResult, vector<int> incomingResult,
    vector<string> incomingResultsSynonyms) {
  IntermediateQueryResult newQueryResult = queryResult;
  bool isValidQueryResult = true;

  for (int i = 0; i < incomingResult.size(); i++) {
    int value = incomingResult[i];
    string synonymName = incomingResultsSynonyms[i];

    if (queryResult.find(synonymName) != queryResult.end()) {
      if (queryResult[synonymName] != value) {
        isValidQueryResult = false;
        break;
      }
    } else {
      newQueryResult[synonymName] = value;
    }
  }

  if (isValidQueryResult) {
    newQueryResults->push_back(newQueryResult);
    insertClauseSynonymValue(newQueryResult);
  }
}

void QueryEvaluator::crossProduct(ClauseIncomingResults incomingResults,
                                  vector<string> incomingResultsSynonyms) {
  vector<IntermediateQueryResult> newQueryResults;

  for (IntermediateQueryResult queryResult : groupQueryResults) {
    for (vector<int> incomingResult : incomingResults) {
      IntermediateQueryResult newQueryResult = queryResult;

      for (int i = 0; i < incomingResult.size(); i++) {
        int value = incomingResult[i];
        string synonymName = incomingResultsSynonyms[i];
        newQueryResult[synonymName] = value;
      }

      newQueryResults.push_back(newQueryResult);
      insertClauseSynonymValue(newQueryResult);
    }
  }

  groupQueryResults = newQueryResults;
  if (groupQueryResults.empty()) {
    areAllClausesTrue = false;
    return;
  }
}

/* Query Optimization Related Methods --------------------------------------- */
SynonymCountsTable QueryEvaluator::getSynonymCounts() {
  SynonymCountsTable synonymCounts = {};
  for (auto synonymToValues : clauseSynonymValuesTable) {
    synonymCounts[synonymToValues.first] = synonymToValues.second.size();
  }
  return synonymCounts;
}

void QueryEvaluator::insertClauseSynonymValue(
    IntermediateQueryResult queryResult) {
  for (auto synonymValuePair : queryResult) {
    string synonym = synonymValuePair.first;
    if (clauseSynonymValuesTable.find(synonym) ==
        clauseSynonymValuesTable.end()) {
      clauseSynonymValuesTable[synonym] = {};
    }
    clauseSynonymValuesTable[synonym].insert(synonymValuePair.second);
  }
}

void QueryEvaluator::filterGroupResultsBySelectSynonyms(
    const vector<Synonym>& selectedSynonyms) {
  vector<IntermediateQueryResult> filteredQueryResults = {};
  for (auto queryResult : groupQueryResults) {
    IntermediateQueryResult newResult = {};
    for (auto synonym : selectedSynonyms) {
      string synonymName = synonym.name;
      if (queryResult.find(synonymName) != queryResult.end()) {
        newResult.insert({synonymName, queryResult.at(synonymName)});
      }
    }
    filteredQueryResults.push_back(newResult);
  }
  groupQueryResults = filteredQueryResults;
}

void QueryEvaluator::mergeGroupResultsIntoFinalResults() {
  if (finalQueryResults.empty()) {
    finalQueryResults = groupQueryResults;
    return;
  }

  vector<IntermediateQueryResult> newFinalQueryResults = {};
  for (auto finalRes : finalQueryResults) {
    for (auto intermediateRes : groupQueryResults) {
      IntermediateQueryResult finalCopy = finalRes;
      IntermediateQueryResult intermediateCopy = intermediateRes;
      finalCopy.merge(intermediateCopy);
      newFinalQueryResults.push_back(finalCopy);
    }
  }
  finalQueryResults = newFinalQueryResults;
}

void QueryEvaluator::filterQuerySynonymsBySelectSynonyms(
    const vector<Synonym>& selectedSynonyms) {
  unordered_set<string> filteredSynonyms = {};
  for (auto synonym : selectedSynonyms) {
    if (queryResultsSynonyms.find(synonym.name) != queryResultsSynonyms.end()) {
      filteredSynonyms.insert(synonym.name);
    }
  }
  queryResultsSynonyms = filteredSynonyms;
}

/* Helpers to Evaluate Based on Previous Clauses ----------------------- */
vector<vector<int>> QueryEvaluator::resolveBothParamsFromResultTable(
    query::SuchThatClause clause) {
  auto rsType = clause.relationshipType;
  auto left = clause.leftParam;
  auto right = clause.rightParam;
  SetOfStmtLists leftRightValuePairsSet;

  for (auto resultEntry : groupQueryResults) {
    int leftValue = resultEntry.at(left.value);
    int rightValue = resultEntry.at(right.value);

    if (pkb->isRs(rsType, leftValue, rightValue))
      leftRightValuePairsSet.insert({leftValue, rightValue});
  }

  return vector<vector<int>>{leftRightValuePairsSet.begin(),
                             leftRightValuePairsSet.end()};
}

unordered_set<int> QueryEvaluator::resolveLeftParam(
    query::SuchThatClause clause) {
  auto rsType = clause.relationshipType;
  auto left = clause.leftParam;
  unordered_set<int> leftValues;

  switch (left.type) {
    case ParamType::INTEGER_LITERAL:
      leftValues.insert(stoi(left.value));
      break;

    case ParamType::NAME_LITERAL:
      leftValues.insert(convertLeftNameLiteralToInt(rsType, left.value));
      break;

    case ParamType::SYNONYM:
      if (queryResultsSynonyms.find(left.value) != queryResultsSynonyms.end()) {
        // get from groupQueryResults
        for (auto entry : groupQueryResults) {
          leftValues.insert(entry[left.value]);
        }
        break;
      }

      // get from pkb
      // fall throught to WILDCARD case

    case ParamType::WILDCARD:
      // there's no ambiguity what syn type the wildcard would be if it were a
      // syn, can store this info in a map that has r/s type as key and syn type
      // as value
      if (rsType == RelationshipType::MODIFIES_P ||
          rsType == RelationshipType::USES_P ||
          rsType == RelationshipType::CALLS ||
          rsType == RelationshipType::CALLS_T) {
        leftValues.merge(pkb->getAllElementsAt(TableType::PROC_TABLE));
      } else {
        // get all stmts of synonym's DesignEntity
        leftValues.merge(pkb->getAllStmts(synonymMap[left.value]));
      }
      break;

    default:
      DMOprintErrMsgAndExit("RsType " + to_string(static_cast<int>(rsType)) +
                            " shouldn't have " +
                            to_string(static_cast<int>(left.type)) +
                            " as left param, param's value = " + left.value);
      break;
  }

  return leftValues;
}

vector<vector<int>> QueryEvaluator::resolveRightParamFromLeftValues(
    query::SuchThatClause clause, unordered_set<int> leftValues) {
  auto rsType = clause.relationshipType;
  auto right = clause.rightParam;
  vector<vector<int>> leftRightValuePairs;

  switch (right.type) {
    case ParamType::INTEGER_LITERAL:

      int rightValue;

      rightValue = stoi(right.value);

      for (int leftValue : leftValues) {
        if (pkb->isRs(rsType, leftValue, rightValue)) {
          leftRightValuePairs.push_back({leftValue, rightValue});
        }
      }
      break;

    case ParamType::NAME_LITERAL:

      rightValue = convertRightNameLiteralToInt(rsType, right.value);

      for (int leftValue : leftValues) {
        if (pkb->isRs(rsType, leftValue, rightValue)) {
          leftRightValuePairs.push_back({leftValue, rightValue});
        }
      }
      break;

    case ParamType::SYNONYM:
      // if synonym is alr in the groupQueryResults
      if (queryResultsSynonyms.find(right.value) !=
          queryResultsSynonyms.end()) {
        unordered_set<int> rightValues;
        for (auto entry : groupQueryResults) {
          rightValues.insert(entry[right.value]);
        }

        // cross product left values and right values
        for (int leftValue : leftValues) {
          for (int rightValue : rightValues) {
            if (pkb->isRs(rsType, leftValue, rightValue)) {
              leftRightValuePairs.push_back({leftValue, rightValue});
            }
          }
        }
      } else {
        for (int leftValue : leftValues) {
          for (int rightValue : pkb->getRight(rsType, leftValue)) {
            leftRightValuePairs.push_back({leftValue, rightValue});
          }
        }
      }

      break;

    case ParamType::WILDCARD:
      for (int leftValue : leftValues) {
        if (!pkb->getRight(rsType, leftValue).empty()) {
          leftRightValuePairs.push_back({leftValue});
        }
      }
      break;

    default:
      break;
  }

  return leftRightValuePairs;
}

int QueryEvaluator::convertLeftNameLiteralToInt(RelationshipType rsType,
                                                string pValue) {
  switch (rsType) {
    case RelationshipType::USES_P:
    case RelationshipType::MODIFIES_P:
    case RelationshipType::CALLS:
    case RelationshipType::CALLS_T:
      return pkb->getIndexOf(TableType::PROC_TABLE, pValue);

    default:
      DMOprintErrMsgAndExit(
          "RsType " + to_string(static_cast<int>(rsType)) +
          " shouldn't have NAME_LITERAL as left param, param's value = " +
          pValue);
      break;
  }
  return -1;
}

int QueryEvaluator::convertRightNameLiteralToInt(RelationshipType rsType,
                                                 string pValue) {
  switch (rsType) {
    case RelationshipType::USES_S:
    case RelationshipType::USES_P:
    case RelationshipType::MODIFIES_S:
    case RelationshipType::MODIFIES_P:
      return pkb->getIndexOf(TableType::VAR_TABLE, pValue);
    case RelationshipType::CALLS:
    case RelationshipType::CALLS_T:
      return pkb->getIndexOf(TableType::PROC_TABLE, pValue);
    default:
      DMOprintErrMsgAndExit(
          "RsType " + to_string(static_cast<int>(rsType)) +
          " shouldn't have NAME_LITERAL as right param, param's value = " +
          pValue);
      break;
  }
  return -1;
}

/* Helpers to Evaluate Based on Previous Clauses: On-Demand Rs -------------- */
vector<tuple<Param, Param, ParamPosition>>
QueryEvaluator::getResolvedParamsForOnDemandRs(const Param& left,
                                               const Param& right) {
  vector<tuple<Param, Param, ParamPosition>> newParams = {};

  if (left.type == ParamType::SYNONYM && right.type == ParamType::SYNONYM) {
    for (auto result : groupQueryResults) {
      if (result.find(left.value) != result.end() &&
          result.find(right.value) != result.end()) {
        Param newLeft = {ParamType::INTEGER_LITERAL,
                         to_string(result[left.value])};
        Param newRight = {ParamType::INTEGER_LITERAL,
                          to_string(result[right.value])};
        newParams.push_back(make_tuple(newLeft, newRight, ParamPosition::BOTH));
      }
    }

    if (newParams.empty()) {
      // try to get individual s1 || s2 results if s1 && s2 not available
      resolveLeftParamForOnDemandRs(left, right, newParams);
      resolveRightParamForOnDemandRs(left, right, newParams);
    }
  } else if (left.type == ParamType::SYNONYM) {
    resolveLeftParamForOnDemandRs(left, right, newParams);
  } else {
    resolveRightParamForOnDemandRs(left, right, newParams);
  }
  return newParams;
}

void QueryEvaluator::resolveLeftParamForOnDemandRs(
    const Param& left, const Param& right,
    vector<tuple<Param, Param, ParamPosition>>& newParams) {
  for (auto result : clauseSynonymValuesTable[left.value]) {
    auto leftLiteral = make_pair(ParamType::INTEGER_LITERAL, to_string(result));
    Param newLeft = {leftLiteral.first, leftLiteral.second};
    newParams.push_back(make_tuple(newLeft, right, ParamPosition::LEFT));
  }
}

void QueryEvaluator::resolveRightParamForOnDemandRs(
    const Param& left, const Param& right,
    vector<tuple<Param, Param, ParamPosition>>& newParams) {
  for (auto result : clauseSynonymValuesTable[right.value]) {
    auto rightLiteral =
        make_pair(ParamType::INTEGER_LITERAL, to_string(result));
    Param newRight = {rightLiteral.first, rightLiteral.second};
    newParams.push_back(make_tuple(left, newRight, ParamPosition::RIGHT));
  }
}

/* Miscellaneous Functions ---------------------------------------------- */
ClauseIncomingResults QueryEvaluator::formatRefResults(
    unordered_set<int> results) {
  ClauseIncomingResults formattedResults = {};
  for (int res : results) {
    formattedResults.push_back({res});
  }

  return formattedResults;
}

unordered_set<int> QueryEvaluator::getAllValuesOfSynonym(string synonymName) {
  DesignEntity designEntity = synonymMap.at(synonymName);
  switch (designEntity) {
    case DesignEntity::STATEMENT:
      return pkb->getAllStmts(DesignEntity::STATEMENT);
    case DesignEntity::PROG_LINE:
      return pkb->getAllStmts(DesignEntity::STATEMENT);
    case DesignEntity::READ:
      return pkb->getAllStmts(DesignEntity::READ);
    case DesignEntity::PRINT:
      return pkb->getAllStmts(DesignEntity::PRINT);
    case DesignEntity::CALL:
      return pkb->getAllStmts(DesignEntity::CALL);
    case DesignEntity::WHILE:
      return pkb->getAllStmts(DesignEntity::WHILE);
    case DesignEntity::IF:
      return pkb->getAllStmts(DesignEntity::IF);
    case DesignEntity::ASSIGN:
      return pkb->getAllStmts(DesignEntity::ASSIGN);
    case DesignEntity::VARIABLE:
      return pkb->getAllElementsAt(TableType::VAR_TABLE);
    case DesignEntity::PROCEDURE:
      return pkb->getAllElementsAt(TableType::PROC_TABLE);
    case DesignEntity::CONSTANT:
      return pkb->getAllElementsAt(TableType::CONST_TABLE);
    default:
      DMOprintErrMsgAndExit(
          "[QE][getAllValuesOfSynonyms] invalid design entity");
      return {};
  }
}

FinalQueryResults QueryEvaluator::getSelectSynonymFinalResults(
    SelectClause select) {
  FinalQueryResults finalResults = {};

  if (select.selectType == SelectType::BOOLEAN) {
    return {{TRUE_SELECT_BOOL_RESULT}};
  }

  if (select.selectType == SelectType::SYNONYMS) {
    for (auto synonym : select.selectSynonyms) {
      if (queryResultsSynonyms.find(synonym.name) ==
          queryResultsSynonyms.end()) {
        unordered_set<int> allValues = getAllValuesOfSynonym(synonym.name);
        for (int value : allValues) {
          groupQueryResults.push_back({{synonym.name, value}});
        }
        mergeGroupResultsIntoFinalResults();
        groupQueryResults.clear();
      }
    }

    for (auto result : finalQueryResults) {
      vector<int> currTupleResult = {};
      for (auto synonym : select.selectSynonyms) {
        currTupleResult.push_back(result[synonym.name]);
      }
      finalResults.push_back(currTupleResult);
    }
  }

  return finalResults;
}

bool QueryEvaluator::checkIsCorrectDesignEntity(int result,
                                                DesignEntity designEntity) {
  switch (designEntity) {
    case DesignEntity::READ:
      return pkb->isStmt(DesignEntity::READ, result);
    case DesignEntity::PRINT:
      return pkb->isStmt(DesignEntity::PRINT, result);
    case DesignEntity::CALL:
      return pkb->isStmt(DesignEntity::CALL, result);
    case DesignEntity::WHILE:
      return pkb->isStmt(DesignEntity::WHILE, result);
    case DesignEntity::IF:
      return pkb->isStmt(DesignEntity::IF, result);
    case DesignEntity::ASSIGN:
      return pkb->isStmt(DesignEntity::ASSIGN, result);
    default:
      return true;
  }
}
