CREATE SCHEMA mx_app_name;

CREATE TABLE output (line text);

-- a hack to run command with another application name
\COPY output FROM PROGRAM 'psql postgres://postgres@localhost:57636/regression?application_name=test -c "CREATE TABLE dist_1(a int)"'
\COPY output FROM PROGRAM 'psql postgres://postgres@localhost:57636/regression?application_name=test -c "SELECT create_distributed_table(''dist_1'', ''a'');"'

-- ensure the command executed fine
SELECT count(*) FROM pg_dist_partition WHERE logicalrelid = 'dist_1'::regclass;

\COPY output FROM PROGRAM 'psql postgres://postgres@localhost:57636/regression?application_name=test -c "DROP TABLE dist_1;"'

DROP SCHEMA mx_app_name CASCADE;
