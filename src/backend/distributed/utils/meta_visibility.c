/*
 * citus_meta_visibility.c
 *
 * Implements the functions for hiding citus objects.
 *
 * Copyright (c) Citus Data, Inc.
 */

#include "postgres.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "common/hashfn.h"
#include "distributed/citus_meta_visibility.h"
#include "distributed/commands.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/listutils.h"
#include "distributed/log_utils.h"
#include "distributed/metadata/dependency.h"
#include "distributed/metadata/distobject.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* memory context for allocating DependentObjects */
static MemoryContext DependentObjectsContext = NULL;

static Node * CreateCitusDependentObjectExpr(int pgMetaTableVarno, int pgMetaTableOid);
static List * GetFuncArgs(int pgMetaTableVarno, int pgMetaTableOid);

PG_FUNCTION_INFO_V1(is_citus_depended_object);

/*
 * is_citus_depended_object a wrapper around IsCitusDependentObject, so
 * see the details there.
 */
Datum
is_citus_depended_object(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	if (PG_ARGISNULL(0 || PG_ARGISNULL(1)))
	{
		PG_RETURN_BOOL(false);
	}

	Oid metaTableId = PG_GETARG_OID(0);
	Oid objectId = PG_GETARG_OID(1);

	if (!OidIsValid(metaTableId) || !OidIsValid(objectId))
	{
		/* we cannot continue without valid meta table oid */
		PG_RETURN_BOOL(false);
	}

	bool dependsOnCitus = false;

	DependentObjectsContext =
		AllocSetContextCreate(
			CurrentMemoryContext,
			"Dependent Objects Context",
			ALLOCSET_DEFAULT_SIZES);

	MemoryContext oldContext = MemoryContextSwitchTo(DependentObjectsContext);

	ObjectAddress objectAdress = { metaTableId, objectId, 0 };

	switch (metaTableId)
	{
		/* handle meta objects whose oids are found in pg_depend */
		case RelationRelationId:
		case ProcedureRelationId:
		case AccessMethodRelationId:
		case EventTriggerRelationId:
		case TriggerRelationId:
		case TSConfigRelationId:
		case TSTemplateRelationId:
		case TSDictionaryRelationId:
		case LanguageRelationId:
		case RewriteRelationId:
		case AttrDefaultRelationId:
		case NamespaceRelationId:
		case ConstraintRelationId:
		case TypeRelationId:
		{
			dependsOnCitus = IsCitusDependentObject(objectAdress, NULL);
			break;
		}

		/*
		 * handle meta objects whose oids are not found in pg_depend,
		 * but their relations', types', or procedures'
		 */
		case SequenceRelationId:
		case StatisticRelationId:
		case IndexRelationId:
		{
			objectAdress.classId = RelationRelationId;
			dependsOnCitus = IsCitusDependentObject(objectAdress, NULL);
			break;
		}

		case EnumRelationId:
		{
			objectAdress.classId = TypeRelationId;
			dependsOnCitus = IsCitusDependentObject(objectAdress, NULL);
			break;
		}

		case AggregateRelationId:
		{
			objectAdress.classId = ProcedureRelationId;
			dependsOnCitus = IsCitusDependentObject(objectAdress, NULL);
			break;
		}

		case AttributeRelationId:
		{
			objectAdress.classId = RelationRelationId;

			if (PG_ARGISNULL(2))
			{
				PG_RETURN_BOOL(false);
			}
			Oid typeId = PG_GETARG_OID(2);
			ObjectAddress typeObjectAdress = { TypeRelationId, typeId, 0 };

			dependsOnCitus = IsCitusDependentObject(objectAdress, NULL) ||
							 IsCitusDependentObject(typeObjectAdress, NULL);
			break;
		}

		default:
		{
			break;
		}
	}

	MemoryContextSwitchTo(oldContext);

	PG_RETURN_BOOL(dependsOnCitus);
}


/*
 * IsCitusDependentObject returns true if the given object is dependent on any citus object.
 * It looks for direct and indirect dependencies of the given object because we have
 * the objects of some metaclasses (pg_attribute, pg_constraint, pg_index, pg_aggregate,
 * pg_statistic, pg_sequence) which are not directly stored in 'pg_depend' meta table,
 * so we use current approach instead of the short one right below (only finds direct deps):
 *
 * Oid citusExtensionId = get_extension_oid("citus", false);
 * ObjectAddress extensionObjectAddress = {ExtensionRelationId, citusExtensionId, 0};
 * return IsObjectAddressOwnedByExtension(&objectAddress, &extensionObjectAddress);
 */
