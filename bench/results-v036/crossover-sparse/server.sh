# source me: the PostgreSQL 18.6 build, REPRODUCING.md §2 settings on the command line
export PATH=/home/user/pg18/bin:$PATH PGHOST=/tmp PGPORT=5519 PGUSER=postgres PGDATABASE=real36
export PGDATA=/home/user/pgdata
PGOPTS="-p 5519 -k /tmp -c shared_buffers=2GB -c effective_cache_size=5GB -c random_page_cost=1.1 -c seq_page_cost=1 -c work_mem=64MB -c maintenance_work_mem=1GB -c max_parallel_workers_per_gather=0 -c jit=off"
pg_start() { su postgres -c "/home/user/pg18/bin/pg_ctl -D $PGDATA -l /home/user/pglog/server.log -w -o '$PGOPTS' start" >/dev/null; }
pg_stop()  { su postgres -c "/home/user/pg18/bin/pg_ctl -D $PGDATA -m fast -w stop" >/dev/null; }
export RESTART="pg_stop; pg_start"
export DROP_CACHES="sync; echo 3 > /proc/sys/vm/drop_caches"
export -f pg_start pg_stop; export PGOPTS
