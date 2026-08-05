/*-------------------------------------------------------------------------
 *
 * mys_execPartition.c
 *	  Support routines for partitioning in Oracle mode
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/executor/mys_execPartition.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/partition.h"
#include "executor/executor.h"
#include "executor/mys_execPartition.h"
#include "executor/mys_nodeModifyTable.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "nodes/mysql/mys_execnodes.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"

struct PartitionTupleRouting
{
	Relation	partition_root;
	PartitionDispatch *partition_dispatch_info;
	ResultRelInfo **nonleaf_partitions;
	int			num_dispatch;
	int			max_dispatch;
	ResultRelInfo **partitions;
	bool	   *is_borrowed_rel;
	int			num_partitions;
	int			max_partitions;
	MemoryContext memcxt;
};

typedef struct PartitionDispatchData
{
	Relation	reldesc;
	PartitionKey key;
	List	   *keystate;		/* list of ExprState */
	PartitionDesc partdesc;
	TupleTableSlot *tupslot;
	AttrMap    *tupmap;
	int			indexes[FLEXIBLE_ARRAY_MEMBER];
}			PartitionDispatchData;

static ResultRelInfo *ExecInitPartitionInfo(ModifyTableState *mtstate,
											EState *estate, PartitionTupleRouting *proute,
											PartitionDispatch dispatch,
											ResultRelInfo *rootResultRelInfo,
											int partidx);
static char *ExecBuildSlotPartitionKeyDescription(Relation rel,
												  Datum *values,
												  bool *isnull,
												  int maxfieldlen);
static void FormPartitionKeyDatum(PartitionDispatch pd,
								  TupleTableSlot *slot,
								  EState *estate,
								  Datum *values,
								  bool *isnull);
static int	get_partition_for_tuple(PartitionDispatch pd, Datum *values,
									bool *isnull);
static void ExecInitRoutingInfo(ModifyTableState *mtstate,
								EState *estate,
								PartitionTupleRouting *proute,
								PartitionDispatch dispatch,
								ResultRelInfo *partRelInfo,
								int partidx,
								bool is_borrowed_rel);
static PartitionDispatch ExecInitPartitionDispatchInfo(EState *estate,
													   PartitionTupleRouting *proute,
													   Oid partoid, PartitionDispatch parent_pd,
													   int partidx, ResultRelInfo *rootResultRelInfo);
static List *adjust_partition_colnos(List *colnos, ResultRelInfo *leaf_part_rri);
static List *adjust_partition_colnos_using_map(List *colnos, AttrMap *attrMap);



/*
 * mys_ExecFindPartition
 *     Like standard ExecFindPartition
 */
