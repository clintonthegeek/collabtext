name | wall_ms | work_items | work_label | source_edits | remote_applications | fragments | tombstones | visible_bytes | process_rss_kb | process_rss_delta_kb
--- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---:
1K doc, random edit | 98.519 | 200 | local edits | 200 | 0 | 450 | 204 | 922 | 33580 | absent
10K doc, random edit | 216.293 | 200 | local edits | 200 | 0 | 588 | 168 | 9878 | 34060 | absent
100K doc, random edit | 675.639 | 200 | local edits | 200 | 0 | 1495 | 181 | 99848 | 36972 | absent
1M doc, random edit | 329.147 | 100 | local edits | 100 | 0 | 1252 | 82 | 999979 | 45096 | absent
sequential (append) | 17.429 | 200 | local edits | 200 | 0 | 1200 | 0 | 100596 | 45096 | absent
random position | 555.465 | 200 | local edits | 200 | 0 | 1495 | 181 | 99848 | 45096 | absent
hotspot [5000, 5100] | 310.419 | 200 | local edits | 200 | 0 | 1322 | 273 | 99808 | 45096 | absent
insert 1 chars | 374.049 | 100 | local edits | 100 | 0 | 1200 | 0 | 100100 | 45096 | absent
insert 10 chars | 385.192 | 100 | local edits | 100 | 0 | 1199 | 0 | 101000 | 45096 | absent
insert 100 chars | 312.006 | 100 | local edits | 100 | 0 | 1200 | 0 | 110000 | 45096 | absent
insert 1000 chars | 313.737 | 100 | local edits | 100 | 0 | 1199 | 0 | 200000 | 45096 | absent
0% tombstones | 114.447 | 200 | local edits | 200 | 0 | 526 | 177 | 4938 | 45096 | absent
50% tombstones | 1767.435 | 200 | local edits | 200 | 0 | 4063 | 2824 | 2444 | 45256 | absent
90% tombstones | 2194.991 | 200 | local edits | 200 | 0 | 5169 | 4934 | 353 | 45788 | absent
3K, 50% tombstones (2250 frags) | 273.510 | 50 | local edits | 50 | 0 | 2336 | 1583 | 1476 | 45788 | absent
5K, 50% tombstones (3746 frags) | 456.099 | 50 | local edits | 50 | 0 | 3820 | 2568 | 2510 | 45788 | absent
8K, 50% tombstones (6005 frags) | 717.986 | 50 | local edits | 50 | 0 | 6076 | 4091 | 3960 | 47004 | absent
K=2 replicas, 50 edits each | 14.500 | 100 | remote applications | 100 | 100 | 128 | 120 | 14 | 45836 | absent
K=5 replicas, 50 edits each | 529.522 | 1000 | remote applications | 250 | 1000 | 368 | 330 | 84 | 45836 | absent
K=10 replicas, 50 edits each | 5138.412 | 4500 | remote applications | 500 | 4500 | 687 | 624 | 116 | 45840 | absent
K=5 hotspot [5000,5100] | 913.923 | 1000 | remote applications | 250 | 1000 | 441 | 213 | 10027 | 45840 | absent
edit phase (500 edits) | 619.666 | 500 | local edits | 500 | 0 | 1296 | 459 | 9684 | 45840 | absent
undo phase (500 undos) | 271.073 | 500 | undo operations | 500 | 0 | 1296 | 459 | 9684 | 45840 | absent
redo phase (500 redos) | 305.083 | 500 | redo operations | 500 | 0 | 1296 | 459 | 9684 | 45840 | absent
undo N=100 edits | 7.464 | 100 | undo operations | 100 | 0 | 354 | 75 | 10000 | 45840 | absent
undo N=500 edits | 261.997 | 500 | undo operations | 500 | 0 | 1296 | 359 | 10000 | 45840 | absent
undo N=1000 edits | 1391.902 | 1000 | undo operations | 1000 | 0 | 2448 | 785 | 10000 | 47216 | absent
after 500 inserts | 531.576 | 500 | local edits | 500 | 0 | 1073 | 0 | 10500 | absent | 0
after 1000 inserts | 2216.776 | 1000 | local edits | 1000 | 0 | 1997 | 0 | 11000 | absent | 0
after 1500 inserts | 4943.902 | 1500 | local edits | 1500 | 0 | 2893 | 0 | 11500 | absent | 0
after 2000 inserts | 8142.554 | 2000 | local edits | 2000 | 0 | 3759 | 0 | 12000 | absent | 0
undo 500 deletions (1929 tombstone frags) | 327.746 | 500 | undo operations | 500 | 0 | 1929 | 1100 | 900 | 47216 | absent
memory growth after 500 edits | 123.490 | 500 | local edits | 500 | 0 | 628 | 176 | 822 | absent | 0
memory growth after 1000 edits | 531.591 | 1000 | local edits | 1000 | 0 | 1253 | 324 | 1686 | absent | -2176
memory growth after 1500 edits | 1246.997 | 1500 | local edits | 1500 | 0 | 1892 | 500 | 2479 | absent | -2176
memory growth after 2000 edits | 2252.644 | 2000 | local edits | 2000 | 0 | 2515 | 660 | 3315 | absent | -2044
memory growth after 2500 edits | 3613.606 | 2500 | local edits | 2500 | 0 | 3119 | 826 | 4159 | absent | -1980
memory growth after 3000 edits | 5312.376 | 3000 | local edits | 3000 | 0 | 3776 | 1002 | 4946 | absent | -1916
memory growth after 3500 edits | 7678.743 | 3500 | local edits | 3500 | 0 | 4402 | 1170 | 5785 | absent | -1436
memory growth after 4000 edits | 10277.572 | 4000 | local edits | 4000 | 0 | 5051 | 1325 | 6678 | absent | -1384
memory growth after 4500 edits | 13582.177 | 4500 | local edits | 4500 | 0 | 5702 | 1466 | 7615 | absent | 948
memory growth after 5000 edits | 16848.125 | 5000 | local edits | 5000 | 0 | 6332 | 1631 | 8496 | absent | 2272
edits before GC request | 1231.247 | 200 | local edits | 200 | 0 | 3979 | 2500 | 2902 | 47976 | absent
edits after GC request | 1198.075 | 200 | local edits | 200 | 0 | 3979 | 2500 | 2902 | 47976 | absent
collect_garbage() request (timed) | 0.000 | 1 | collect_garbage() calls | 0 | 0 | 3979 | 2500 | 2902 | absent | absent
realistic 3-client session | 537.985 | 600 | remote applications | 300 | 600 | 565 | 235 | 952 | 47984 | absent
N=50 edits: reconnect catch-up | 6.950 | 104 | remote applications | 100 | 104 | 116 | 37 | 1198 | 47984 | absent
N=100 edits: reconnect catch-up | 37.166 | 209 | remote applications | 200 | 209 | 227 | 62 | 1565 | 47984 | absent
N=200 edits: reconnect catch-up | 146.735 | 416 | remote applications | 400 | 416 | 448 | 159 | 2070 | 47984 | absent
controlled catch-up after partition | 83.543 | 314 | remote applications | 300 | 314 | 337 | 101 | 2767 | 47988 | absent
sustained editing, no GC requests | 1580.717 | 600 | local edits | 600 | 0 | - | - | - | 45052 | absent
sustained editing, GC requests every 100 ops | 1591.901 | 600 | local edits | 600 | 0 | - | - | - | 45052 | absent
collect_garbage() requests issued | 0.000 | 30 | collect_garbage() calls | 0 | 0 | 907 | 476 | 1832 | absent | absent
