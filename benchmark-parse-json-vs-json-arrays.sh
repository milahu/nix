#!/usr/bin/env bash

# node's garbage collector (and other optimizations)
# distort the benchmark result
# so we use a new node process for every run

out=benchmark-parse-json-vs-json-arrays.$(date +%s).log
echo out = $out

for i in $(seq 0 500); do
	line=""
	for f in json json-numtypes json-arrays json-arrays-fmt; do
		line+=$(node benchmark-parse-json-vs-json-arrays.js $f)"   "
	done
	echo "$line" | tee -a $out
done

echo out = $out