bool
IsCitusDependentObject(ObjectAddress objectAddress, HTAB *dependentObjects)
{
	Oid citusId = get_extension_oid("citus", false);

	if (dependentObjects == NULL)
	{
		HASHCTL info;
		uint32 hashFlags = (HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
		memset(&info, 0, sizeof(info));
		info.keysize = sizeof(Oid);
		info.hash = oid_hash;
		info.entrysize = sizeof(ObjectAddress);
		info.hcxt = DependentObjectsContext;

		dependentObjects = hash_create("dependent objects map", 256, &info, hashFlags);
	}

	bool found = false;
	hash_search(dependentObjects, &objectAddress.objectId, HASH_ENTER, &found);
	if (found)
	{
		/* previously visited object, so no need to revisit it */
		return false;
	}

	bool citusDependent = false;

	ScanKeyData key[2];
	HeapTuple depTup = NULL;

	/* iterate the actual pg_depend catalog */
	Relation depRel = table_open(DependRelationId, AccessShareLock);

	/* scan pg_depend for classid = $1 AND objid = $2 using pg_depend_depender_index */
	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectAddress.classId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(objectAddress.objectId));
	SysScanDesc depScan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2,
											 key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		Oid refClassId = pg_depend->refclassid;
		Oid refObjectId = pg_depend->refobjid;

		if (OidIsValid(refClassId) && OidIsValid(refObjectId))
		{
			/* found dependency with citus */
			if (citusId == refObjectId)
			{
				citusDependent = true;
				break;
			}

			ObjectAddress refObjectAddress = { refClassId, refObjectId, 0 };
			citusDependent = IsCitusDependentObject(refObjectAddress, dependentObjects);

			/* found dependency to citus */
			if (citusDependent)
			{
				break;
			}
		}
	}

	systable_endscan(depScan);
	relation_close(depRel, AccessShareLock);

	return citusDependent;
}


/*
 * HideCitusDependentObjectsFromPgMetaTable adds a NOT is_citus_depended_object(oid, oid, smallint) expr
 * to the security quals of meta class RTEs.
 */
bool
HideCitusDependentObjectsFromPgMetaTable(Node *node, void *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		MemoryContext queryContext = GetMemoryChunkContext(query);

		/*
		 * We process the whole rtable rather than visiting individual RangeTblEntry's
		 * in the walker, since we need to know the varno to generate the right
		 * filter.
		 */
		int varno = 0;
		RangeTblEntry *rangeTableEntry = NULL;

		foreach_ptr(rangeTableEntry, query->rtable)
		{
			varno++;

			if (rangeTableEntry->rtekind == RTE_RELATION)
			{
				/* make sure the expression is in the right memory context */
				MemoryContext originalContext = MemoryContextSwitchTo(queryContext);

				Oid metaTableOid = InvalidOid;

				/* add NOT is_citus_depended_object(oid, oid, oid) to the security quals of the RTE */
				switch (rangeTableEntry->relid)
				{
					/* pg_class */
					case RelationRelationId:
					{
						metaTableOid = RelationRelationId;
						break;
					}

					/* pg_proc */
					case ProcedureRelationId:
					{
						metaTableOid = ProcedureRelationId;
						break;
					}

					/* pg_am */
					case AccessMethodRelationId:
					{
						metaTableOid = AccessMethodRelationId;
						break;
					}

					/* pg_type */
					case TypeRelationId:
					{
						metaTableOid = TypeRelationId;
						break;
					}

					/* pg_enum */
					case EnumRelationId:
					{
						metaTableOid = EnumRelationId;
						break;
					}

					/* pg_event_trigger */
					case EventTriggerRelationId:
					{
						metaTableOid = EventTriggerRelationId;
						break;
					}

					/* pg_trigger */
					case TriggerRelationId:
					{
						metaTableOid = TriggerRelationId;
						break;
					}

					/* pg_rewrite */
					case RewriteRelationId:
					{
						metaTableOid = RewriteRelationId;
						break;
					}

					/* pg_attrdef */
					case AttrDefaultRelationId:
					{
						metaTableOid = AttrDefaultRelationId;
						break;
					}

					/* pg_constraint */
					case ConstraintRelationId:
					{
						metaTableOid = ConstraintRelationId;
						break;
					}

					/* pg_ts_config */
					case TSConfigRelationId:
					{
						metaTableOid = TSConfigRelationId;
						break;
					}

					/* pg_ts_template */
					case TSTemplateRelationId:
					{
						metaTableOid = TSTemplateRelationId;
						break;
					}

					/* pg_ts_dict */
					case TSDictionaryRelationId:
					{
						metaTableOid = TSDictionaryRelationId;
						break;
					}

					/* pg_language */
					case LanguageRelationId:
					{
						metaTableOid = LanguageRelationId;
						break;
					}

					/* pg_namespace */
					case NamespaceRelationId:
					{
						metaTableOid = NamespaceRelationId;
						break;
					}

					/* pg_sequence */
					case SequenceRelationId:
					{
						metaTableOid = SequenceRelationId;
						break;
					}

					/* pg_statistic */
					case StatisticRelationId:
					{
						metaTableOid = StatisticRelationId;
						break;
					}

					/* pg_attribute */
					case AttributeRelationId:
					{
						metaTableOid = AttributeRelationId;
						break;
					}

					/* pg_index */
					case IndexRelationId:
					{
						metaTableOid = IndexRelationId;
						break;
					}

					/* pg_aggregate */
					case AggregateRelationId:
					{
						metaTableOid = AggregateRelationId;
						break;
					}

					default:
					{
						metaTableOid = InvalidOid;
						break;
					}
				}

				if (OidIsValid(metaTableOid))
				{
					query->jointree->quals = make_and_qual(
						query->jointree->quals, CreateCitusDependentObjectExpr(varno,
																			   metaTableOid));
				}

				MemoryContextSwitchTo(originalContext);
			}
		}

		return query_tree_walker((Query *) node, HideCitusDependentObjectsFromPgMetaTable,
								 context, 0);
	}

	return expression_tree_walker(node, HideCitusDependentObjectsFromPgMetaTable,
								  context);
}


