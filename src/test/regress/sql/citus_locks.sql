-- store the current gpid in a variable
SELECT global_pid AS coordinator_gpid
FROM get_all_active_transactions()
WHERE process_id = pg_backend_pid() \gset

-- list the locks on relations for current distributed transaction
SELECT relation::regclass, nodeid, mode, granted
FROM citus_locks
WHERE global_pid = :coordinator_gpid AND locktype = 'relation'
ORDER BY 1, 2, 3, 4;
