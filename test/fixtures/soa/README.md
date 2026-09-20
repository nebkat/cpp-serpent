Draft 4 Structure-of-Arrays tables. Each `.bjd` is dart-bjdata's draft 4 encoding of the `.json`
beside it (`bjdata encode`, the default layout; `records_columns.bjd` with `--column-major`),
except `offsets.bjd`, written by hand because dart-bjdata prefers a dictionary to an offset
table for every string it is given: `[${ name [$U] n U }#3`, then three records of an index and
a byte, then four `U` offsets and eight bytes of text.
