#pragma once
// Whole-install backup (.opcanbackup) to move everything to another opcan install (there is no account or server).
// The file is an SQLite database: a copy of opcan.db (VACUUM INTO) plus the data dir's own files (avatars, user
// piece sets, user sound sets) as rows of backup_files(path TEXT PRIMARY KEY, data BLOB). Never the AI coach
// models or the endgame tablebases: large and downloadable again.
#include <string>

// "" or an error.
std::string exportBackup(const std::string& dbPath, const std::string& dataDir, const std::string& outPath);
// "" when the file can be restored (an opcan backup whose schema isn't newer than this opcan's), else why.
// info (optional): a one-line summary for the confirmation, e.g. "3 profiles, 1,204 games, made 2026-11-03".
std::string inspectBackup(const std::string& path, std::string* info = nullptr);
// Replaces dbPath (removing its -wal/-shm) with the backup's database, minus backup_files, and writes those files
// into dataDir. The database must not be open. The old database is kept as <dbPath>.before-restore.
// "" or an error (then nothing was replaced).
std::string restoreBackup(const std::string& backupPath, const std::string& dbPath, const std::string& dataDir);
