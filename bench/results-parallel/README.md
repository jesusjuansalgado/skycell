# Parallel cross-matching

The paper's table `tab:parallel` (Sect. "Parallel cross-matching") and the
A/B behind skycell PR #43, which made the custom scan parallel safe.

## Host and server

- The same type of cloud machine as `../results-pg18`: Firecracker/KVM
  micro-VM, x86-64, 4 vCPU Intel Xeon @ 2.80 GHz, 15 GiB, Linux 6.18.
- PostgreSQL 18.6 built from source.
- **Default settings except `shared_buffers = 2GB`**. Parallelism and JIT are
  on, and `random_page_cost` is 4. This is unlike the rest of the paper, which
  turns parallelism and JIT off (`../../REPRODUCING.md`, §2).
- Everything is cached: 326 MB per heap, 2 GB of buffers. All runs are warm.

## Data

`../32_parallel_data.sql`:

- 5M rows, 70% uniform and 30% in 200 Gaussian clusters, seeded.
- The heap is stored at random (`cat_rand`) and in cell order (`cat_clus`),
  each with a B-tree on the cell column.
- 1M targets on catalogue sources, displaced by up to 0.3″ (50 MB), and a 200k
  subset (10 MB).

## Files

- `scale.tsv`, from `../32_parallel_xmatch.sh`: the paper's table. One library
  (the parallel-safe one), 1M targets, 1″ and 30″, both heaps. Serial, against
  PostgreSQL's default two workers, against three, set as the probe table's
  `parallel_workers`. The columns hold the median of three warm runs, the
  workers launched, and `count|sum(id)`, which is identical across rows of a
  configuration.
- `ab.tsv`, from `ab_driver.sh`: 48 runs, 2 heaps × 2 target counts × 3 radii
  (1″, 5″, 30″). Each runs skycell v0.35 (old) and the parallel-safe library
  (new) at `max_parallel_workers_per_gather` 0, 2 and 4, in alternating order.
  The plan column gives the scan mode and the workers planned. v0.35 never runs
  a cross-match in parallel. The new library's plans get the workers the
  probe table's size allows: 1 for 200k targets, 2 for 1M, whatever the
  setting above that. Results agree in every configuration.

Q3C and pgSphere were not measured in parallel.
