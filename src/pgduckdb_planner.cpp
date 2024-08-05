#include "duckdb.hpp"

extern "C" {
#include "postgres.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "utils/syscache.h"
}

#include "pgduckdb/pgduckdb_duckdb.hpp"
#include "pgduckdb/scan/postgres_scan.hpp"
#include "pgduckdb/pgduckdb_node.hpp"
#include "pgduckdb/pgduckdb_planner.hpp"
#include "pgduckdb/pgduckdb_types.hpp"
#include "pgduckdb/pgduckdb_utils.hpp"

static PlannerInfo *
duckdb_plan_query(Query *parse, ParamListInfo boundParams) {

	PlannerGlobal *glob = makeNode(PlannerGlobal);

	glob->boundParams = boundParams;
	glob->subplans = NIL;
	glob->subroots = NIL;
	glob->rewindPlanIDs = NULL;
	glob->finalrtable = NIL;
	glob->finalrteperminfos = NIL;
	glob->finalrowmarks = NIL;
	glob->resultRelations = NIL;
	glob->appendRelations = NIL;
	glob->relationOids = NIL;
	glob->invalItems = NIL;
	glob->paramExecTypes = NIL;
	glob->lastPHId = 0;
	glob->lastRowMarkId = 0;
	glob->lastPlanNodeId = 0;
	glob->transientPlan = false;
	glob->dependsOnRole = false;

	return subquery_planner(glob, parse, NULL, false, 0.0);
}

static Plan *
duckdb_create_plan(Query *query, const char *queryString, ParamListInfo boundParams) {

	List *rtables = query->rtable;

	/* Extract required vars for table */
	int flags = PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	List *vars = list_concat(pull_var_clause((Node *)query->targetList, flags),
	                         pull_var_clause((Node *)query->jointree->quals, flags));

	PlannerInfo *queryPlannerInfo = duckdb_plan_query(query, boundParams);
	auto duckdbConnection = pgduckdb::DuckdbCreateConnection(rtables, queryPlannerInfo, vars, queryString);
	auto context = duckdbConnection->context;

	auto preparedQuery = context->Prepare(queryString);

	if (preparedQuery->HasError()) {
		elog(WARNING, "(DuckDB) %s", preparedQuery->GetError().c_str());
		return nullptr;
	}

	CustomScan *duckdbNode = makeNode(CustomScan);

	auto &preparedResultTypes = preparedQuery->GetTypes();

	for (auto i = 0; i < preparedResultTypes.size(); i++) {
		auto &column = preparedResultTypes[i];
		Oid postgresColumnOid = pgduckdb::GetPostgresDuckDBType(column);

		HeapTuple tp;
		Form_pg_type typtup;

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(postgresColumnOid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for type %u", postgresColumnOid);

		typtup = (Form_pg_type)GETSTRUCT(tp);

		Var *var = makeVar(INDEX_VAR, i + 1, postgresColumnOid, typtup->typtypmod, typtup->typcollation, 0);

		duckdbNode->custom_scan_tlist =
		    lappend(duckdbNode->custom_scan_tlist,
		            makeTargetEntry((Expr *)var, i + 1, (char *)preparedQuery->GetNames()[i].c_str(), false));

		ReleaseSysCache(tp);
	}

	duckdbNode->custom_private = list_make2(duckdbConnection.release(), preparedQuery.release());
	duckdbNode->methods = &duckdb_scan_scan_methods;

	return (Plan *)duckdbNode;
}

PlannedStmt *
duckdb_plan_node(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams) {
	/* We need to check can we DuckDB create plan */
	Plan *duckdbPlan = (Plan *)castNode(CustomScan, duckdb_create_plan(parse, query_string, boundParams));

	if (!duckdbPlan) {
		return nullptr;
	}

	/* build the PlannedStmt result */
	PlannedStmt *result = makeNode(PlannedStmt);

	result->commandType = parse->commandType;
	result->queryId = parse->queryId;
	result->hasReturning = (parse->returningList != NIL);
	result->hasModifyingCTE = parse->hasModifyingCTE;
	result->canSetTag = parse->canSetTag;
	result->transientPlan = false;
	result->dependsOnRole = false;
	result->parallelModeNeeded = false;
	result->planTree = duckdbPlan;
	result->rtable = NULL;
	result->permInfos = NULL;
	result->resultRelations = NULL;
	result->appendRelations = NULL;
	result->subplans = NIL;
	result->rewindPlanIDs = NULL;
	result->rowMarks = NIL;
	result->relationOids = NIL;
	result->invalItems = NIL;
	result->paramExecTypes = NIL;

	/* utilityStmt should be null, but we might as well copy it */
	result->utilityStmt = parse->utilityStmt;
	result->stmt_location = parse->stmt_location;
	result->stmt_len = parse->stmt_len;

	return result;
}