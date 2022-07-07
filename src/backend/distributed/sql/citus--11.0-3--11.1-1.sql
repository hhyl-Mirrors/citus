DROP FUNCTION pg_catalog.worker_create_schema(bigint,text);
DROP FUNCTION pg_catalog.worker_cleanup_job_schema_cache();
DROP FUNCTION pg_catalog.worker_fetch_foreign_file(text, text, bigint, text[], integer[]);
DROP FUNCTION pg_catalog.worker_fetch_partition_file(bigint, integer, integer, integer, text, integer);
DROP FUNCTION pg_catalog.worker_hash_partition_table(bigint, integer, text, text, oid, anyarray);
DROP FUNCTION pg_catalog.worker_merge_files_into_table(bigint, integer, text[], text[]);
DROP FUNCTION pg_catalog.worker_range_partition_table(bigint, integer, text, text, oid, anyarray);
DROP FUNCTION pg_catalog.worker_repartition_cleanup(bigint);

#include "../../columnar/sql/columnar--11.0-3--11.1-1.sql"

DROP FUNCTION pg_catalog.get_all_active_transactions(OUT datid oid, OUT process_id int, OUT initiator_node_identifier int4,
                                                     OUT worker_query BOOL, OUT transaction_number int8, OUT transaction_stamp timestamptz,
                                                     OUT global_pid int8);
#include "udfs/get_all_active_transactions/11.1-1.sql"


CREATE TYPE citus.citus_move_shard_placement_arguments AS (
	shard_id bigint,
	source_node_name text,
	source_node_port integer,
	target_node_name text,
	target_node_port integer,
	shard_transfer_mode citus.shard_transfer_mode
);
ALTER TYPE citus.citus_move_shard_placement_arguments SET SCHEMA pg_catalog;

CREATE TYPE citus.citus_job_status AS ENUM ('scheduled', 'done');
ALTER TYPE citus.citus_job_status SET SCHEMA pg_catalog;

CREATE TABLE citus.pg_dist_rebalance_jobs(
    jobid bigserial NOT NULL,
    status pg_catalog.citus_job_status default 'scheduled',
    citus_move_shard_placement pg_catalog.citus_move_shard_placement_arguments
);
--  SELECT granted to PUBLIC in upgrade script
ALTER TABLE citus.pg_dist_rebalance_jobs SET SCHEMA pg_catalog;
CREATE UNIQUE INDEX pg_dist_rebalance_jobs_jobid_index ON pg_catalog.pg_dist_rebalance_jobs using btree(jobid);
CREATE INDEX pg_dist_rebalance_jobs_status_jobid_index ON pg_catalog.pg_dist_rebalance_jobs using btree(status, jobid);