ResultRelInfo *
mys_ExecFindPartition(ModifyTableState *mtstate,
					  ResultRelInfo *rootResultRelInfo,
					  PartitionTupleRouting *proute,
					  TupleTableSlot *slot,
					  EState *estate)
{
    PartitionDispatch *pd = proute->partition_dispatch_info;
	Datum		values[PARTITION_MAX_KEYS];
	bool		isnull[PARTITION_MAX_KEYS];
	Relation	rel;
	PartitionDispatch dispatch;
	PartitionDesc partdesc;
	ExprContext *ecxt = GetPerTupleExprContext(estate);
	TupleTableSlot *ecxt_scantuple_saved = ecxt->ecxt_scantuple;
	TupleTableSlot *rootslot = slot;
	TupleTableSlot *myslot = NULL;
	MemoryContext oldcxt;
	ResultRelInfo *rri = NULL;

	oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	if (rootResultRelInfo->ri_RelationDesc->rd_rel->relispartition)
		ExecPartitionCheck(rootResultRelInfo, slot, estate, true);

	dispatch = pd[0];
	while (dispatch != NULL)
	{
		int			partidx = -1;
		bool		is_leaf;

		CHECK_FOR_INTERRUPTS();

		rel = dispatch->reldesc;
		partdesc = dispatch->partdesc;

		ecxt->ecxt_scantuple = slot;
		FormPartitionKeyDatum(dispatch, slot, estate, values, isnull);

		if (partdesc->nparts == 0 ||
			(partidx = get_partition_for_tuple(dispatch, values, isnull)) < 0)
		{
			char	   *val_desc;

			val_desc = ExecBuildSlotPartitionKeyDescription(rel,
															values, isnull, 64);
			Assert(OidIsValid(RelationGetRelid(rel)));
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("no partition of relation \"%s\" found for row",
							RelationGetRelationName(rel)),
					 val_desc ?
					 errdetail("Partition key of the failing row contains %s.",
							   val_desc) : 0,
					 errtable(rel)));
		}

		is_leaf = partdesc->is_leaf[partidx];
		if (is_leaf)
		{
			if (likely(dispatch->indexes[partidx] >= 0))
			{
				Assert(dispatch->indexes[partidx] < proute->num_partitions);
				rri = proute->partitions[dispatch->indexes[partidx]];
			}
			else
			{
				rri = ExecLookupResultRelByOid(mtstate,
											   partdesc->oids[partidx],
											   true, false);
				if (rri)
				{
					CheckValidResultRel(rri, CMD_INSERT, ONCONFLICT_NONE, NIL);

					ExecInitRoutingInfo(mtstate, estate, proute, dispatch,
										rri, partidx, true);
				}
				else
				{
					rri = ExecInitPartitionInfo(mtstate, estate, proute,
												dispatch,
												rootResultRelInfo, partidx);
				}
			}
			Assert(rri != NULL);

			dispatch = NULL;
		}
		else
		{
			if (likely(dispatch->indexes[partidx] >= 0))
			{
				Assert(dispatch->indexes[partidx] < proute->num_dispatch);

				rri = proute->nonleaf_partitions[dispatch->indexes[partidx]];

				dispatch = pd[dispatch->indexes[partidx]];
			}
			else
			{
				PartitionDispatch subdispatch;

				subdispatch = ExecInitPartitionDispatchInfo(estate,
															proute,
															partdesc->oids[partidx],
															dispatch, partidx,
															mtstate->rootResultRelInfo);
				Assert(dispatch->indexes[partidx] >= 0 &&
					   dispatch->indexes[partidx] < proute->num_dispatch);

				rri = proute->nonleaf_partitions[dispatch->indexes[partidx]];
				dispatch = subdispatch;
			}

			if (dispatch->tupslot)
			{
				AttrMap    *map = dispatch->tupmap;
				TupleTableSlot *tempslot = myslot;

				myslot = dispatch->tupslot;
				slot = execute_attr_map_slot(map, slot, myslot);

				if (tempslot != NULL)
					ExecClearTuple(tempslot);
			}
		}

		if (partidx == partdesc->boundinfo->default_index)
		{
			if (is_leaf)
			{
				TupleConversionMap *map = ExecGetRootToChildMap(rri, estate);

				if (map)
					slot = execute_attr_map_slot(map->attrMap, rootslot,
												 rri->ri_PartitionTupleSlot);
				else
					slot = rootslot;
			}

			ExecPartitionCheck(rri, slot, estate, true);
		}
	}

	if (myslot != NULL)
		ExecClearTuple(myslot);

	ecxt->ecxt_scantuple = ecxt_scantuple_saved;
	MemoryContextSwitchTo(oldcxt);

	return rri;
}

/*
 * ExecInitPartitionInfo
 */
