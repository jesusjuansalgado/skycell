#!/bin/bash
# The setup these results were measured on, as run (REPRODUCING.md §1-3).
# A record, not a general installer: paths are this container's.
set -euo pipefail

# build dependencies (Ubuntu 24.04)
apt-get install -y libicu-dev libreadline-dev zlib1g-dev pkg-config bison flex libhealpix-cxx-dev

# PostgreSQL 18.6 from source, --without-llvm, gcc, -O2
# (sha256 555610c24d53e4316da5b7d3fc25c279d96856d5e0e23ee308c328c5fa881d9f,
# as published next to the tarball)
mkdir -p /home/user/build && cd /home/user/build
curl -sS -O https://ftp.postgresql.org/pub/source/v18.6/postgresql-18.6.tar.bz2
tar xjf postgresql-18.6.tar.bz2
(cd postgresql-18.6 && CFLAGS=-O2 CC=gcc ./configure --prefix=/home/user/pg18 --without-llvm \
   && make -j4 world-bin && make install-world-bin)
export PATH=/home/user/pg18/bin:$PATH

# Q3C 2.0.5, pgSphere 1.5.2 (with Ubuntu's HEALPix C++), skycell v0.36, PGXS defaults
git clone --depth 1 --branch v2.0.5 https://github.com/segasai/q3c.git
git clone --depth 1 --branch 1.5.2 https://github.com/postgrespro/pgsphere.git
git clone --depth 1 --branch v0.36 https://github.com/jesusjuansalgado/skycell.git skycell-v036
(cd q3c && make -j4 && make install)
(cd pgsphere && make -j4 && make install)
(cd skycell-v036/ext && make -j4 && make install)

# cluster; server.sh starts it with REPRODUCING.md §2's settings on the command line
mkdir -p /home/user/pgdata /home/user/pglog && chown postgres /home/user/pgdata /home/user/pglog
su postgres -c "/home/user/pg18/bin/initdb -D /home/user/pgdata -U postgres --no-locale -E UTF8"
. "$(dirname "$0")/server.sh"
pg_start
createdb real36
psql -c "CREATE EXTENSION skycell" -c "CREATE EXTENSION q3c" -c "CREATE EXTENSION pg_sphere"
psql -Atc "SELECT extversion FROM pg_extension WHERE extname = 'skycell'"     # 0.36

# skycell's regression suite: all 5 pass under PostgreSQL's defaults; under
# §2's settings `parallel` differs only in its plans, since it tests parallel
# plans and §2 sets max_parallel_workers_per_gather = 0
(cd skycell-v036/ext && make installcheck)

# real DR3 positions: 19_gaia_real.sh (ESA TAP), then the loads
cd /home/user/skycell-paper/bench
GAIA_DIR=/home/user/gaia bash 19_gaia_real.sh     # re-run once: three all-sky chunks
                                                   # and the fields failed the first time
GAIA_DIR=/home/user/gaia bash 19_gaia_load.sh     # 10,000,000 / 10,000,000 / 1,593,958
psql -c "DROP TABLE gaia_realu"                    # not read by these three results
GAIA_DIR=/home/user/gaia bash 19_gaia_load_split.sh
rm /home/user/gaia/*.csv

# then, in this order (the estimator's ANALYZEs replace gaia_realc's statistics):
#   results-v036/realcone/run.sh
#   results-v036/xmreal/run.sh
#   results-v036/realcone/same-host-026/load.sh, run.sh (twice: runs 1-4, 5-8)
#   results-v036/xmreal/same-host-026.sh
#   results-v036/estimator/run.sh
