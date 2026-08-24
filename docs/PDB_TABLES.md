# The export.pdb table list, by name

`export.pdb` is a DeviceSQL database with exactly 20 tables, and its page header
carries one pointer per table at `raw type == index`, 0..19. Public readers
(crate-digger, rekordcrate) name eight of them and leave the rest as
`unknown9`, `unknown10`, `unknown14`, `unknown15`, `unknown17`, `unknown18`.

All twenty are named in the rekordbox 7 binary. DeviceSQL compiles its schema to
C, and each table gets an `__epl_<TABLE>_create` / `__epl_<TABLE>_opencfun` pair
that survives in the symbol table:

    nm -n rekordbox | grep -oE '__epl_DJDB[A-Z]+_create'

Those emit in reverse schema order, so reversing the address-sorted list
reconstructs the declaration order, which is the table index.

| type | DeviceSQL table | public name | what we write |
|---|---|---|---|
| 0  | `DJDBCONTENT`            | tracks            | our tracks |
| 1  | `DJDBGENRE`              | genres            | interned |
| 2  | `DJDBARTIST`             | artists           | interned |
| 3  | `DJDBALBUM`              | albums            | interned |
| 4  | `DJDBLABEL`              | labels            | interned |
| 5  | `DJDBKEY`                | keys              | interned |
| 6  | `DJDBCOLOR`              | colors            | static (8 rows) |
| 7  | `DJDBPLAYLIST`           | playlist_tree     | our playlists |
| 8  | `DJDBSONGPLAYLIST`       | playlist_entries  | our playlist entries |
| 9  | `DJDBHOTCUEBANKLIST`     | *unknown9*        | empty |
| 10 | `DJDBSONGHOTCUEBANKLIST` | *unknown10*       | empty |
| 11 | `DJDBHISTORY`            | history_playlists | empty |
| 12 | `DJDBSONGHISTORY`        | history_entries   | empty |
| 13 | `DJDBIMAGE`              | artwork           | empty |
| 14 | `DJDBMIXERPARAM`         | *unknown14*       | empty |
| 15 | `DJDBHOTCUEBANKPOINT`    | *unknown15*       | empty |
| 16 | `DJDBMENUITEMS`          | columns           | static (26 rows) |
| 17 | `DJDBCATEGORY`           | *unknown17*       | static (20 rows) |
| 18 | `DJDBSORT`               | *unknown18*       | static (17 rows) |
| 19 | `DJDBPROPERTY`           | history           | static (1 row) |

A 21st table, `DJDBTRACKCUE`, is declared in the schema but is not exported: a
CDJ-validated real drive carries 20 table pointers, types 0..19, and no more.

## Corrections this forces

* **Type 19 is not "history".** It is `DJDBPROPERTY`, database properties. Our
  one static row there is 40 bytes holding a date (`2014-10-19`) and `1000`,
  which reads as a creation stamp and a schema version — and not at all as a
  history playlist. The public name is wrong; the history tables are 11 and 12.
* **16/17/18 are one mechanism, not three mysteries.** `MENUITEMS` holds the
  browse-menu labels (`DEFAULT`, `ALPHABET`, `PLAYLIST`, `HOT CUE BANK`, ...),
  and `CATEGORY` and `SORT` are id-keyed rows referencing them — which browse
  categories exist and which sort options exist. The CDJ's browse UI is
  data-driven out of the database, which is why emptying these breaks browsing
  while the track rows stay perfectly readable.
* **Corroboration.** rekordbox's own master-database schema lists
  `DJMDMENUITEMS`, `DJMDCATEGORY`, `DJMDSORT` consecutively in that order, from
  an independent string table, matching 16/17/18.

## Still open

Per-table *column* lists. The column-name strings are all present
(`MENUITEMID`, `CONTENTID`, `PARENTID`, `ATTRIBUTE`, `CONDITION`, `INFOORDER`,
`SEARCHSTR`, ...) but pooled rather than laid out per table, so mapping fields
to offsets needs the `_create` bodies. We do not need them today: 6/16/17/18/19
are copied verbatim from a real export and are byte-identical across every
export examined.

`DJDBMIXERPARAM` (14) is worth a look later — it is per-track mixer data (gain
and peak), which is plausibly what a player uses for auto-gain. We leave it
empty and playback is fine, so it is a feature, not a gap.
