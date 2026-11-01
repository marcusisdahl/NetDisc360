# data

Data that NetDisc360 builds in, kept here as its source.

## systemlink.json

The games that can be played over System Link, with how many players fit on
one console and in all, for Xbox 360 and original Xbox games. The dashboard's
System Link list and the LAN party page show it. `tools/make_systemlink.py`
turns it into `server/systemlink.cpp`; run it after changing the JSON (CI
runs it with `--check`).

### Where it comes from

The game lists and player counts come from the English Wikipedia articles
[List of Xbox 360 System Link games](https://en.wikipedia.org/wiki/List_of_Xbox_360_System_Link_games)
and [List of Xbox System Link games](https://en.wikipedia.org/wiki/List_of_Xbox_System_Link_games),
by Wikipedia contributors, under the
[Creative Commons Attribution-ShareAlike 4.0 License](https://creativecommons.org/licenses/by-sa/4.0/)
(CC BY-SA 4.0). The JSON's `sources` list names every source used and how it
was reached (some Wikipedia tables through scrapes of them that other
projects published). It also names these, for the other fields:

- **Title IDs:** IronRingX/xbox360-gamelist (Xbox 360, from disc dumps),
  xenia-manager/database (Xbox 360), jeltaqq/Xbox-Original-GameList (original
  Xbox, from XBE headers).
- **Cross-checks and notes:** the xenia-manager database's System Link status
  (an emulator's, so only a hint for real consoles), ShawMerlin's Aurora
  SystemLink filter (Title IDs only), the TeamXLink wiki and Halopedia
  (CC BY-SA 3.0). Each game's `source` and `notes` say which were used for it.

A Title ID is a fact about a game, not anyone's text; the player counts and
notes are Wikipedia's and carry its licence.

### Changes made

From the Wikipedia tables: reformatted to JSON, the player counts parsed
into numbers (`null` where the table gives none), Title IDs and notes added
from the sources above, and the games checked and found not to support
System Link listed apart (`checkedNotSystemLink`). For the built-in table,
`make_systemlink.py` keeps only the games with a Title ID and makes a game
that is listed twice under one Title ID one row, its names joined and each
count the higher one known.

### Licence

`systemlink.json`, and `server/systemlink.cpp` made from it, are under
CC BY-SA 4.0, not the MIT licence of the rest of NetDisc360. Share changes
to them under the same licence, and keep the attribution above (the
dashboard shows it under the System Link list).