static ResultRelInfo *
ExecInitPartitionInfo(ModifyTableState *mtstate, EState *estate,
					  PartitionTupleRouting *proute,
					  PartitionDispatch dispatch,
					  ResultRelInfo *rootResultRelInfo,
					  int partidx)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	Oid			partOid = dispatch->partdesc->oids[partidx];
	Relation	partrel;
	int			firstVarno = mtstate->resultRelInfo[0].ri_RangeTableIndex;
	Relation	firstResultRel = mtstate->resultRelInfo[0].ri_RelationDesc;
	ResultRelInfo *leaf_part_rri;
	MemoryContext oldcxt;
	AttrMap    *part_attmap = NULL;
	bool		found_whole_row;

	oldcxt = MemoryContextSwitchTo(proute->memcxt);

	partrel = table_open(partOid, RowExclusiveLock);

	leaf_part_rri = makeNode(ResultRelInfo);
	InitResultRelInfo(leaf_part_rri,
					  partrel,
					  0,
					  rootResultRelInfo,
					  estate->es_instrument);

	CheckValidResultRel(leaf_part_rri, CMD_INSERT, ONCONFLICT_NONE, NIL);

	if (partrel->rd_rel->relhasindex &&
		leaf_part_rri->ri_IndexRelationDescs == NULL)
		ExecOpenIndices(leaf_part_rri,
						(node != NULL &&
						 node->onConflictAction != ONCONFLICT_NONE));

	if (node && node->withCheckOptionLists != NIL)
	{
		List	   *wcoList;
		List	   *wcoExprs = NIL;
		ListCell   *ll;

		Assert((node->operation == CMD_INSERT &&
				list_length(node->withCheckOptionLists) == 1 &&
				list_length(node->resultRelations) == 1) ||
			   (node->operation == CMD_UPDATE &&
				list_length(node->withCheckOptionLists) ==
				list_length(node->resultRelations)));

		wcoList = linitial(node->withCheckOptionLists);

		part_attmap =
			build_attrmap_by_name(RelationGetDescr(partrel),
								  RelationGetDescr(firstResultRel), false);
		wcoList = (List *)
			map_variable_attnos((Node *) wcoList,
								firstVarno, 0,
								part_attmap,
								RelationGetForm(partrel)->reltype,
								&found_whole_row);

		foreach(ll, wcoList)
		{
			WithCheckOption *wco = castNode(WithCheckOption, lfirst(ll));
			ExprState  *wcoExpr = ExecInitQual(castNode(List, wco->qual),
											   &mtstate->ps);

			wcoExprs = lappend(wcoExprs, wcoExpr);
		}

		leaf_part_rri->ri_WithCheckOptions = wcoList;
		leaf_part_rri->ri_WithCheckOptionExprs = wcoExprs;
	}

	if (node && node->returningLists != NIL)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;
		List	   *returningList;

		Assert((node->operation == CMD_INSERT &&
				list_length(node->returningLists) == 1 &&
				list_length(node->resultRelations) == 1) ||
			   (node->operation == CMD_UPDATE &&
				list_length(node->returningLists) ==
				list_length(node->resultRelations)));

		returningList = linitial(node->returningLists);

		if (part_attmap == NULL)
			part_attmap =
				build_attrmap_by_name(RelationGetDescr(partrel),
									  RelationGetDescr(firstResultRel), false);
		returningList = (List *)
			map_variable_attnos((Node *) returningList,
								firstVarno, 0,
								part_attmap,
								RelationGetForm(partrel)->reltype,
								&found_whole_row);

		leaf_part_rri->ri_returningList = returningList;

		Assert(mtstate->ps.ps_ResultTupleSlot != NULL);
		slot = mtstate->ps.ps_ResultTupleSlot;
		Assert(mtstate->ps.ps_ExprContext != NULL);
		econtext = mtstate->ps.ps_ExprContext;
		leaf_part_rri->ri_projectReturning =
			ExecBuildProjectionInfo(returningList, econtext, slot,
									&mtstate->ps, RelationGetDescr(partrel));
	}

	ExecInitRoutingInfo(mtstate, estate, proute, dispatch,
						leaf_part_rri, partidx, false);

	if (node && node->onConflictAction != ONCONFLICT_NONE)
	{
		TupleDesc	partrelDesc = RelationGetDescr(partrel);
		ExprContext *econtext = mtstate->ps.ps_ExprContext;
		ListCell   *lc;
		List	   *arbiterIndexes = NIL;

		if (list_length(rootResultRelInfo->ri_onConflictArbiterIndexes) > 0)
		{
			List	   *childIdxs;

			childIdxs = RelationGetIndexList(leaf_part_rri->ri_RelationDesc);

			foreach(lc, childIdxs)
			{
				Oid			childIdx = lfirst_oid(lc);
				List	   *ancestors;
				ListCell   *lc2;

				ancestors = get_partition_ancestors(childIdx);
				foreach(lc2, rootResultRelInfo->ri_onConflictArbiterIndexes)
				{
					if (list_member_oid(ancestors, lfirst_oid(lc2)))
						arbiterIndexes = lappend_oid(arbiterIndexes, childIdx);
				}
				list_free(ancestors);
			}
		}


		if (list_length(rootResultRelInfo->ri_onConflictArbiterIndexes) !=
			list_length(arbiterIndexes))
			elog(ERROR, "invalid arbiter index list");
		leaf_part_rri->ri_onConflictArbiterIndexes = arbiterIndexes;


		if (node->onConflictAction == ONCONFLICT_UPDATE)
		{
			OnConflictSetState *onconfl = makeNode(OnConflictSetState);
			TupleConversionMap *map;

			map = ExecGetRootToChildMap(leaf_part_rri, estate);

			Assert(node->onConflictSet != NIL);
			Assert(rootResultRelInfo->ri_onConflict != NULL);

			leaf_part_rri->ri_onConflict = onconfl;

			onconfl->oc_Existing =
				table_slot_create(leaf_part_rri->ri_RelationDesc,
								  &mtstate->ps.state->es_tupleTable);

			if (map == NULL)
			{
				onconfl->oc_ProjSlot =
					rootResultRelInfo->ri_onConflict->oc_ProjSlot;
				onconfl->oc_ProjInfo =
					rootResultRelInfo->ri_onConflict->oc_ProjInfo;
				onconfl->oc_WhereClause =
					rootResultRelInfo->ri_onConflict->oc_WhereClause;
			}
			else
			{
				List	   *onconflset;
				List	   *onconflcols;

				onconflset = copyObject(node->onConflictSet);
				if (part_attmap == NULL)
					part_attmap =
						build_attrmap_by_name(RelationGetDescr(partrel),
											  RelationGetDescr(firstResultRel), false);
				onconflset = (List *)
					map_variable_attnos((Node *) onconflset,
										INNER_VAR, 0,
										part_attmap,
										RelationGetForm(partrel)->reltype,
										&found_whole_row);
				onconflset = (List *)
					map_variable_attnos((Node *) onconflset,
										firstVarno, 0,
										part_attmap,
										RelationGetForm(partrel)->reltype,
										&found_whole_row);

				onconflcols = adjust_partition_colnos(node->onConflictCols,
													  leaf_part_rri);

				onconfl->oc_ProjSlot =
					table_slot_create(partrel,
									  &mtstate->ps.state->es_tupleTable);

				onconfl->oc_ProjInfo =
					ExecBuildUpdateProjection(onconflset,
											  true,
											  onconflcols,
											  partrelDesc,
											  econtext,
											  onconfl->oc_ProjSlot,
											  &mtstate->ps);

				if (node->onConflictWhere)
				{
					List	   *clause;

					clause = copyObject((List *) node->onConflictWhere);
					clause = (List *)
						map_variable_attnos((Node *) clause,
											INNER_VAR, 0,
											part_attmap,
											RelationGetForm(partrel)->reltype,
											&found_whole_row);
					clause = (List *)
						map_variable_attnos((Node *) clause,
											firstVarno, 0,
											part_attmap,
											RelationGetForm(partrel)->reltype,
											&found_whole_row);
					onconfl->oc_WhereClause =
						ExecInitQual((List *) clause, &mtstate->ps);
				}
			}
		}
	}

	MemoryContextSwitchTo(estate->es_query_cxt);
	estate->es_tuple_routing_result_relations =
		lappend(estate->es_tuple_routing_result_relations,
				leaf_part_rri);

    /*
	 * Initialize information about this partition that's needed to handle
	 * MERGE.  We take the "first" result relation's mergeActionList as
	 * reference and make copy for this relation, converting stuff that
	 * references attribute numbers to match this relation's.
	 *
	 * This duplicates much of the logic in ExecInitMerge(), so something
	 * changes there, look here too.
	 */
	if (node && node->operation == CMD_MERGE)
	{
		List	   *firstMergeActionList = linitial(node->mergeActionLists);
		ListCell   *lc;
		ExprContext *econtext = mtstate->ps.ps_ExprContext;

		if (part_attmap == NULL)
			part_attmap =
				build_attrmap_by_name(RelationGetDescr(partrel),
									  RelationGetDescr(firstResultRel), false);

		// if (unlikely(!leaf_part_rri->ri_projectNewInfoValid))
		// 	ExecInitMergeTupleSlots(mtstate, leaf_part_rri);

		foreach(lc, firstMergeActionList)
		{
			/* Make a copy for this relation to be safe.  */
			MergeAction *action = copyObject(lfirst(lc));
			MergeActionState *action_state;

			/* Generate the action's state for this relation */
			action_state = makeNode(MergeActionState);
			action_state->mas_action = action;

				/* PG18: matchKind replaces matched; ri_MergeActions replaces separate lists */
				leaf_part_rri->ri_MergeActions[action->matchKind] =
					lappend(leaf_part_rri->ri_MergeActions[action->matchKind],
							action_state);

			switch (action->commandType)
			{
				case CMD_INSERT:

					/*
					 * ExecCheckPlanOutput() already done on the targetlist
					 * when "first" result relation initialized and it is same
					 * for all result relations.
					 */
					action_state->mas_proj =
						ExecBuildProjectionInfo(action->targetList, econtext,
												leaf_part_rri->ri_newTupleSlot,
												&mtstate->ps,
												RelationGetDescr(partrel));
					break;
				case CMD_UPDATE:

					/*
					 * Convert updateColnos from "first" result relation
					 * attribute numbers to this result rel's.
					 */
					if (part_attmap)
						action->updateColnos =
							adjust_partition_colnos_using_map(action->updateColnos,
															  part_attmap);
					action_state->mas_proj =
						ExecBuildUpdateProjection(action->targetList,
												  true,
												  action->updateColnos,
												  RelationGetDescr(leaf_part_rri->ri_RelationDesc),
												  econtext,
												  leaf_part_rri->ri_newTupleSlot,
												  NULL);
					break;
				case CMD_DELETE:
					case CMD_NOTHING:
						/* PG18 supports DO NOTHING merge actions */
						break;

					break;

				default:
					elog(ERROR, "unknown action in MERGE WHEN clause");
			}

			/* found_whole_row intentionally ignored. */
			action->qual =
				map_variable_attnos(action->qual,
									firstVarno, 0,
									part_attmap,
									RelationGetForm(partrel)->reltype,
									&found_whole_row);
			action_state->mas_whenqual =
				ExecInitQual((List *) action->qual, &mtstate->ps);
		}
	}

	MemoryContextSwitchTo(oldcxt);

	return leaf_part_rri;
}

