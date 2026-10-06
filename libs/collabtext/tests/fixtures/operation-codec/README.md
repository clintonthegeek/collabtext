# Operation codec golden bytes

Text and list operation frames are version 0002 (S03): an inserted run carries its FugueMax
placement `k` -- parent `p`, side `s` (0 left, 1 right) and right origin `r`, each identity
an empty array when absent -- instead of a fractional locator, and the split-relocation list
is gone. Global and gap frames are unchanged at 0001.


These frames were assembled from the specified schema and literal hexadecimal
values, independently of the native encoder. There is no trailing newline.
Public-header tests compare actual engine output with the five operation frames;
global/gap frames cover sparse full-width values and terminal interval metadata.
No legacy format reader or migration is required.
