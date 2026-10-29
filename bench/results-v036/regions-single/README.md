# Footprints of a single size with skycell 0.36 (Sect. 5.7's single-size sentence)

`bands.sql` compares skycell's default region GiST (the `float4` box key,
`skyregion_box4_gist_ops`) with pgSphere's native GiST on `scircle`, for
overlap (`&&`), on footprints that all have one size. It rebuilds the laptop
measurement of `GIST_REGION_DESIGN.md`'s "Round fifty-eight", whose scratch
tables were never committed, from that round's description:

- five bands of 25,000 circles each, of one radius per band (0.05, 0.1, 0.3,
  0.5 and 1 deg), centres uniform on the sphere, each band and each index in a
  table of its own;
- 20 fresh probe circles per band, of the band's radius;
- each probe a literal query; buffers summed over every plan node of
  `EXPLAIN (ANALYZE, BUFFERS)`.

Time is measured as paired trials, unlike that round: an untimed pass, then 5
repetitions with the two methods in an order drawn per probe and repetition.
`summarize.py` gives the paired median ratio with a 95% bootstrap interval over
probes. Two seeds (`bands_seed58.out`, `bands_seed85.out`); raw rows in
`bs_time.csv` and `bs_explain.csv`. Both methods returned the same count for
every probe. Host and versions as `../cones/` (`versions.txt`).

| seed | radius | skycell pages | pgSphere pages | skycell ms | pgSphere ms | ratio [95%] |
|---|---|---|---|---|---|---|
| 0.58 | 0.05 deg | 4 | 6 | 0.0350 | 0.0390 | 0.92 [0.90, 0.94] |
| 0.58 | 0.1 deg | 4 | 6 | 0.0360 | 0.0400 | 0.92 [0.89, 0.95] |
| 0.58 | 0.3 deg | 5 | 8 | 0.0380 | 0.0410 | 0.92 [0.88, 0.95] |
| 0.58 | 0.5 deg | 8 | 12 | 0.0420 | 0.0440 | 0.94 [0.90, 0.99] |
| 0.58 | 1.0 deg | 23 | 30.5 | 0.0500 | 0.0550 | 0.91 [0.88, 0.96] |
| 0.85 | 0.05 deg | 4 | 6 | 0.0355 | 0.0370 | 0.95 [0.93, 0.97] |
| 0.85 | 0.1 deg | 4 | 6 | 0.0355 | 0.0390 | 0.92 [0.89, 0.95] |
| 0.85 | 0.3 deg | 6 | 8 | 0.0385 | 0.0410 | 0.92 [0.90, 0.95] |
| 0.85 | 0.5 deg | 8 | 12 | 0.0390 | 0.0430 | 0.90 [0.88, 0.95] |
| 0.85 | 1.0 deg | 23 | 31 | 0.0480 | 0.0540 | 0.90 [0.86, 0.96] |

skycell reads 25–37% fewer pages (median) and takes 5–10% less time in every
band, on both seeds. The laptop round had 10–25% fewer pages and 10–20% less
time. The plans differ, as they did there: pgSphere's queries run as a Bitmap
Heap Scan, skycell's as an Index Scan, on every probe.
