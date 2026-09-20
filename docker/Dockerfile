# PostgreSQL 18 + Q3C + pgSphere (from the PGDG apt repo) + toolchain to build skycell
FROM postgres:18
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential postgresql-server-dev-18 \
      postgresql-18-q3c postgresql-18-pgsphere \
 && rm -rf /var/lib/apt/lists/*
