#include <Backups/BackupEntriesCollector.h>
#include <Backups/BackupEntryFromMemory.h>
#include <Backups/IBackupCoordination.h>
#include <Backups/BackupUtils.h>
#include <Databases/IDatabase.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/formatAST.h>
#include <Storages/IStorage.h>
#include <base/chrono_io.h>
#include <base/insertAtEnd.h>
#include <Common/escapeForFileName.h>
#include <boost/range/algorithm/copy.hpp>
#include <filesystem>

namespace fs = std::filesystem;


namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_COLLECT_OBJECTS_FOR_BACKUP;
    extern const int CANNOT_BACKUP_TABLE;
    extern const int TABLE_IS_DROPPED;
    extern const int LOGICAL_ERROR;
}


bool BackupEntriesCollector::TableKey::operator ==(const TableKey & right) const
{
    return (name == right.name) && (is_temporary == right.is_temporary);
}

bool BackupEntriesCollector::TableKey::operator <(const TableKey & right) const
{
    return (name < right.name) || ((name == right.name) && (is_temporary < right.is_temporary));
}

std::string_view BackupEntriesCollector::toString(Stage stage)
{
    switch (stage)
    {
        case Stage::kPreparing: return "Preparing";
        case Stage::kFindingTables: return "Finding tables";
        case Stage::kExtractingDataFromTables: return "Extracting data from tables";
        case Stage::kRunningPostTasks: return "Running post tasks";
        case Stage::kWritingBackup: return "Writing backup";
        case Stage::kError: return "Error";
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unknown backup stage: {}", static_cast<int>(stage));
}


BackupEntriesCollector::BackupEntriesCollector(
    const ASTBackupQuery::Elements & backup_query_elements_,
    const BackupSettings & backup_settings_,
    std::shared_ptr<IBackupCoordination> backup_coordination_,
    const ContextPtr & context_,
    std::chrono::seconds timeout_)
    : backup_query_elements(backup_query_elements_)
    , backup_settings(backup_settings_)
    , backup_coordination(backup_coordination_)
    , context(context_)
    , timeout(timeout_)
    , log(&Poco::Logger::get("BackupEntriesCollector"))
{
}

BackupEntriesCollector::~BackupEntriesCollector() = default;

BackupEntries BackupEntriesCollector::getBackupEntries()
{
    try
    {
        /// getBackupEntries() must not be called multiple times.
        if (current_stage != Stage::kPreparing)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Already making backup entries");

        /// Calculate the root path for collecting backup entries, it's either empty or has the format "shards/<shard_num>/replicas/<replica_num>/".
        calculateRootPathInBackup();

        /// Do renaming in the create queries according to the renaming config.
        renaming_map = makeRenamingMapFromBackupQuery(backup_query_elements);

        /// Find databases and tables which we're going to put to the backup.
        setStage(Stage::kFindingTables);
        collectDatabasesAndTablesInfo();

        /// Make backup entries for the definitions of the found databases.
        makeBackupEntriesForDatabasesDefs();

        /// Make backup entries for the definitions of the found tables.
        makeBackupEntriesForTablesDefs();

        /// Make backup entries for the data of the found tables.
        setStage(Stage::kExtractingDataFromTables);
        makeBackupEntriesForTablesData();

        /// Run all the tasks added with addPostCollectingTask().
        setStage(Stage::kRunningPostTasks);
        runPostCollectingTasks();

        /// No more backup entries or tasks are allowed after this point.
        setStage(Stage::kWritingBackup);

        return std::move(backup_entries);
    }
    catch (...)
    {
        try
        {
            setStage(Stage::kError, getCurrentExceptionMessage(false));
        }
        catch (...)
        {
        }
        throw;
    }
}

void BackupEntriesCollector::setStage(Stage new_stage, const String & error_message)
{
    if (new_stage == Stage::kError)
        LOG_ERROR(log, "{} failed with error: {}", toString(current_stage), error_message);
    else
        LOG_TRACE(log, "{}", toString(new_stage));

    current_stage = new_stage;

    if (new_stage == Stage::kError)
    {
        backup_coordination->syncStageError(backup_settings.host_id, error_message);
    }
    else
    {
        auto all_hosts
            = BackupSettings::Util::filterHostIDs(backup_settings.cluster_host_ids, backup_settings.shard_num, backup_settings.replica_num);
        backup_coordination->syncStage(backup_settings.host_id, static_cast<int>(new_stage), all_hosts, timeout);
    }
}

/// Calculates the root path for collecting backup entries,
/// it's either empty or has the format "shards/<shard_num>/replicas/<replica_num>/".
void BackupEntriesCollector::calculateRootPathInBackup()
{
    root_path_in_backup = "/";
    if (!backup_settings.host_id.empty())
    {
        auto [shard_num, replica_num]
            = BackupSettings::Util::findShardNumAndReplicaNum(backup_settings.cluster_host_ids, backup_settings.host_id);
        root_path_in_backup = root_path_in_backup / fs::path{"shards"} / std::to_string(shard_num) / "replicas" / std::to_string(replica_num);
    }
    LOG_TRACE(log, "Will use path in backup: {}", doubleQuoteString(String{root_path_in_backup}));
}

/// Finds databases and tables which we will put to the backup.
void BackupEntriesCollector::collectDatabasesAndTablesInfo()
{
    bool use_timeout = (timeout.count() >= 0);
    auto start_time = std::chrono::steady_clock::now();

    int pass = 0;
    do
    {
        database_infos.clear();
        table_infos.clear();
        consistent = true;

        /// Collect information about databases and tables specified in the BACKUP query.
        for (const auto & element : backup_query_elements)
        {
            switch (element.type)
            {
                case ASTBackupQuery::ElementType::TABLE:
                {
                    collectTableInfo({element.database_name, element.table_name}, false, element.partitions, true);
                    break;
                }

                case ASTBackupQuery::ElementType::TEMPORARY_TABLE:
                {
                    collectTableInfo({"", element.table_name}, true, element.partitions, true);
                    break;
                }

                case ASTBackupQuery::ElementType::DATABASE:
                {
                    collectDatabaseInfo(element.database_name, element.except_tables, true);
                    break;
                }

                case ASTBackupQuery::ElementType::ALL:
                {
                    collectAllDatabasesInfo(element.except_databases, element.except_tables);
                    break;
                }
            }
        }

        /// We have to check consistency of collected information to protect from the case when some table or database is
        /// renamed during this collecting making the collected information invalid.
        checkConsistency();

        /// Two passes is absolute minimum (see `previous_table_names` & `previous_database_names`).
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (!consistent && (pass >= 2) && use_timeout)
        {
            if (elapsed > timeout)
                throw Exception(
                    ErrorCodes::CANNOT_COLLECT_OBJECTS_FOR_BACKUP,
                    "Couldn't collect tables and databases to make a backup (pass #{}, elapsed {})",
                    pass,
                    to_string(elapsed));
        }

        if (pass >= 2)
            LOG_WARNING(log, "Couldn't collect tables and databases to make a backup (pass #{}, elapsed {})", pass, to_string(elapsed));
        ++pass;
    } while (!consistent);

    LOG_INFO(log, "Will backup {} databases and {} tables", database_infos.size(), table_infos.size());
}

void BackupEntriesCollector::collectTableInfo(
    const QualifiedTableName & table_name, bool is_temporary_table, const std::optional<ASTs> & partitions, bool throw_if_not_found)
{
    /// Gather information about the table.
    DatabasePtr database;
    StoragePtr storage;
    TableLockHolder table_lock;
    ASTPtr create_table_query;

    TableKey table_key{table_name, is_temporary_table};

    if (throw_if_not_found)
    {
        auto resolved_id = is_temporary_table
            ? context->resolveStorageID(StorageID{"", table_name.table}, Context::ResolveExternal)
            : context->resolveStorageID(StorageID{table_name.database, table_name.table}, Context::ResolveGlobal);
        std::tie(database, storage) = DatabaseCatalog::instance().getDatabaseAndTable(resolved_id, context);
        table_lock = storage->lockForShare(context->getInitialQueryId(), context->getSettingsRef().lock_acquire_timeout);
        create_table_query = storage->getCreateQueryForBackup(*this);
    }
    else
    {
        auto resolved_id = is_temporary_table
            ? context->tryResolveStorageID(StorageID{"", table_name.table}, Context::ResolveExternal)
            : context->tryResolveStorageID(StorageID{table_name.database, table_name.table}, Context::ResolveGlobal);
        if (!resolved_id.empty())
            std::tie(database, storage) = DatabaseCatalog::instance().tryGetDatabaseAndTable(resolved_id, context);

        if (storage)
        {
            try
            {
                table_lock = storage->lockForShare(context->getInitialQueryId(), context->getSettingsRef().lock_acquire_timeout);
                create_table_query = storage->getCreateQueryForBackup(*this);
            }
            catch (Exception & e)
            {
                if (e.code() != ErrorCodes::TABLE_IS_DROPPED)
                    throw;
            }
        }

        if (!create_table_query)
        {
            consistent &= !table_infos.contains(table_key);
            return;
        }
    }

    fs::path data_path_in_backup;
    if (is_temporary_table)
    {
        auto table_name_in_backup = renaming_map.getNewTemporaryTableName(table_name.table);
        data_path_in_backup = root_path_in_backup / "temporary_tables" / "data" / escapeForFileName(table_name_in_backup);
    }
    else
    {
        auto table_name_in_backup = renaming_map.getNewTableName(table_name);
        data_path_in_backup
            = root_path_in_backup / "data" / escapeForFileName(table_name_in_backup.database) / escapeForFileName(table_name_in_backup.table);
    }

    /// Check that information is consistent.
    const auto & create = create_table_query->as<const ASTCreateQuery &>();
    if ((create.getTable() != table_name.table) || (is_temporary_table != create.temporary) || (create.getDatabase() != table_name.database))
    {
        /// Table was renamed recently.
        consistent = false;
        return;
    }

    if (auto it = table_infos.find(table_key); it != table_infos.end())
    {
        const auto & table_info = it->second;
        if ((table_info.database != database) || (table_info.storage != storage))
        {
            /// Table was renamed recently.
            consistent = false;
            return;
        }
    }

    /// Add information to `table_infos`.
    auto & res_table_info = table_infos[table_key];
    res_table_info.database = database;
    res_table_info.storage = storage;
    res_table_info.table_lock = table_lock;
    res_table_info.create_table_query = create_table_query;
    res_table_info.data_path_in_backup = data_path_in_backup;

    if (partitions)
    {
        if (!res_table_info.partitions)
            res_table_info.partitions.emplace();
        insertAtEnd(*res_table_info.partitions, *partitions);
    }
}

void BackupEntriesCollector::collectDatabaseInfo(const String & database_name, const std::set<DatabaseAndTableName> & except_table_names, bool throw_if_not_found)
{
    /// Gather information about the database.
    DatabasePtr database;
    ASTPtr create_database_query;

    if (throw_if_not_found)
    {
        database = DatabaseCatalog::instance().getDatabase(database_name);
        create_database_query = database->getCreateDatabaseQueryForBackup();
    }
    else
    {
        database = DatabaseCatalog::instance().tryGetDatabase(database_name);
        if (!database)
        {
            consistent &= !database_infos.contains(database_name);
            return;
        }

        try
        {
            create_database_query = database->getCreateDatabaseQueryForBackup();
        }
        catch (...)
        {
            /// The database has been dropped recently.
            consistent &= !database_infos.contains(database_name);
            return;
        }
    }

    /// Check that information is consistent.
    const auto & create = create_database_query->as<const ASTCreateQuery &>();
    if (create.getDatabase() != database_name)
    {
        /// Database was renamed recently.
        consistent = false;
        return;
    }

    if (auto it = database_infos.find(database_name); it != database_infos.end())
    {
        const auto & database_info = it->second;
        if (database_info.database != database)
        {
            /// Database was renamed recently.
            consistent = false;
            return;
        }
    }

    /// Add information to `database_infos`.
    auto & res_database_info = database_infos[database_name];
    res_database_info.database = database;
    res_database_info.create_database_query = create_database_query;

    /// Add information about tables too.
    for (auto it = database->getTablesIteratorForBackup(*this); it->isValid(); it->next())
    {
        if (except_table_names.contains({database_name, it->name()}))
            continue;

        collectTableInfo({database_name, it->name()}, /* is_temporary_table= */ false, {}, /* throw_if_not_found= */ false);
        if (!consistent)
            return;
    }
}

void BackupEntriesCollector::collectAllDatabasesInfo(const std::set<String> & except_database_names, const std::set<DatabaseAndTableName> & except_table_names)
{
    for (const auto & [database_name, database] : DatabaseCatalog::instance().getDatabases())
    {
        if (except_database_names.contains(database_name))
            continue;
        collectDatabaseInfo(database_name, except_table_names, false);
        if (!consistent)
            return;
    }
}

/// Check for consistency of collected information about databases and tables.
void BackupEntriesCollector::checkConsistency()
{
    if (!consistent)
        return; /// Already inconsistent, no more checks necessary

    /// Databases found while we were scanning tables and while we were scanning databases - must be the same.
    for (const auto & [key, table_info] : table_infos)
    {
        auto it = database_infos.find(key.name.database);
        if (it != database_infos.end())
        {
            const auto & database_info = it->second;
            if (database_info.database != table_info.database)
            {
                consistent = false;
                return;
            }
        }
    }

    /// We need to scan tables at least twice to be sure that we haven't missed any table which could be renamed
    /// while we were scanning.
    std::set<String> database_names;
    std::set<TableKey> table_names;
    boost::range::copy(database_infos | boost::adaptors::map_keys, std::inserter(database_names, database_names.end()));
    boost::range::copy(table_infos | boost::adaptors::map_keys, std::inserter(table_names, table_names.end()));

    if (!previous_database_names || !previous_table_names || (*previous_database_names != database_names)
        || (*previous_table_names != table_names))
    {
        previous_database_names = std::move(database_names);
        previous_table_names = std::move(table_names);
        consistent = false;
    }
}

/// Make backup entries for all the definitions of all the databases found.
void BackupEntriesCollector::makeBackupEntriesForDatabasesDefs()
{
    for (const auto & [database_name, database_info] : database_infos)
    {
        LOG_TRACE(log, "Adding definition of database {}", backQuoteIfNeed(database_name));

        ASTPtr new_create_query = database_info.create_database_query;
        renameDatabaseAndTableNameInCreateQuery(context->getGlobalContext(), renaming_map, new_create_query);

        String new_database_name = renaming_map.getNewDatabaseName(database_name);
        auto metadata_path_in_backup = root_path_in_backup / "metadata" / (escapeForFileName(new_database_name) + ".sql");

        backup_entries.emplace_back(metadata_path_in_backup, std::make_shared<BackupEntryFromMemory>(serializeAST(*new_create_query)));
    }
}

/// Calls IDatabase::backupTable() for all the tables found to make backup entries for tables.
void BackupEntriesCollector::makeBackupEntriesForTablesDefs()
{
    for (const auto & [key, table_info] : table_infos)
    {
        LOG_TRACE(log, "Adding definition of {}table {}", (key.is_temporary ? "temporary " : ""), key.name.getFullName());

        ASTPtr new_create_query = table_info.create_table_query;
        renameDatabaseAndTableNameInCreateQuery(context->getGlobalContext(), renaming_map, new_create_query);

        fs::path metadata_path_in_backup;
        if (key.is_temporary)
        {
            auto new_name = renaming_map.getNewTemporaryTableName(key.name.table);
            metadata_path_in_backup = root_path_in_backup / "temporary_tables" / "metadata" / (escapeForFileName(new_name) + ".sql");
        }
        else
        {
            auto new_name = renaming_map.getNewTableName(key.name);
            metadata_path_in_backup
                = root_path_in_backup / "metadata" / escapeForFileName(new_name.database) / (escapeForFileName(new_name.table) + ".sql");
        }

        backup_entries.emplace_back(metadata_path_in_backup, std::make_shared<BackupEntryFromMemory>(serializeAST(*new_create_query)));
    }
}

void BackupEntriesCollector::makeBackupEntriesForTablesData()
{
    if (backup_settings.structure_only)
        return;

    for (const auto & [key, table_info] : table_infos)
    {
        LOG_TRACE(log, "Adding data of {}table {}", (key.is_temporary ? "temporary " : ""), key.name.getFullName());
        const auto & storage = table_info.storage;
        const auto & data_path_in_backup = table_info.data_path_in_backup;
        const auto & partitions = table_info.partitions;
        storage->backupData(*this, data_path_in_backup, partitions);
    }
}

void BackupEntriesCollector::addBackupEntry(const String & file_name, BackupEntryPtr backup_entry)
{
    if (current_stage == Stage::kWritingBackup)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding backup entries is not allowed");
    backup_entries.emplace_back(file_name, backup_entry);
}

void BackupEntriesCollector::addBackupEntries(const BackupEntries & backup_entries_)
{
    if (current_stage == Stage::kWritingBackup)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding backup entries is not allowed");
    insertAtEnd(backup_entries, backup_entries_);
}

void BackupEntriesCollector::addBackupEntries(BackupEntries && backup_entries_)
{
    if (current_stage == Stage::kWritingBackup)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding backup entries is not allowed");
    insertAtEnd(backup_entries, std::move(backup_entries_));
}

void BackupEntriesCollector::addPostCollectingTask(std::function<void()> task)
{
    if (current_stage == Stage::kWritingBackup)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding post tasks is not allowed");
    post_collecting_tasks.push(std::move(task));
}

/// Runs all the tasks added with addPostCollectingTask().
void BackupEntriesCollector::runPostCollectingTasks()
{
    /// Post collecting tasks can add other post collecting tasks, our code is fine with that.
    while (!post_collecting_tasks.empty())
    {
        auto task = std::move(post_collecting_tasks.front());
        post_collecting_tasks.pop();
        std::move(task)();
    }
}

void BackupEntriesCollector::throwPartitionsNotSupported(const StorageID & storage_id, const String & table_engine)
{
    throw Exception(
        ErrorCodes::CANNOT_BACKUP_TABLE,
        "Table engine {} doesn't support partitions, cannot backup table {}",
        table_engine,
        storage_id.getFullTableName());
}

}
