/*-------------------------------------------------------------------------
 *
 * citus_meta_visibility.h
 *   Hide citus objects.
 *
 * Copyright (c) CitusDependent Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef CITUS_META_VISIBILITY_H
#define CITUS_META_VISIBILITY_H

#include "catalog/objectaddress.h"
#include "nodes/nodes.h"
#include "postgres_ext.h"
#include "utils/hsearch.h"

extern bool HideCitusDependentObjectsFromPgMetaTable(Node *node, void *context);
extern bool IsCitusDependentEnum(Oid enumId);
extern bool IsCitusDependentIndex(Oid indexId);
extern bool IsCitusDependentAggregate(Oid aggregateId);
extern bool IsCitusDependentSequence(Oid sequenceRelationId);
extern bool IsCitusDependentStatistic(Oid statisticRelationId);
extern bool IsCitusDependentAttribute(Oid attributeRelationId, short attNum);
extern bool IsCitusDependentObject(ObjectAddress objectAddress, HTAB *dependentObjects);


#endif /* CITUS_META_VISIBILITY_H */
