# Operation codec v1 golden bytes

These frames were assembled from the specified schema and literal hexadecimal
values, independently of the native encoder. There is no trailing newline.
Public-header tests compare actual engine output with the five operation frames;
global/gap frames cover sparse full-width values and terminal interval metadata.
No legacy format reader or migration is required.