static void
FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull)
{
	ListCell   *partexpr_item;
	int			i;

	if (pd->key->partexprs != NIL && pd->keystate == NIL)
	{
		/* Check caller has set up context correctly */
		Assert(estate != NULL &&
			   GetPerTupleExprContext(estate)->ecxt_scantuple == slot);

		/* First time through, set up expression evaluation state */
		pd->keystate = ExecPrepareExprList(pd->key->partexprs, estate);
	}

	partexpr_item = list_head(pd->keystate);
	for (i = 0; i < pd->key->partnatts; i++)
	{
		AttrNumber	keycol = pd->key->partattrs[i];
		Datum		datum;
		bool		isNull;

		if (keycol != 0)
		{
			/* Plain column; get the value directly from the heap tuple */
			datum = slot_getattr(slot, keycol, &isNull);
		}
		else
		{
			/* Expression; need to evaluate it */
			if (partexpr_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");
			datum = ExecEvalExprSwitchContext((ExprState *) lfirst(partexpr_item),
											  GetPerTupleExprContext(estate),
											  &isNull);
			partexpr_item = lnext(pd->keystate, partexpr_item);
		}
		values[i] = datum;
		isnull[i] = isNull;
	}

	if (partexpr_item != NULL)
		elog(ERROR, "wrong number of partition key expressions");
}

static int
get_partition_for_tuple(PartitionDispatch pd, Datum *values, bool *isnull)
{
	int			bound_offset;
	int			part_index = -1;
	PartitionKey key = pd->key;
	PartitionDesc partdesc = pd->partdesc;
	PartitionBoundInfo boundinfo = partdesc->boundinfo;

	/* Route as appropriate based on partitioning strategy. */
	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			{
				uint64		rowHash;

				rowHash = compute_partition_hash_value(key->partnatts,
													   key->partsupfunc,
													   key->partcollation,
													   values, isnull);

				part_index = boundinfo->indexes[rowHash % boundinfo->nindexes];
			}
			break;

		case PARTITION_STRATEGY_LIST:
			if (isnull[0])
			{
				if (partition_bound_accepts_nulls(boundinfo))
					part_index = boundinfo->null_index;
			}
			else
			{
				bool		equal = false;

				bound_offset = partition_list_bsearch(key->partsupfunc,
													  key->partcollation,
													  boundinfo,
													  values[0], &equal);
				if (bound_offset >= 0 && equal)
					part_index = boundinfo->indexes[bound_offset];
			}
			break;

		case PARTITION_STRATEGY_RANGE:
			{
				bool		equal = false,
							range_partkey_has_null = false;
				int			i;

				/*
				 * No range includes NULL, so this will be accepted by the
				 * default partition if there is one, and otherwise rejected.
				 */
				for (i = 0; i < key->partnatts; i++)
				{
					if (isnull[i])
					{
						range_partkey_has_null = true;
						break;
					}
				}

				if (!range_partkey_has_null)
				{
					bound_offset = partition_range_datum_bsearch(key->partsupfunc,
																 key->partcollation,
																 boundinfo,
																 key->partnatts,
																 values,
																 &equal);

					/*
					 * The bound at bound_offset is less than or equal to the
					 * tuple value, so the bound at offset+1 is the upper
					 * bound of the partition we're looking for, if there
					 * actually exists one.
					 */
					part_index = boundinfo->indexes[bound_offset + 1];
				}
			}
			break;

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	/*
	 * part_index < 0 means we failed to find a partition of this parent. Use
	 * the default partition, if there is one.
	 */
	if (part_index < 0)
		part_index = boundinfo->default_index;

	return part_index;
}

static void
ExecInitRoutingInfo(ModifyTableState *mtstate,
					EState *estate,
					PartitionTupleRouting *proute,
					PartitionDispatch dispatch,
					ResultRelInfo *partRelInfo,
					int partidx,
					bool is_borrowed_rel)
{
	MemoryContext oldcxt;
	int			rri_index;

	oldcxt = MemoryContextSwitchTo(proute->memcxt);

	/*
	 * Set up a tuple conversion map to convert a tuple routed to the
	 * partition from the parent's type to the partition's.
	 */
/* PG16: map computed lazily */

	/*
	 * If a partition has a different rowtype than the root parent, initialize
	 * a slot dedicated to storing this partition's tuples.  The slot is used
	 * for various operations that are applied to tuples after routing, such
	 * as checking constraints.
	 */
	if (ExecGetRootToChildMap(partRelInfo, estate) != NULL)
	{
		Relation	partrel = partRelInfo->ri_RelationDesc;

		/*
		 * Initialize the slot itself setting its descriptor to this
		 * partition's TupleDesc; TupleDesc reference will be released at the
		 * end of the command.
		 */
		partRelInfo->ri_PartitionTupleSlot =
			table_slot_create(partrel, &estate->es_tupleTable);
	}
	else
		partRelInfo->ri_PartitionTupleSlot = NULL;

	/*
	 * If the partition is a foreign table, let the FDW init itself for
	 * routing tuples to the partition.
	 */
	if (partRelInfo->ri_FdwRoutine != NULL &&
		partRelInfo->ri_FdwRoutine->BeginForeignInsert != NULL)
		partRelInfo->ri_FdwRoutine->BeginForeignInsert(mtstate, partRelInfo);

	/*
	 * Determine if the FDW supports batch insert and determine the batch size
	 * (a FDW may support batching, but it may be disabled for the
	 * server/table or for this particular query).
	 *
	 * If the FDW does not support batching, we set the batch size to 1.
	 */
	if (mtstate->operation == CMD_INSERT &&
		partRelInfo->ri_FdwRoutine != NULL &&
		partRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize &&
		partRelInfo->ri_FdwRoutine->ExecForeignBatchInsert)
		partRelInfo->ri_BatchSize =
			partRelInfo->ri_FdwRoutine->GetForeignModifyBatchSize(partRelInfo);
	else
		partRelInfo->ri_BatchSize = 1;

	Assert(partRelInfo->ri_BatchSize >= 1);

	partRelInfo->ri_CopyMultiInsertBuffer = NULL;

	/*
	 * Keep track of it in the PartitionTupleRouting->partitions array.
	 */
	Assert(dispatch->indexes[partidx] == -1);

	rri_index = proute->num_partitions++;

	/* Allocate or enlarge the array, as needed */
	if (proute->num_partitions >= proute->max_partitions)
	{
		if (proute->max_partitions == 0)
		{
			proute->max_partitions = 8;
			proute->partitions = (ResultRelInfo **)
				palloc(sizeof(ResultRelInfo *) * proute->max_partitions);
			proute->is_borrowed_rel = (bool *)
				palloc(sizeof(bool) * proute->max_partitions);
		}
		else
		{
			proute->max_partitions *= 2;
			proute->partitions = (ResultRelInfo **)
				repalloc(proute->partitions, sizeof(ResultRelInfo *) *
						 proute->max_partitions);
			proute->is_borrowed_rel = (bool *)
				repalloc(proute->is_borrowed_rel, sizeof(bool) *
						 proute->max_partitions);
		}
	}

	proute->partitions[rri_index] = partRelInfo;
	proute->is_borrowed_rel[rri_index] = is_borrowed_rel;
	dispatch->indexes[partidx] = rri_index;

	MemoryContextSwitchTo(oldcxt);
}

static PartitionDispatch
ExecInitPartitionDispatchInfo(EState *estate,
							  PartitionTupleRouting *proute, Oid partoid,
							  PartitionDispatch parent_pd, int partidx,
							  ResultRelInfo *rootResultRelInfo)
{
	Relation	rel;
	PartitionDesc partdesc;
	PartitionDispatch pd;
	int			dispatchidx;
	MemoryContext oldcxt;

	/*
	 * For data modification, it is better that executor does not include
	 * partitions being detached, except when running in snapshot-isolation
	 * mode.  This means that a read-committed transaction immediately gets a
	 * "no partition for tuple" error when a tuple is inserted into a
	 * partition that's being detached concurrently, but a transaction in
	 * repeatable-read mode can still use such a partition.
	 */
	if (estate->es_partition_directory == NULL)
		estate->es_partition_directory =
			CreatePartitionDirectory(estate->es_query_cxt,
									 !IsolationUsesXactSnapshot());

	oldcxt = MemoryContextSwitchTo(proute->memcxt);

	/*
	 * Only sub-partitioned tables need to be locked here.  The root
	 * partitioned table will already have been locked as it's referenced in
	 * the query's rtable.
	 */
	if (partoid != RelationGetRelid(proute->partition_root))
		rel = table_open(partoid, RowExclusiveLock);
	else
		rel = proute->partition_root;
	partdesc = PartitionDirectoryLookup(estate->es_partition_directory, rel);

	pd = (PartitionDispatch) palloc(offsetof(PartitionDispatchData, indexes) +
									partdesc->nparts * sizeof(int));
	pd->reldesc = rel;
	pd->key = RelationGetPartitionKey(rel);
	pd->keystate = NIL;
	pd->partdesc = partdesc;
	if (parent_pd != NULL)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);

		/*
		 * For sub-partitioned tables where the column order differs from its
		 * direct parent partitioned table, we must store a tuple table slot
		 * initialized with its tuple descriptor and a tuple conversion map to
		 * convert a tuple from its parent's rowtype to its own.  This is to
		 * make sure that we are looking at the correct row using the correct
		 * tuple descriptor when computing its partition key for tuple
		 * routing.
		 */
		pd->tupmap = build_attrmap_by_name_if_req(RelationGetDescr(parent_pd->reldesc),
												  tupdesc, false);
		pd->tupslot = pd->tupmap ?
			MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual) : NULL;
	}
	else
	{
		/* Not required for the root partitioned table */
		pd->tupmap = NULL;
		pd->tupslot = NULL;
	}

	/*
	 * Initialize with -1 to signify that the corresponding partition's
	 * ResultRelInfo or PartitionDispatch has not been created yet.
	 */
	memset(pd->indexes, -1, sizeof(int) * partdesc->nparts);

	/* Track in PartitionTupleRouting for later use */
	dispatchidx = proute->num_dispatch++;

	/* Allocate or enlarge the array, as needed */
	if (proute->num_dispatch >= proute->max_dispatch)
	{
		if (proute->max_dispatch == 0)
		{
			proute->max_dispatch = 4;
			proute->partition_dispatch_info = (PartitionDispatch *)
				palloc(sizeof(PartitionDispatch) * proute->max_dispatch);
			proute->nonleaf_partitions = (ResultRelInfo **)
				palloc(sizeof(ResultRelInfo *) * proute->max_dispatch);
		}
		else
		{
			proute->max_dispatch *= 2;
			proute->partition_dispatch_info = (PartitionDispatch *)
				repalloc(proute->partition_dispatch_info,
						 sizeof(PartitionDispatch) * proute->max_dispatch);
			proute->nonleaf_partitions = (ResultRelInfo **)
				repalloc(proute->nonleaf_partitions,
						 sizeof(ResultRelInfo *) * proute->max_dispatch);
		}
	}
	proute->partition_dispatch_info[dispatchidx] = pd;

	/*
	 * If setting up a PartitionDispatch for a sub-partitioned table, we may
	 * also need a minimally valid ResultRelInfo for checking the partition
	 * constraint later; set that up now.
	 */
	if (parent_pd)
	{
		ResultRelInfo *rri = makeNode(ResultRelInfo);

		InitResultRelInfo(rri, rel, 0, rootResultRelInfo, 0);
		proute->nonleaf_partitions[dispatchidx] = rri;
	}
	else
		proute->nonleaf_partitions[dispatchidx] = NULL;

	/*
	 * Finally, if setting up a PartitionDispatch for a sub-partitioned table,
	 * install a downlink in the parent to allow quick descent.
	 */
	if (parent_pd)
	{
		Assert(parent_pd->indexes[partidx] == -1);
		parent_pd->indexes[partidx] = dispatchidx;
	}

	MemoryContextSwitchTo(oldcxt);

	return pd;
}

