// SPDX-License-Identifier: MIT
#pragma once
// Reads Aurora's databases (Data/Databases/content.db and settings.db, SQLite
// files fetched over FTP, never written): which tile is which Aurora row, and
// so which GameData folder (<Title ID>_<row id>, both as 8 hex digits) holds
// its art; and the title updates Aurora has for each game, and which of them
// it applies. Only what that needs: one table's rows, read straight from the
// file's b-tree pages, with no SQLite library and no journal.
#include <string>
#include <vector>

namespace nia {
struct SqlValue { bool null; long long number; std::string text; };

// The rows of table in an SQLite database file, each with one value per name
// in columns, in that order (an INTEGER PRIMARY KEY column gives the row id,
// as does an INTEGER column the table's own PRIMARY KEY (column) names; a
// column the table hasn't got is null). False, with why in error, if the
// file isn't one this can read.
bool ReadSqlTable(const std::vector<unsigned char>& file, const char* table, const std::vector<std::string>& columns,
                  std::vector<std::vector<SqlValue> >& rows, std::string& error);

// One row of Aurora's ContentItems table.
struct AuroraItem { long long id; std::string directory; unsigned titleId; std::string titleName; };
bool ReadAuroraItems(const std::vector<unsigned char>& file, std::vector<AuroraItem>& items, std::string& error);

// One of the title updates Aurora has for a game (content.db's TitleUpdates),
// and whether it's the one Aurora applies to it (settings.db's
// ActiveTitleUpdates). The IDs and versions are DWORDs, which Aurora keeps as
// signed numbers; version is the update's number (XboxUnity's: 8 for Title
// Update 8). name is Aurora's (DisplayName), file the update's file name.
struct TitleUpdate { long long id; unsigned titleId, mediaId, baseVersion, version; std::string name, file; bool active; };
bool ReadTitleUpdates(const std::vector<unsigned char>& contentDb, std::vector<TitleUpdate>& updates, std::string& error);
// The ids (TitleUpdates' Id) of those in use.
bool ReadActiveTitleUpdates(const std::vector<unsigned char>& settingsDb, std::vector<long long>& ids, std::string& error);
}
