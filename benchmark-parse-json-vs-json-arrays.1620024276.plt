set terminal jpeg; set output "benchmark-parse-json-vs-json-arrays.1620024276.jpeg"

set key off
set multiplot layout 3,1
unset xtics


set ytics ("0" 0, "400" 0.4)
set yrange[0:0.6]
plot "benchmark-parse-json-vs-json-arrays.1620024276.log" u 0:1 w l title "gen 1", "" u 0:8 w l title "gen 2", "" u 0:15 w l title "gen 3"

set ytics ("0" 0, "80" 0.08, "120" 0.12)
set yrange[0:0.2]
plot "benchmark-parse-json-vs-json-arrays.1620024276.log" u 0:3 w l title "parse 1", "" u 0:10 w l title "parse 2", "" u 0:17 w l title "parse 3"

set ytics ("0" 0, "120" 0.12)
set yrange[0:0.2]
plot "benchmark-parse-json-vs-json-arrays.1620024276.log" u 0:5 w l title "walk 1", "" u 0:12 w l title "walk 2", "" u 0:19 w l title "walk 3"

unset multiplot

# -> 8 vs 12 seconds to parse json-arrays vs json-numtypes
# -> json-arrays is 50 % faster to parse

# -> json character count
# json: json size =            4983258 bytes
# json-numtypes: json size =   4419789 bytes
# json-arrays: json size =     2358814 bytes
# json-arrays-fmt: json size = 2358814 bytes

# json-arrays vs json-numtypes = 2358814 / 4419789 = 0.53
# json-arrays vs json = 2358814 / 4983258 = 0.47
# -> 100 % smaller in size

