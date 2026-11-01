// SPDX-License-Identifier: MIT
#pragma once
// Reads Aurora's content database (Data/Databases/content.db, an SQLite file
// fetched over FTP) to tell which tile is which Aurora row, and so which
// GameData folder (<Title ID>_<row id>, both as 8 hex digits) holds its art.
// Only what that needs: one table's rows, read straight from the file's
// b-tree pages, with no SQLite library and no journal.
#include <string>
#include <vector>

namespace nia {
struct SqlValue { bool null; long long number; std::string text; };

// The rows of table in an SQLite database file, each with one value per name
// in columns, in that order (an INTEGER PRIMARY KEY column gives the row id;
// a column the table hasn't got is null). False, with why in error, if the
// file isn't one this can read.
bool ReadSqlTable(const std::vector<unsigned char>& file, const char* table, const std::vector<std::string>& columns,
                  std::vector<std::vector<SqlValue> >& rows, std::string& error);

// One row of Aurora's ContentItems table.
struct AuroraItem { long long id; std::string directory; unsigned titleId; std::string titleName; };
bool ReadAuroraItems(const std::vector<unsigned char>& file, std::vector<AuroraItem>& items, std::string& error);
}