static List *
adjust_partition_colnos(List *colnos, ResultRelInfo *leaf_part_rri)
{
	TupleConversionMap *map = ExecGetChildToRootMap(leaf_part_rri);
	
    Assert(map != NULL);

	return adjust_partition_colnos_using_map(colnos, map->attrMap);
}

static List *
adjust_partition_colnos_using_map(List *colnos, AttrMap *attrMap)
{
	List	   *new_colnos = NIL;
	ListCell   *lc;

	Assert(attrMap != NULL);	/* else we shouldn't be here */

	foreach(lc, colnos)
	{
		AttrNumber	parentattrno = lfirst_int(lc);

		if (parentattrno <= 0 ||
			parentattrno > attrMap->maplen ||
			attrMap->attnums[parentattrno - 1] == 0)
			elog(ERROR, "unexpected attno %d in target column list",
				 parentattrno);
		new_colnos = lappend_int(new_colnos,
								 attrMap->attnums[parentattrno - 1]);
	}

	return new_colnos;
}

static char *
ExecBuildSlotPartitionKeyDescription(Relation rel,
									 Datum *values,
									 bool *isnull,
									 int maxfieldlen)
{
	StringInfoData buf;
	PartitionKey key = RelationGetPartitionKey(rel);
	int			partnatts = get_partition_natts(key);
	int			i;
	Oid			relid = RelationGetRelid(rel);
	AclResult	aclresult;

	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	/* If the user has table-level access, just go build the description. */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/*
		 * Step through the columns of the partition key and make sure the
		 * user has SELECT rights on all of them.
		 */
		for (i = 0; i < partnatts; i++)
		{
			AttrNumber	attnum = get_partition_col_attnum(key, i);

			/*
			 * If this partition key column is an expression, we return no
			 * detail rather than try to figure out what column(s) the
			 * expression includes and if the user has SELECT rights on them.
			 */
			if (attnum == InvalidAttrNumber ||
				pg_attribute_aclcheck(relid, attnum, GetUserId(),
									  ACL_SELECT) != ACLCHECK_OK)
				return NULL;
		}
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "(%s) = (",
					 pg_get_partkeydef_columns(relid, true));

	for (i = 0; i < partnatts; i++)
	{
		char	   *val;
		int			vallen;

		if (isnull[i])
			val = "null";
		else
		{
			Oid			foutoid;
			bool		typisvarlena;

			getTypeOutputInfo(get_partition_col_typid(key, i),
							  &foutoid, &typisvarlena);
			val = OidOutputFunctionCall(foutoid, values[i]);
		}

		if (i > 0)
			appendStringInfoString(&buf, ", ");

		/* truncate if needed */
		vallen = strlen(val);
		if (vallen <= maxfieldlen)
			appendBinaryStringInfo(&buf, val, vallen);
		else
		{
			vallen = pg_mbcliplen(val, vallen, maxfieldlen);
			appendBinaryStringInfo(&buf, val, vallen);
			appendStringInfoString(&buf, "...");
		}
	}

	appendStringInfoChar(&buf, ')');

	return buf.data;
}
