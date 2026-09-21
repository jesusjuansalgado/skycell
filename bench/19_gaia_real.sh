#!/bin/bash
OUT=/tmp/gaia
# (1) all-sky sample of REAL positions: random_index is a precomputed random
# permutation, so this is a uniform random thinning of the true point process --
# every position is a real Gaia DR3 position and the density contrast is preserved
# at every scale, unlike resampling onto an order-9 map.
for i in $(seq 0 9); do
  f="$OUT/allsky_$i.csv"; [ -s "$f" ] && continue
  lo=$((i * 1000000)); hi=$(((i+1) * 1000000))
  curl -sS --max-time 600 -G 'https://gea.esac.esa.int/tap-server/tap/sync' \
    --data-urlencode 'REQUEST=doQuery' --data-urlencode 'LANG=ADQL' --data-urlencode 'FORMAT=csv' \
    --data-urlencode 'MAXREC=4000000' \
    --data-urlencode "QUERY=SELECT source_id, ra, dec FROM gaiadr3.gaia_source WHERE random_index >= $lo AND random_index < $hi" \
    -o "$f.tmp" 2>/dev/null
  if [ -s "$f.tmp" ] && head -1 "$f.tmp" | grep -q '^source_id'; then mv "$f.tmp" "$f"; echo "allsky $i: $(($(wc -l < "$f") - 1)) rows"; fi
done
# (2) complete, full-density real fields for crowding: cluster cores, bulge, Magellanic.
#     name:ra:dec:radius_deg
for fld in "omega_cen:201.697:-47.4795:0.25" "47tuc:6.0236:-72.0814:0.25" "m4:245.8967:-26.5256:0.25" \
           "baade:18.17:-29.95:0.25" "gal_centre:266.4168:-29.0078:0.25" "lmc:80.8942:-69.7561:0.5" \
           "m13:250.4235:36.4613:0.25" "ngc104_off:8.0:-72.0:0.25"; do
  n=${fld%%:*}; r=${fld##*:}; rest=${fld#*:}; ra=${rest%%:*}; rest=${rest#*:}; dec=${rest%%:*}
  f="$OUT/field_$n.csv"; [ -s "$f" ] && continue
  curl -sS --max-time 600 -G 'https://gea.esac.esa.int/tap-server/tap/sync' \
    --data-urlencode 'REQUEST=doQuery' --data-urlencode 'LANG=ADQL' --data-urlencode 'FORMAT=csv' \
    --data-urlencode 'MAXREC=4000000' \
    --data-urlencode "QUERY=SELECT source_id, ra, dec FROM gaiadr3.gaia_source WHERE 1=CONTAINS(POINT('ICRS',ra,dec), CIRCLE('ICRS',$ra,$dec,$r))" \
    -o "$f.tmp" 2>/dev/null
  if [ -s "$f.tmp" ] && head -1 "$f.tmp" | grep -q '^source_id'; then mv "$f.tmp" "$f"; echo "field $n: $(($(wc -l < "$f") - 1)) rows"; fi
done
echo "TOTAL $(cat $OUT/*.csv 2>/dev/null | grep -vc '^source_id') rows"
echo GAIA_FETCH_DONE