/*
 * HasPgLocksTable returns true if RTEs contain pg_locks table.
 */
bool
HasPgLocksTable(List *rtes)
{
	Oid pgLocksId = get_relname_relid("pg_locks", get_namespace_oid("pg_catalog", false));

	RangeTblEntry *rte = NULL;
	foreach_ptr(rte, rtes)
	{
		if (rte->relid == pgLocksId)
		{
			return true;
		}
	}

	return false;
}


/*
 * CreateCitusDependentObjectExpr constructs an expression of the form:
 * NOT pg_catalog.is_citus_depended_object(oid, oid, oid)
 */
static Node *
CreateCitusDependentObjectExpr(int pgMetaTableVarno, int pgMetaTableOid)
{
	/* build the call to read_intermediate_result */
	FuncExpr *funcExpr = makeNode(FuncExpr);
	funcExpr->funcid = CitusDependentObjectFuncId();
	funcExpr->funcretset = false;
	funcExpr->funcvariadic = false;
	funcExpr->funcformat = 0;
	funcExpr->funccollid = 0;
	funcExpr->inputcollid = 0;
	funcExpr->location = -1;
	funcExpr->args = GetFuncArgs(pgMetaTableVarno, pgMetaTableOid);

	BoolExpr *notExpr = makeNode(BoolExpr);
	notExpr->boolop = NOT_EXPR;
	notExpr->args = list_make1(funcExpr);
	notExpr->location = -1;

	return (Node *) notExpr;
}


/*
 * GetFuncArgs returns func arguments for pg_catalog.is_citus_depended_object
 */
static List *
GetFuncArgs(int pgMetaTableVarno, int pgMetaTableOid)
{
	Const *metaTableOidConst = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
										 ObjectIdGetDatum(pgMetaTableOid),
										 false, true);

	AttrNumber oidAttNum = (pgMetaTableOid == IndexRelationId ||
							pgMetaTableOid == EnumRelationId) ? 2 : 1;

	Var *oidVar = makeVar(pgMetaTableVarno, oidAttNum,
						  (pgMetaTableOid == AggregateRelationId) ? REGPROCOID : OIDOID,
						  -1, InvalidOid, 0);

	if (pgMetaTableOid == AttributeRelationId)
	{
		AttrNumber typOidAttNum = 3;
		Var *typeOidVar = makeVar(pgMetaTableVarno, typOidAttNum, OIDOID, -1, InvalidOid,
								  0);

		return list_make3((Node *) metaTableOidConst, (Node *) oidVar,
						  (Node *) typeOidVar);
	}
	else
	{
		Const *dummyTypeOidConst = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
											 ObjectIdGetDatum(InvalidOid),
											 false, true);

		return list_make3((Node *) metaTableOidConst, (Node *) oidVar,
						  (Node *) dummyTypeOidConst);
	}
}
