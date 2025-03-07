#include <Core/Defines.h>

#include "Common/hex.h"
#include <Common/Macros.h>
#include <Common/StringUtils/StringUtils.h>
#include <Common/ThreadPool.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/ZooKeeper/Types.h>
#include <Common/escapeForFileName.h>
#include <Common/formatReadable.h>
#include <Common/thread_local_rng.h>
#include <Common/typeid_cast.h>

#include <Storages/AlterCommands.h>
#include <Storages/PartitionCommands.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeList.h>
#include <Storages/MergeTree/MergeTreeBackgroundExecutor.h>
#include <Storages/MergeTree/MergedBlockOutputStream.h>
#include <Storages/MergeTree/PinnedPartUUIDs.h>
#include <Storages/MergeTree/PartitionPruner.h>
#include <Storages/MergeTree/ReplicatedMergeTreeTableMetadata.h>
#include <Storages/MergeTree/ReplicatedMergeTreeSink.h>
#include <Storages/MergeTree/ReplicatedMergeTreeQuorumEntry.h>
#include <Storages/MergeTree/ReplicatedMergeTreeMutationEntry.h>
#include <Storages/MergeTree/ReplicatedMergeTreeAddress.h>
#include <Storages/MergeTree/ReplicatedMergeTreeQuorumAddedParts.h>
#include <Storages/MergeTree/ReplicatedMergeTreePartHeader.h>
#include <Storages/MergeTree/MergeFromLogEntryTask.h>
#include <Storages/MergeTree/MutateFromLogEntryTask.h>
#include <Storages/VirtualColumnUtils.h>
#include <Storages/MergeTree/MergeTreeReaderCompact.h>


#include <Databases/IDatabase.h>
#include <Databases/DatabaseOnDisk.h>

#include <Parsers/formatAST.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTOptimizeQuery.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/queryToString.h>
#include <Parsers/ASTCheckQuery.h>
#include <Parsers/ASTSetQuery.h>

#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/BuildQueryPipelineSettings.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>

#include <IO/ReadBufferFromString.h>
#include <IO/Operators.h>
#include <IO/ConnectionTimeouts.h>
#include <IO/ConnectionTimeoutsContext.h>

#include <Interpreters/InterpreterAlterQuery.h>
#include <Interpreters/PartLog.h>
#include <Interpreters/Context.h>
#include <Interpreters/DDLTask.h>
#include <Interpreters/InterserverCredentials.h>

#include <DataStreams/copyData.h>

#include <Poco/DirectoryIterator.h>

#include <base/range.h>
#include <base/scope_guard.h>
#include <base/scope_guard_safe.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <numeric>
#include <thread>
#include <future>

#include <boost/algorithm/string/join.hpp>

namespace fs = std::filesystem;

namespace ProfileEvents
{
    extern const Event ReplicatedPartMerges;
    extern const Event ReplicatedPartMutations;
    extern const Event ReplicatedPartFailedFetches;
    extern const Event ReplicatedPartFetchesOfMerged;
    extern const Event ObsoleteReplicatedParts;
    extern const Event ReplicatedPartFetches;
    extern const Event DataAfterMergeDiffersFromReplica;
    extern const Event DataAfterMutationDiffersFromReplica;
    extern const Event CreatedLogEntryForMerge;
    extern const Event NotCreatedLogEntryForMerge;
    extern const Event CreatedLogEntryForMutation;
    extern const Event NotCreatedLogEntryForMutation;
}

namespace CurrentMetrics
{
    extern const Metric BackgroundFetchesPoolTask;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_READ_ALL_DATA;
    extern const int NOT_IMPLEMENTED;
    extern const int NO_ZOOKEEPER;
    extern const int INCORRECT_DATA;
    extern const int INCOMPATIBLE_COLUMNS;
    extern const int REPLICA_IS_ALREADY_EXIST;
    extern const int NO_REPLICA_HAS_PART;
    extern const int LOGICAL_ERROR;
    extern const int TOO_MANY_UNEXPECTED_DATA_PARTS;
    extern const int ABORTED;
    extern const int REPLICA_IS_NOT_IN_QUORUM;
    extern const int TABLE_IS_READ_ONLY;
    extern const int NOT_FOUND_NODE;
    extern const int NO_ACTIVE_REPLICAS;
    extern const int NOT_A_LEADER;
    extern const int TABLE_WAS_NOT_DROPPED;
    extern const int PARTITION_ALREADY_EXISTS;
    extern const int TOO_MANY_RETRIES_TO_FETCH_PARTS;
    extern const int RECEIVED_ERROR_FROM_REMOTE_IO_SERVER;
    extern const int PARTITION_DOESNT_EXIST;
    extern const int UNFINISHED;
    extern const int RECEIVED_ERROR_TOO_MANY_REQUESTS;
    extern const int PART_IS_TEMPORARILY_LOCKED;
    extern const int CANNOT_ASSIGN_OPTIMIZE;
    extern const int KEEPER_EXCEPTION;
    extern const int ALL_REPLICAS_LOST;
    extern const int REPLICA_STATUS_CHANGED;
    extern const int CANNOT_ASSIGN_ALTER;
    extern const int DIRECTORY_ALREADY_EXISTS;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int UNKNOWN_POLICY;
    extern const int NO_SUCH_DATA_PART;
    extern const int INTERSERVER_SCHEME_DOESNT_MATCH;
    extern const int DUPLICATE_DATA_PART;
    extern const int BAD_ARGUMENTS;
    extern const int CONCURRENT_ACCESS_NOT_SUPPORTED;
    extern const int CHECKSUM_DOESNT_MATCH;
}

namespace ActionLocks
{
    extern const StorageActionBlockType PartsMerge;
    extern const StorageActionBlockType PartsFetch;
    extern const StorageActionBlockType PartsSend;
    extern const StorageActionBlockType ReplicationQueue;
    extern const StorageActionBlockType PartsTTLMerge;
    extern const StorageActionBlockType PartsMove;
}


static const auto QUEUE_UPDATE_ERROR_SLEEP_MS        = 1 * 1000;
static const auto MUTATIONS_FINALIZING_SLEEP_MS      = 1 * 1000;
static const auto MUTATIONS_FINALIZING_IDLE_SLEEP_MS = 5 * 1000;

void StorageReplicatedMergeTree::setZooKeeper()
{
    /// Every ReplicatedMergeTree table is using only one ZooKeeper session.
    /// But if several ReplicatedMergeTree tables are using different
    /// ZooKeeper sessions, some queries like ATTACH PARTITION FROM may have
    /// strange effects. So we always use only one session for all tables.
    /// (excluding auxiliary zookeepers)

    std::lock_guard lock(current_zookeeper_mutex);
    if (zookeeper_name == default_zookeeper_name)
    {
        current_zookeeper = getContext()->getZooKeeper();
    }
    else
    {
        current_zookeeper = getContext()->getAuxiliaryZooKeeper(zookeeper_name);
    }
}

zkutil::ZooKeeperPtr StorageReplicatedMergeTree::tryGetZooKeeper() const
{
    std::lock_guard lock(current_zookeeper_mutex);
    return current_zookeeper;
}

zkutil::ZooKeeperPtr StorageReplicatedMergeTree::getZooKeeper() const
{
    auto res = tryGetZooKeeper();
    if (!res)
        throw Exception("Cannot get ZooKeeper", ErrorCodes::NO_ZOOKEEPER);
    return res;
}

static std::string normalizeZooKeeperPath(std::string zookeeper_path)
{
    if (!zookeeper_path.empty() && zookeeper_path.back() == '/')
        zookeeper_path.resize(zookeeper_path.size() - 1);
    /// If zookeeper chroot prefix is used, path should start with '/', because chroot concatenates without it.
    if (!zookeeper_path.empty() && zookeeper_path.front() != '/')
        zookeeper_path = "/" + zookeeper_path;

    return zookeeper_path;
}

static String extractZooKeeperName(const String & path)
{
    if (path.empty())
        throw Exception("ZooKeeper path should not be empty", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
    auto pos = path.find(':');
    if (pos != String::npos)
    {
        auto zookeeper_name = path.substr(0, pos);
        if (zookeeper_name.empty())
            throw Exception("Zookeeper path should start with '/' or '<auxiliary_zookeeper_name>:/'", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        return zookeeper_name;
    }
    static constexpr auto default_zookeeper_name = "default";
    return default_zookeeper_name;
}

static String extractZooKeeperPath(const String & path)
{
    if (path.empty())
        throw Exception("ZooKeeper path should not be empty", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
    auto pos = path.find(':');
    if (pos != String::npos)
    {
        return normalizeZooKeeperPath(path.substr(pos + 1, String::npos));
    }
    return normalizeZooKeeperPath(path);
}

static MergeTreePartInfo makeDummyDropRangeForMovePartitionOrAttachPartitionFrom(const String & partition_id)
{
    /// NOTE We don't have special log entry type for MOVE PARTITION/ATTACH PARTITION FROM,
    /// so we use REPLACE_RANGE with dummy range of one block, which means "attach, not replace".
    /// It's safe to fill drop range for MOVE PARTITION/ATTACH PARTITION FROM with zeros,
    /// because drop range for REPLACE PARTITION must contain at least 2 blocks,
    /// so we can distinguish dummy drop range from any real or virtual part.
    /// But we should never construct such part name, even for virtual part,
    /// because it can be confused with real part <partition>_0_0_0.
    /// TODO get rid of this.

    MergeTreePartInfo drop_range;
    drop_range.partition_id = partition_id;
    drop_range.min_block = 0;
    drop_range.max_block = 0;
    drop_range.level = 0;
    drop_range.mutation = 0;
    return drop_range;
}

StorageReplicatedMergeTree::StorageReplicatedMergeTree(
    const String & zookeeper_path_,
    const String & replica_name_,
    bool attach,
    const StorageID & table_id_,
    const String & relative_data_path_,
    const StorageInMemoryMetadata & metadata_,
    ContextMutablePtr context_,
    const String & date_column_name,
    const MergingParams & merging_params_,
    std::unique_ptr<MergeTreeSettings> settings_,
    bool has_force_restore_data_flag,
    bool allow_renaming_)
    : MergeTreeData(table_id_,
                    relative_data_path_,
                    metadata_,
                    context_,
                    date_column_name,
                    merging_params_,
                    std::move(settings_),
                    true,                   /// require_part_metadata
                    attach,
                    [this] (const std::string & name) { enqueuePartForCheck(name); })
    , zookeeper_name(extractZooKeeperName(zookeeper_path_))
    , zookeeper_path(extractZooKeeperPath(zookeeper_path_))
    , replica_name(replica_name_)
    , replica_path(fs::path(zookeeper_path) / "replicas" / replica_name_)
    , reader(*this)
    , writer(*this)
    , merger_mutator(*this,
        getContext()->getSettingsRef().background_merges_mutations_concurrency_ratio *
        getContext()->getSettingsRef().background_pool_size)
    , merge_strategy_picker(*this)
    , queue(*this, merge_strategy_picker)
    , fetcher(*this)
    , cleanup_thread(*this)
    , part_check_thread(*this)
    , restarting_thread(*this)
    , part_moves_between_shards_orchestrator(*this)
    , allow_renaming(allow_renaming_)
    , replicated_fetches_pool_size(getContext()->getSettingsRef().background_fetches_pool_size)
    , replicated_fetches_throttler(std::make_shared<Throttler>(getSettings()->max_replicated_fetches_network_bandwidth, getContext()->getReplicatedFetchesThrottler()))
    , replicated_sends_throttler(std::make_shared<Throttler>(getSettings()->max_replicated_sends_network_bandwidth, getContext()->getReplicatedSendsThrottler()))
{
    queue_updating_task = getContext()->getSchedulePool().createTask(
        getStorageID().getFullTableName() + " (StorageReplicatedMergeTree::queueUpdatingTask)", [this]{ queueUpdatingTask(); });

    mutations_updating_task = getContext()->getSchedulePool().createTask(
        getStorageID().getFullTableName() + " (StorageReplicatedMergeTree::mutationsUpdatingTask)", [this]{ mutationsUpdatingTask(); });

    merge_selecting_task = getContext()->getSchedulePool().createTask(
        getStorageID().getFullTableName() + " (StorageReplicatedMergeTree::mergeSelectingTask)", [this] { mergeSelectingTask(); });

    /// Will be activated if we win leader election.
    merge_selecting_task->deactivate();

    mutations_finalizing_task = getContext()->getSchedulePool().createTask(
        getStorageID().getFullTableName() + " (StorageReplicatedMergeTree::mutationsFinalizingTask)", [this] { mutationsFinalizingTask(); });

    if (getContext()->hasZooKeeper() || getContext()->hasAuxiliaryZooKeeper(zookeeper_name))
    {
        /// It's possible for getZooKeeper() to timeout if  zookeeper host(s) can't
        /// be reached. In such cases Poco::Exception is thrown after a connection
        /// timeout - refer to src/Common/ZooKeeper/ZooKeeperImpl.cpp:866 for more info.
        ///
        /// Side effect of this is that the CreateQuery gets interrupted and it exits.
        /// But the data Directories for the tables being created aren't cleaned up.
        /// This unclean state will hinder table creation on any retries and will
        /// complain that the Directory for table already exists.
        ///
        /// To achieve a clean state on failed table creations, catch this error and
        /// call dropIfEmpty() method only if the operation isn't ATTACH then proceed
        /// throwing the exception. Without this, the Directory for the tables need
        /// to be manually deleted before retrying the CreateQuery.
        try
        {
            if (zookeeper_name == default_zookeeper_name)
            {
                current_zookeeper = getContext()->getZooKeeper();
            }
            else
            {
                current_zookeeper = getContext()->getAuxiliaryZooKeeper(zookeeper_name);
            }
        }
        catch (...)
        {
            if (!attach)
                dropIfEmpty();
            throw;
        }
    }

    bool skip_sanity_checks = false;

    if (current_zookeeper && current_zookeeper->exists(replica_path + "/flags/force_restore_data"))
    {
        skip_sanity_checks = true;
        current_zookeeper->remove(replica_path + "/flags/force_restore_data");

        LOG_WARNING(log, "Skipping the limits on severity of changes to data parts and columns (flag {}/flags/force_restore_data).", replica_path);
    }
    else if (has_force_restore_data_flag)
    {
        skip_sanity_checks = true;

        LOG_WARNING(log, "Skipping the limits on severity of changes to data parts and columns (flag force_restore_data).");
    }

    loadDataParts(skip_sanity_checks);

    if (!current_zookeeper)
    {
        if (!attach)
        {
            dropIfEmpty();
            throw Exception("Can't create replicated table without ZooKeeper", ErrorCodes::NO_ZOOKEEPER);
        }

        /// Do not activate the replica. It will be readonly.
        LOG_ERROR(log, "No ZooKeeper: table will be in readonly mode.");
        is_readonly = true;
        return;
    }

    if (attach && !current_zookeeper->exists(zookeeper_path + "/metadata"))
    {
        LOG_WARNING(log, "No metadata in ZooKeeper for {}: table will be in readonly mode.", zookeeper_path);
        is_readonly = true;
        has_metadata_in_zookeeper = false;
        return;
    }

    auto metadata_snapshot = getInMemoryMetadataPtr();

    /// May it be ZK lost not the whole root, so the upper check passed, but only the /replicas/replica
    /// folder.
    if (attach && !current_zookeeper->exists(replica_path))
    {
        LOG_WARNING(log, "No metadata in ZooKeeper for {}: table will be in readonly mode", replica_path);
        is_readonly = true;
        has_metadata_in_zookeeper = false;
        return;
    }

    if (!attach)
    {
        if (!getDataParts().empty())
            throw Exception("Data directory for table already contains data parts"
                " - probably it was unclean DROP table or manual intervention."
                " You must either clear directory by hand or use ATTACH TABLE"
                " instead of CREATE TABLE if you need to use that parts.", ErrorCodes::INCORRECT_DATA);

        try
        {
            bool is_first_replica = createTableIfNotExists(metadata_snapshot);

            try
            {
                /// NOTE If it's the first replica, these requests to ZooKeeper look redundant, we already know everything.

                /// We have to check granularity on other replicas. If it's fixed we
                /// must create our new replica with fixed granularity and store this
                /// information in /replica/metadata.
                other_replicas_fixed_granularity = checkFixedGranularityInZookeeper();

                checkTableStructure(zookeeper_path, metadata_snapshot);

                Coordination::Stat metadata_stat;
                current_zookeeper->get(zookeeper_path + "/metadata", &metadata_stat);
                metadata_version = metadata_stat.version;
            }
            catch (Coordination::Exception & e)
            {
                if (!is_first_replica && e.code == Coordination::Error::ZNONODE)
                    throw Exception("Table " + zookeeper_path + " was suddenly removed.", ErrorCodes::ALL_REPLICAS_LOST);
                else
                    throw;
            }

            if (!is_first_replica)
                createReplica(metadata_snapshot);
        }
        catch (...)
        {
            /// If replica was not created, rollback creation of data directory.
            dropIfEmpty();
            throw;
        }
    }
    else
    {
        /// In old tables this node may missing or be empty
        String replica_metadata;
        const bool replica_metadata_exists = current_zookeeper->tryGet(replica_path + "/metadata", replica_metadata);

        if (!replica_metadata_exists || replica_metadata.empty())
        {
            /// We have to check shared node granularity before we create ours.
            other_replicas_fixed_granularity = checkFixedGranularityInZookeeper();

            ReplicatedMergeTreeTableMetadata current_metadata(*this, metadata_snapshot);

            current_zookeeper->createOrUpdate(replica_path + "/metadata", current_metadata.toString(),
                zkutil::CreateMode::Persistent);
        }

        checkTableStructure(replica_path, metadata_snapshot);
        checkParts(skip_sanity_checks);

        if (current_zookeeper->exists(replica_path + "/metadata_version"))
        {
            metadata_version = parse<int>(current_zookeeper->get(replica_path + "/metadata_version"));
        }
        else
        {
            /// This replica was created with old clickhouse version, so we have
            /// to take version of global node. If somebody will alter our
            /// table, then we will fill /metadata_version node in zookeeper.
            /// Otherwise on the next restart we can again use version from
            /// shared metadata node because it was not changed.
            Coordination::Stat metadata_stat;
            current_zookeeper->get(zookeeper_path + "/metadata", &metadata_stat);
            metadata_version = metadata_stat.version;
        }
        /// Temporary directories contain uninitialized results of Merges or Fetches (after forced restart),
        /// don't allow to reinitialize them, delete each of them immediately.
        clearOldTemporaryDirectories(0);
        clearOldWriteAheadLogs();
    }

    createNewZooKeeperNodes();
    syncPinnedPartUUIDs();
}


bool StorageReplicatedMergeTree::checkFixedGranularityInZookeeper()
{
    auto zookeeper = getZooKeeper();
    String metadata_str = zookeeper->get(zookeeper_path + "/metadata");
    auto metadata_from_zk = ReplicatedMergeTreeTableMetadata::parse(metadata_str);
    return metadata_from_zk.index_granularity_bytes == 0;
}


void StorageReplicatedMergeTree::waitMutationToFinishOnReplicas(
    const Strings & replicas, const String & mutation_id) const
{
    if (replicas.empty())
        return;


    std::set<String> inactive_replicas;
    for (const String & replica : replicas)
    {
        LOG_DEBUG(log, "Waiting for {} to apply mutation {}", replica, mutation_id);
        zkutil::EventPtr wait_event = std::make_shared<Poco::Event>();

        while (!partial_shutdown_called)
        {
            /// Mutation maybe killed or whole replica was deleted.
            /// Wait event will unblock at this moment.
            Coordination::Stat exists_stat;
            if (!getZooKeeper()->exists(fs::path(zookeeper_path) / "mutations" / mutation_id, &exists_stat, wait_event))
            {
                throw Exception(ErrorCodes::UNFINISHED, "Mutation {} was killed, manually removed or table was dropped", mutation_id);
            }

            auto zookeeper = getZooKeeper();
            /// Replica could be inactive.
            if (!zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "is_active"))
            {
                LOG_WARNING(log, "Replica {} is not active during mutation. Mutation will be done asynchronously when replica becomes active.", replica);

                inactive_replicas.emplace(replica);
                break;
            }

            String mutation_pointer = fs::path(zookeeper_path) / "replicas" / replica / "mutation_pointer";
            std::string mutation_pointer_value;
            /// Replica could be removed
            if (!zookeeper->tryGet(mutation_pointer, mutation_pointer_value, nullptr, wait_event))
            {
                LOG_WARNING(log, "Replica {} was removed", replica);
                break;
            }
            else if (mutation_pointer_value >= mutation_id) /// Maybe we already processed more fresh mutation
                break;                                      /// (numbers like 0000000000 and 0000000001)

            /// Replica can become inactive, so wait with timeout and recheck it
            if (wait_event->tryWait(1000))
                continue;

            /// Here we check mutation for errors on local replica. If they happen on this replica
            /// they will happen on each replica, so we can check only in-memory info.
            auto mutation_status = queue.getIncompleteMutationsStatus(mutation_id);
            /// If mutation status is empty, than local replica may just not loaded it into memory.
            if (mutation_status && !mutation_status->latest_fail_reason.empty())
                break;
        }

        /// This replica inactive, don't check anything
        if (!inactive_replicas.empty() && inactive_replicas.count(replica))
            break;

        /// It maybe already removed from zk, but local in-memory mutations
        /// state was not updated.
        if (!getZooKeeper()->exists(fs::path(zookeeper_path) / "mutations" / mutation_id))
        {
            throw Exception(ErrorCodes::UNFINISHED, "Mutation {} was killed, manually removed or table was dropped", mutation_id);
        }

        if (partial_shutdown_called)
            throw Exception("Mutation is not finished because table shutdown was called. It will be done after table restart.",
                ErrorCodes::UNFINISHED);

        /// Replica inactive, don't check mutation status
        if (!inactive_replicas.empty() && inactive_replicas.count(replica))
            continue;

        /// At least we have our current mutation
        std::set<String> mutation_ids;
        mutation_ids.insert(mutation_id);

        /// Here we check mutation for errors or kill on local replica. If they happen on this replica
        /// they will happen on each replica, so we can check only in-memory info.
        auto mutation_status = queue.getIncompleteMutationsStatus(mutation_id, &mutation_ids);
        checkMutationStatus(mutation_status, mutation_ids);
    }

    if (!inactive_replicas.empty())
    {
        throw Exception(ErrorCodes::UNFINISHED,
                        "Mutation is not finished because some replicas are inactive right now: {}. Mutation will be done asynchronously",
                        boost::algorithm::join(inactive_replicas, ", "));
    }
}

void StorageReplicatedMergeTree::createNewZooKeeperNodes()
{
    auto zookeeper = getZooKeeper();

    /// Working with quorum.
    zookeeper->createIfNotExists(zookeeper_path + "/quorum", String());
    zookeeper->createIfNotExists(zookeeper_path + "/quorum/parallel", String());
    zookeeper->createIfNotExists(zookeeper_path + "/quorum/last_part", String());
    zookeeper->createIfNotExists(zookeeper_path + "/quorum/failed_parts", String());

    /// Tracking lag of replicas.
    zookeeper->createIfNotExists(replica_path + "/min_unprocessed_insert_time", String());
    zookeeper->createIfNotExists(replica_path + "/max_processed_insert_time", String());

    /// Mutations
    zookeeper->createIfNotExists(zookeeper_path + "/mutations", String());
    zookeeper->createIfNotExists(replica_path + "/mutation_pointer", String());

    /// Nodes for remote fs zero-copy replication
    const auto settings = getSettings();
    if (settings->allow_remote_fs_zero_copy_replication)
    {
        zookeeper->createIfNotExists(zookeeper_path + "/zero_copy_s3", String());
        zookeeper->createIfNotExists(zookeeper_path + "/zero_copy_s3/shared", String());
        zookeeper->createIfNotExists(zookeeper_path + "/zero_copy_hdfs", String());
        zookeeper->createIfNotExists(zookeeper_path + "/zero_copy_hdfs/shared", String());
    }

    /// Part movement.
    zookeeper->createIfNotExists(zookeeper_path + "/part_moves_shard", String());
    zookeeper->createIfNotExists(zookeeper_path + "/pinned_part_uuids", getPinnedPartUUIDs()->toString());
    /// For ALTER PARTITION with multi-leaders
    zookeeper->createIfNotExists(zookeeper_path + "/alter_partition_version", String());
}


bool StorageReplicatedMergeTree::createTableIfNotExists(const StorageMetadataPtr & metadata_snapshot)
{
    auto zookeeper = getZooKeeper();
    zookeeper->createAncestors(zookeeper_path);

    for (size_t i = 0; i < 1000; ++i)
    {
        /// Invariant: "replicas" does not exist if there is no table or if there are leftovers from incompletely dropped table.
        if (zookeeper->exists(zookeeper_path + "/replicas"))
        {
            LOG_DEBUG(log, "This table {} is already created, will add new replica", zookeeper_path);
            return false;
        }

        /// There are leftovers from incompletely dropped table.
        if (zookeeper->exists(zookeeper_path + "/dropped"))
        {
            /// This condition may happen when the previous drop attempt was not completed
            ///  or when table is dropped by another replica right now.
            /// This is Ok because another replica is definitely going to drop the table.

            LOG_WARNING(log, "Removing leftovers from table {} (this might take several minutes)", zookeeper_path);
            String drop_lock_path = zookeeper_path + "/dropped/lock";
            Coordination::Error code = zookeeper->tryCreate(drop_lock_path, "", zkutil::CreateMode::Ephemeral);

            if (code == Coordination::Error::ZNONODE || code == Coordination::Error::ZNODEEXISTS)
            {
                LOG_WARNING(log, "The leftovers from table {} were removed by another replica", zookeeper_path);
            }
            else if (code != Coordination::Error::ZOK)
            {
                throw Coordination::Exception(code, drop_lock_path);
            }
            else
            {
                auto metadata_drop_lock = zkutil::EphemeralNodeHolder::existing(drop_lock_path, *zookeeper);
                if (!removeTableNodesFromZooKeeper(zookeeper, zookeeper_path, metadata_drop_lock, log))
                {
                    /// Someone is recursively removing table right now, we cannot create new table until old one is removed
                    continue;
                }
            }
        }

        LOG_DEBUG(log, "Creating table {}", zookeeper_path);

        /// We write metadata of table so that the replicas can check table parameters with them.
        String metadata_str = ReplicatedMergeTreeTableMetadata(*this, metadata_snapshot).toString();

        Coordination::Requests ops;
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path, "", zkutil::CreateMode::Persistent));

        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/metadata", metadata_str,
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/columns", metadata_snapshot->getColumns().toString(),
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/log", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/blocks", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/block_numbers", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/nonincrement_block_numbers", "",
            zkutil::CreateMode::Persistent)); /// /nonincrement_block_numbers dir is unused, but is created nonetheless for backwards compatibility.
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/leader_election", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/temp", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/replicas", "last added replica: " + replica_name,
            zkutil::CreateMode::Persistent));

        /// And create first replica atomically. See also "createReplica" method that is used to create not the first replicas.

        ops.emplace_back(zkutil::makeCreateRequest(replica_path, "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/host", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/log_pointer", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/queue", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/parts", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/flags", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/is_lost", "0",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/metadata", metadata_str,
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/columns", metadata_snapshot->getColumns().toString(),
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/metadata_version", std::to_string(metadata_version),
            zkutil::CreateMode::Persistent));

        Coordination::Responses responses;
        auto code = zookeeper->tryMulti(ops, responses);
        if (code == Coordination::Error::ZNODEEXISTS)
        {
            LOG_WARNING(log, "It looks like the table {} was created by another server at the same moment, will retry", zookeeper_path);
            continue;
        }
        else if (code != Coordination::Error::ZOK)
        {
            zkutil::KeeperMultiException::check(code, ops, responses);
        }

        return true;
    }

    /// Do not use LOGICAL_ERROR code, because it may happen if user has specified wrong zookeeper_path
    throw Exception("Cannot create table, because it is created concurrently every time "
                    "or because of wrong zookeeper_path "
                    "or because of logical error", ErrorCodes::REPLICA_IS_ALREADY_EXIST);
}

void StorageReplicatedMergeTree::createReplica(const StorageMetadataPtr & metadata_snapshot)
{
    auto zookeeper = getZooKeeper();

    LOG_DEBUG(log, "Creating replica {}", replica_path);

    Coordination::Error code;

    do
    {
        Coordination::Stat replicas_stat;
        String replicas_value;

        if (!zookeeper->tryGet(zookeeper_path + "/replicas", replicas_value, &replicas_stat))
            throw Exception(ErrorCodes::ALL_REPLICAS_LOST,
                "Cannot create a replica of the table {}, because the last replica of the table was dropped right now",
                zookeeper_path);

        /// It is not the first replica, we will mark it as "lost", to immediately repair (clone) from existing replica.
        /// By the way, it's possible that the replica will be first, if all previous replicas were removed concurrently.
        const String is_lost_value = replicas_stat.numChildren ? "1" : "0";

        Coordination::Requests ops;
        ops.emplace_back(zkutil::makeCreateRequest(replica_path, "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/host", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/log_pointer", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/queue", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/parts", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/flags", "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/is_lost", is_lost_value,
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/metadata", ReplicatedMergeTreeTableMetadata(*this, metadata_snapshot).toString(),
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/columns", metadata_snapshot->getColumns().toString(),
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(replica_path + "/metadata_version", std::to_string(metadata_version),
            zkutil::CreateMode::Persistent));

        /// Check version of /replicas to see if there are any replicas created at the same moment of time.
        ops.emplace_back(zkutil::makeSetRequest(zookeeper_path + "/replicas", "last added replica: " + replica_name, replicas_stat.version));

        Coordination::Responses responses;
        code = zookeeper->tryMulti(ops, responses);

        switch (code)
        {
            case Coordination::Error::ZNODEEXISTS:
                throw Exception(ErrorCodes::REPLICA_IS_ALREADY_EXIST, "Replica {} already exists", replica_path);
            case Coordination::Error::ZBADVERSION:
                LOG_ERROR(log, "Retrying createReplica(), because some other replicas were created at the same time");
                break;
            case Coordination::Error::ZNONODE:
                throw Exception(ErrorCodes::ALL_REPLICAS_LOST, "Table {} was suddenly removed", zookeeper_path);
            default:
                zkutil::KeeperMultiException::check(code, ops, responses);
        }
    } while (code == Coordination::Error::ZBADVERSION);
}

void StorageReplicatedMergeTree::drop()
{
    /// There is also the case when user has configured ClickHouse to wrong ZooKeeper cluster
    /// or metadata of staled replica were removed manually,
    /// in this case, has_metadata_in_zookeeper = false, and we also permit to drop the table.

    if (has_metadata_in_zookeeper)
    {
        /// Table can be shut down, restarting thread is not active
        /// and calling StorageReplicatedMergeTree::getZooKeeper()/getAuxiliaryZooKeeper() won't suffice.
        zkutil::ZooKeeperPtr zookeeper;
        if (zookeeper_name == default_zookeeper_name)
            zookeeper = getContext()->getZooKeeper();
        else
            zookeeper = getContext()->getAuxiliaryZooKeeper(zookeeper_name);

        /// If probably there is metadata in ZooKeeper, we don't allow to drop the table.
        if (!zookeeper)
            throw Exception("Can't drop readonly replicated table (need to drop data in ZooKeeper as well)", ErrorCodes::TABLE_IS_READ_ONLY);

        shutdown();
        dropReplica(zookeeper, zookeeper_path, replica_name, log);
    }

    dropAllData();
}

void StorageReplicatedMergeTree::dropReplica(zkutil::ZooKeeperPtr zookeeper, const String & zookeeper_path, const String & replica, Poco::Logger * logger)
{
    if (zookeeper->expired())
        throw Exception("Table was not dropped because ZooKeeper session has expired.", ErrorCodes::TABLE_WAS_NOT_DROPPED);

    auto remote_replica_path = zookeeper_path + "/replicas/" + replica;
    LOG_INFO(logger, "Removing replica {}, marking it as lost", remote_replica_path);
    /// Mark itself lost before removing, because the following recursive removal may fail
    /// and partially dropped replica may be considered as alive one (until someone will mark it lost)
    zookeeper->trySet(zookeeper_path + "/replicas/" + replica + "/is_lost", "1");
    /// It may left some garbage if replica_path subtree are concurrently modified
    zookeeper->tryRemoveRecursive(remote_replica_path);
    if (zookeeper->exists(remote_replica_path))
        LOG_ERROR(logger, "Replica was not completely removed from ZooKeeper, {} still exists and may contain some garbage.", remote_replica_path);

    /// Check that `zookeeper_path` exists: it could have been deleted by another replica after execution of previous line.
    Strings replicas;
    if (Coordination::Error::ZOK != zookeeper->tryGetChildren(zookeeper_path + "/replicas", replicas) || !replicas.empty())
        return;

    LOG_INFO(logger, "{} is the last replica, will remove table", remote_replica_path);

    /** At this moment, another replica can be created and we cannot remove the table.
      * Try to remove /replicas node first. If we successfully removed it,
      * it guarantees that we are the only replica that proceed to remove the table
      * and no new replicas can be created after that moment (it requires the existence of /replicas node).
      * and table cannot be recreated with new /replicas node on another servers while we are removing data,
      * because table creation is executed in single transaction that will conflict with remaining nodes.
      */

    /// Node /dropped works like a lock that protects from concurrent removal of old table and creation of new table.
    /// But recursive removal may fail in the middle of operation leaving some garbage in zookeeper_path, so
    /// we remove it on table creation if there is /dropped node. Creating thread may remove /dropped node created by
    /// removing thread, and it causes race condition if removing thread is not finished yet.
    /// To avoid this we also create ephemeral child before starting recursive removal.
    /// (The existence of child node does not allow to remove parent node).
    Coordination::Requests ops;
    Coordination::Responses responses;
    String drop_lock_path = zookeeper_path + "/dropped/lock";
    ops.emplace_back(zkutil::makeRemoveRequest(zookeeper_path + "/replicas", -1));
    ops.emplace_back(zkutil::makeCreateRequest(zookeeper_path + "/dropped", "", zkutil::CreateMode::Persistent));
    ops.emplace_back(zkutil::makeCreateRequest(drop_lock_path, "", zkutil::CreateMode::Ephemeral));
    Coordination::Error code = zookeeper->tryMulti(ops, responses);

    if (code == Coordination::Error::ZNONODE || code == Coordination::Error::ZNODEEXISTS)
    {
        LOG_WARNING(logger, "Table {} is already started to be removing by another replica right now", remote_replica_path);
    }
    else if (code == Coordination::Error::ZNOTEMPTY)
    {
        LOG_WARNING(logger, "Another replica was suddenly created, will keep the table {}", remote_replica_path);
    }
    else if (code != Coordination::Error::ZOK)
    {
        zkutil::KeeperMultiException::check(code, ops, responses);
    }
    else
    {
        auto metadata_drop_lock = zkutil::EphemeralNodeHolder::existing(drop_lock_path, *zookeeper);
        LOG_INFO(logger, "Removing table {} (this might take several minutes)", zookeeper_path);
        removeTableNodesFromZooKeeper(zookeeper, zookeeper_path, metadata_drop_lock, logger);
    }
}

bool StorageReplicatedMergeTree::removeTableNodesFromZooKeeper(zkutil::ZooKeeperPtr zookeeper,
        const String & zookeeper_path, const zkutil::EphemeralNodeHolder::Ptr & metadata_drop_lock, Poco::Logger * logger)
{
    bool completely_removed = false;
    Strings children;
    Coordination::Error code = zookeeper->tryGetChildren(zookeeper_path, children);
    if (code == Coordination::Error::ZNONODE)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "There is a race condition between creation and removal of replicated table. It's a bug");


    for (const auto & child : children)
        if (child != "dropped")
            zookeeper->tryRemoveRecursive(fs::path(zookeeper_path) / child);

    Coordination::Requests ops;
    Coordination::Responses responses;
    ops.emplace_back(zkutil::makeRemoveRequest(metadata_drop_lock->getPath(), -1));
    ops.emplace_back(zkutil::makeRemoveRequest(fs::path(zookeeper_path) / "dropped", -1));
    ops.emplace_back(zkutil::makeRemoveRequest(zookeeper_path, -1));
    code = zookeeper->tryMulti(ops, responses);

    if (code == Coordination::Error::ZNONODE)
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "There is a race condition between creation and removal of replicated table. It's a bug");
    }
    else if (code == Coordination::Error::ZNOTEMPTY)
    {
        LOG_ERROR(logger, "Table was not completely removed from ZooKeeper, {} still exists and may contain some garbage,"
                          "but someone is removing it right now.", zookeeper_path);
    }
    else if (code != Coordination::Error::ZOK)
    {
        /// It is still possible that ZooKeeper session is expired or server is killed in the middle of the delete operation.
        zkutil::KeeperMultiException::check(code, ops, responses);
    }
    else
    {
        metadata_drop_lock->setAlreadyRemoved();
        completely_removed = true;
        LOG_INFO(logger, "Table {} was successfully removed from ZooKeeper", zookeeper_path);
    }

    return completely_removed;
}


/** Verify that list of columns and table storage_settings_ptr match those specified in ZK (/metadata).
  * If not, throw an exception.
  */
void StorageReplicatedMergeTree::checkTableStructure(const String & zookeeper_prefix, const StorageMetadataPtr & metadata_snapshot)
{
    auto zookeeper = getZooKeeper();

    ReplicatedMergeTreeTableMetadata old_metadata(*this, metadata_snapshot);

    Coordination::Stat metadata_stat;
    String metadata_str = zookeeper->get(fs::path(zookeeper_prefix) / "metadata", &metadata_stat);
    auto metadata_from_zk = ReplicatedMergeTreeTableMetadata::parse(metadata_str);
    old_metadata.checkEquals(metadata_from_zk, metadata_snapshot->getColumns(), getContext());

    Coordination::Stat columns_stat;
    auto columns_from_zk = ColumnsDescription::parse(zookeeper->get(fs::path(zookeeper_prefix) / "columns", &columns_stat));

    const ColumnsDescription & old_columns = metadata_snapshot->getColumns();
    if (columns_from_zk != old_columns)
    {
        throw Exception(ErrorCodes::INCOMPATIBLE_COLUMNS,
            "Table columns structure in ZooKeeper is different from local table structure. Local columns:\n"
            "{}\nZookeeper columns:\n{}", old_columns.toString(), columns_from_zk.toString());
    }
}

void StorageReplicatedMergeTree::setTableStructure(
    ColumnsDescription new_columns, const ReplicatedMergeTreeTableMetadata::Diff & metadata_diff)
{
    StorageInMemoryMetadata new_metadata = getInMemoryMetadata();
    StorageInMemoryMetadata old_metadata = getInMemoryMetadata();

    new_metadata.columns = new_columns;

    if (!metadata_diff.empty())
    {
        auto parse_key_expr = [] (const String & key_expr)
        {
            ParserNotEmptyExpressionList parser(false);
            auto new_sorting_key_expr_list = parseQuery(parser, key_expr, 0, DBMS_DEFAULT_MAX_PARSER_DEPTH);

            ASTPtr order_by_ast;
            if (new_sorting_key_expr_list->children.size() == 1)
                order_by_ast = new_sorting_key_expr_list->children[0];
            else
            {
                auto tuple = makeASTFunction("tuple");
                tuple->arguments->children = new_sorting_key_expr_list->children;
                order_by_ast = tuple;
            }
            return order_by_ast;
        };

        if (metadata_diff.sorting_key_changed)
        {
            auto order_by_ast = parse_key_expr(metadata_diff.new_sorting_key);
            auto & sorting_key = new_metadata.sorting_key;
            auto & primary_key = new_metadata.primary_key;

            sorting_key.recalculateWithNewAST(order_by_ast, new_metadata.columns, getContext());

            if (primary_key.definition_ast == nullptr)
            {
                /// Primary and sorting key become independent after this ALTER so we have to
                /// save the old ORDER BY expression as the new primary key.
                auto old_sorting_key_ast = old_metadata.getSortingKey().definition_ast;
                primary_key = KeyDescription::getKeyFromAST(
                    old_sorting_key_ast, new_metadata.columns, getContext());
            }
        }

        if (metadata_diff.sampling_expression_changed)
        {
            auto sample_by_ast = parse_key_expr(metadata_diff.new_sampling_expression);
            new_metadata.sampling_key.recalculateWithNewAST(sample_by_ast, new_metadata.columns, getContext());
        }

        if (metadata_diff.skip_indices_changed)
            new_metadata.secondary_indices = IndicesDescription::parse(metadata_diff.new_skip_indices, new_columns, getContext());

        if (metadata_diff.constraints_changed)
            new_metadata.constraints = ConstraintsDescription::parse(metadata_diff.new_constraints);

        if (metadata_diff.projections_changed)
            new_metadata.projections = ProjectionsDescription::parse(metadata_diff.new_projections, new_columns, getContext());

        if (metadata_diff.ttl_table_changed)
        {
            if (!metadata_diff.new_ttl_table.empty())
            {
                ParserTTLExpressionList parser;
                auto ttl_for_table_ast = parseQuery(parser, metadata_diff.new_ttl_table, 0, DBMS_DEFAULT_MAX_PARSER_DEPTH);
                new_metadata.table_ttl = TTLTableDescription::getTTLForTableFromAST(
                    ttl_for_table_ast, new_metadata.columns, getContext(), new_metadata.primary_key);
            }
            else /// TTL was removed
            {
                new_metadata.table_ttl = TTLTableDescription{};
            }
        }
    }

    /// Changes in columns may affect following metadata fields
    new_metadata.column_ttls_by_name.clear();
    for (const auto & [name, ast] : new_metadata.columns.getColumnTTLs())
    {
        auto new_ttl_entry = TTLDescription::getTTLFromAST(ast, new_metadata.columns, getContext(), new_metadata.primary_key);
        new_metadata.column_ttls_by_name[name] = new_ttl_entry;
    }

    if (new_metadata.partition_key.definition_ast != nullptr)
        new_metadata.partition_key.recalculateWithNewColumns(new_metadata.columns, getContext());

    if (!metadata_diff.sorting_key_changed) /// otherwise already updated
        new_metadata.sorting_key.recalculateWithNewColumns(new_metadata.columns, getContext());

    /// Primary key is special, it exists even if not defined
    if (new_metadata.primary_key.definition_ast != nullptr)
    {
        new_metadata.primary_key.recalculateWithNewColumns(new_metadata.columns, getContext());
    }
    else
    {
        new_metadata.primary_key = KeyDescription::getKeyFromAST(new_metadata.sorting_key.definition_ast, new_metadata.columns, getContext());
        new_metadata.primary_key.definition_ast = nullptr;
    }

    if (!metadata_diff.sampling_expression_changed && new_metadata.sampling_key.definition_ast != nullptr)
        new_metadata.sampling_key.recalculateWithNewColumns(new_metadata.columns, getContext());

    if (!metadata_diff.skip_indices_changed) /// otherwise already updated
    {
        for (auto & index : new_metadata.secondary_indices)
            index.recalculateWithNewColumns(new_metadata.columns, getContext());
    }

    if (!metadata_diff.ttl_table_changed && new_metadata.table_ttl.definition_ast != nullptr)
        new_metadata.table_ttl = TTLTableDescription::getTTLForTableFromAST(
            new_metadata.table_ttl.definition_ast, new_metadata.columns, getContext(), new_metadata.primary_key);

    /// Even if the primary/sorting/partition keys didn't change we must reinitialize it
    /// because primary/partition key column types might have changed.
    checkTTLExpressions(new_metadata, old_metadata);
    setProperties(new_metadata, old_metadata);

    auto table_id = getStorageID();
    DatabaseCatalog::instance().getDatabase(table_id.database_name)->alterTable(getContext(), table_id, new_metadata);
}


/** If necessary, restore a part, replica itself adds a record for its receipt.
  * What time should I put for this entry in the queue? Time is taken into account when calculating lag of replica.
  * For these purposes, it makes sense to use creation time of missing part
  *  (that is, in calculating lag, it will be taken into account how old is the part we need to recover).
  */
static time_t tryGetPartCreateTime(zkutil::ZooKeeperPtr & zookeeper, const String & replica_path, const String & part_name)
{
    time_t res = 0;

    /// We get creation time of part, if it still exists (was not merged, for example).
    Coordination::Stat stat;
    String unused;
    if (zookeeper->tryGet(fs::path(replica_path) / "parts" / part_name, unused, &stat))
        res = stat.ctime / 1000;

    return res;
}


void StorageReplicatedMergeTree::checkParts(bool skip_sanity_checks)
{
    auto zookeeper = getZooKeeper();

    Strings expected_parts_vec = zookeeper->getChildren(fs::path(replica_path) / "parts");

    /// Parts in ZK.
    NameSet expected_parts(expected_parts_vec.begin(), expected_parts_vec.end());

    /// There are no PreCommitted parts at startup.
    auto parts = getDataParts({MergeTreeDataPartState::Committed, MergeTreeDataPartState::Outdated});

    /** Local parts that are not in ZK.
      * In very rare cases they may cover missing parts
      * and someone may think that pushing them to zookeeper is good idea.
      * But actually we can't precisely determine that ALL missing parts
      * covered by this unexpected part. So missing parts will be downloaded.
      */
    DataParts unexpected_parts;

    /// Collect unexpected parts
    for (const auto & part : parts)
        if (!expected_parts.count(part->name))
            unexpected_parts.insert(part); /// this parts we will place to detached with ignored_ prefix

    /// Which parts should be taken from other replicas.
    Strings parts_to_fetch;

    for (const String & missing_name : expected_parts)
        if (!getActiveContainingPart(missing_name))
            parts_to_fetch.push_back(missing_name);

    /** To check the adequacy, for the parts that are in the FS, but not in ZK, we will only consider not the most recent parts.
      * Because unexpected new parts usually arise only because they did not have time to enroll in ZK with a rough restart of the server.
      * It also occurs from deduplicated parts that did not have time to retire.
      */
    size_t unexpected_parts_nonnew = 0;
    UInt64 unexpected_parts_nonnew_rows = 0;
    UInt64 unexpected_parts_rows = 0;

    for (const auto & part : unexpected_parts)
    {
        if (part->info.level > 0)
        {
            ++unexpected_parts_nonnew;
            unexpected_parts_nonnew_rows += part->rows_count;
        }

        unexpected_parts_rows += part->rows_count;
    }

    const UInt64 parts_to_fetch_blocks = std::accumulate(parts_to_fetch.cbegin(), parts_to_fetch.cend(), 0,
        [&](UInt64 acc, const String& part_name)
        {
            if (const auto part_info = MergeTreePartInfo::tryParsePartName(part_name, format_version))
                return acc + part_info->getBlocksCount();

            LOG_ERROR(log, "Unexpected part name: {}", part_name);
            return acc;
        });

    /** We can automatically synchronize data,
      *  if the ratio of the total number of errors to the total number of parts (minimum - on the local filesystem or in ZK)
      *  is no more than some threshold (for example 50%).
      *
      * A large ratio of mismatches in the data on the filesystem and the expected data
      *  may indicate a configuration error (the server accidentally connected as a replica not from right shard).
      * In this case, the protection mechanism does not allow the server to start.
      */

    UInt64 total_rows_on_filesystem = 0;
    for (const auto & part : parts)
        total_rows_on_filesystem += part->rows_count;

    const auto storage_settings_ptr = getSettings();
    bool insane = unexpected_parts_rows > total_rows_on_filesystem * storage_settings_ptr->replicated_max_ratio_of_wrong_parts;

    constexpr const char * sanity_report_fmt = "The local set of parts of table {} doesn't look like the set of parts in ZooKeeper: "
                                               "{} rows of {} total rows in filesystem are suspicious. "
                                               "There are {} unexpected parts with {} rows ({} of them is not just-written with {} rows), "
                                               "{} missing parts (with {} blocks).";

    if (insane && !skip_sanity_checks)
    {
        throw Exception(ErrorCodes::TOO_MANY_UNEXPECTED_DATA_PARTS, sanity_report_fmt, getStorageID().getNameForLogs(),
                        formatReadableQuantity(unexpected_parts_rows), formatReadableQuantity(total_rows_on_filesystem),
                        unexpected_parts.size(), unexpected_parts_rows, unexpected_parts_nonnew, unexpected_parts_nonnew_rows,
                        parts_to_fetch.size(), parts_to_fetch_blocks);
    }

    if (unexpected_parts_nonnew_rows > 0)
    {
        LOG_WARNING(log, sanity_report_fmt, getStorageID().getNameForLogs(),
                    formatReadableQuantity(unexpected_parts_rows), formatReadableQuantity(total_rows_on_filesystem),
                    unexpected_parts.size(), unexpected_parts_rows, unexpected_parts_nonnew, unexpected_parts_nonnew_rows,
                    parts_to_fetch.size(), parts_to_fetch_blocks);
    }

    /// Add to the queue jobs to pick up the missing parts from other replicas and remove from ZK the information that we have them.
    std::vector<std::future<Coordination::ExistsResponse>> exists_futures;
    exists_futures.reserve(parts_to_fetch.size());
    for (const String & part_name : parts_to_fetch)
    {
        String part_path = fs::path(replica_path) / "parts" / part_name;
        exists_futures.emplace_back(zookeeper->asyncExists(part_path));
    }

    std::vector<std::future<Coordination::MultiResponse>> enqueue_futures;
    enqueue_futures.reserve(parts_to_fetch.size());
    for (size_t i = 0; i < parts_to_fetch.size(); ++i)
    {
        const String & part_name = parts_to_fetch[i];

        Coordination::Requests ops;

        LOG_ERROR(log, "Removing locally missing part from ZooKeeper and queueing a fetch: {}", part_name);
        time_t part_create_time = 0;
        Coordination::ExistsResponse exists_resp = exists_futures[i].get();
        if (exists_resp.error == Coordination::Error::ZOK)
        {
            part_create_time = exists_resp.stat.ctime / 1000;
            removePartFromZooKeeper(part_name, ops, exists_resp.stat.numChildren > 0);
        }
        LogEntry log_entry;
        log_entry.type = LogEntry::GET_PART;
        log_entry.source_replica = "";
        log_entry.new_part_name = part_name;
        log_entry.create_time = part_create_time;

        /// We assume that this occurs before the queue is loaded (queue.initialize).
        ops.emplace_back(zkutil::makeCreateRequest(
            fs::path(replica_path) / "queue/queue-", log_entry.toString(), zkutil::CreateMode::PersistentSequential));
        enqueue_futures.emplace_back(zookeeper->asyncMulti(ops));
    }

    for (auto & future : enqueue_futures)
        future.get();

    /// Remove extra local parts.
    for (const DataPartPtr & part : unexpected_parts)
    {
        LOG_ERROR(log, "Renaming unexpected part {} to ignored_{}", part->name, part->name);
        forgetPartAndMoveToDetached(part, "ignored", true);
    }
}


void StorageReplicatedMergeTree::syncPinnedPartUUIDs()
{
    auto zookeeper = getZooKeeper();

    Coordination::Stat stat;
    String s = zookeeper->get(zookeeper_path + "/pinned_part_uuids", &stat);

    std::lock_guard lock(pinned_part_uuids_mutex);

    /// Unsure whether or not this can be called concurrently.
    if (pinned_part_uuids->stat.version < stat.version)
    {
        auto new_pinned_part_uuids = std::make_shared<PinnedPartUUIDs>();
        new_pinned_part_uuids->fromString(s);
        new_pinned_part_uuids->stat = stat;

        pinned_part_uuids = new_pinned_part_uuids;
    }
}

void StorageReplicatedMergeTree::checkPartChecksumsAndAddCommitOps(const zkutil::ZooKeeperPtr & zookeeper,
    const DataPartPtr & part, Coordination::Requests & ops, String part_name, NameSet * absent_replicas_paths)
{
    if (part_name.empty())
        part_name = part->name;

    auto local_part_header = ReplicatedMergeTreePartHeader::fromColumnsAndChecksums(
        part->getColumns(), part->checksums);

    Strings replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");
    std::shuffle(replicas.begin(), replicas.end(), thread_local_rng);
    bool has_been_already_added = false;

    for (const String & replica : replicas)
    {
        String current_part_path = fs::path(zookeeper_path) / "replicas" / replica / "parts" / part_name;

        String part_zk_str;
        if (!zookeeper->tryGet(current_part_path, part_zk_str))
        {
            if (absent_replicas_paths)
                absent_replicas_paths->emplace(current_part_path);

            continue;
        }

        ReplicatedMergeTreePartHeader replica_part_header;
        if (part_zk_str.empty())
        {
            String columns_str;
            String checksums_str;
            if (zookeeper->tryGet(fs::path(current_part_path) / "columns", columns_str) &&
                zookeeper->tryGet(fs::path(current_part_path) / "checksums", checksums_str))
            {
                replica_part_header = ReplicatedMergeTreePartHeader::fromColumnsAndChecksumsZNodes(columns_str, checksums_str);
            }
            else
            {
                if (zookeeper->exists(current_part_path))
                    throw Exception(ErrorCodes::LOGICAL_ERROR, "Part {} has empty header and does not have columns and checksums. "
                                                               "Looks like a bug.", current_part_path);
                LOG_INFO(log, "Not checking checksums of part {} with replica {} because part was removed from ZooKeeper", part_name, replica);
                continue;
            }
        }
        else
        {
            replica_part_header = ReplicatedMergeTreePartHeader::fromString(part_zk_str);
        }

        if (replica_part_header.getColumnsHash() != local_part_header.getColumnsHash())
        {
            /// Either it's a bug or ZooKeeper contains broken data.
            /// TODO Fix KILL MUTATION and replace CHECKSUM_DOESNT_MATCH with LOGICAL_ERROR
            /// (some replicas may skip killed mutation even if it was executed on other replicas)
            throw Exception(ErrorCodes::CHECKSUM_DOESNT_MATCH, "Part {} from {} has different columns hash", part_name, replica);
        }

        replica_part_header.getChecksums().checkEqual(local_part_header.getChecksums(), true);

        if (replica == replica_name)
            has_been_already_added = true;

        /// If we verify checksums in "sequential manner" (i.e. recheck absence of checksums on other replicas when commit)
        /// then it is enough to verify checksums on at least one replica since checksums on other replicas must be the same.
        if (absent_replicas_paths)
        {
            absent_replicas_paths->clear();
            break;
        }
    }

    if (!has_been_already_added)
    {
        const auto storage_settings_ptr = getSettings();
        String part_path = fs::path(replica_path) / "parts" / part_name;

        //ops.emplace_back(zkutil::makeCheckRequest(
        //    zookeeper_path + "/columns", expected_columns_version));

        if (storage_settings_ptr->use_minimalistic_part_header_in_zookeeper)
        {
            ops.emplace_back(zkutil::makeCreateRequest(
                part_path, local_part_header.toString(), zkutil::CreateMode::Persistent));
        }
        else
        {
            ops.emplace_back(zkutil::makeCreateRequest(
                part_path, "", zkutil::CreateMode::Persistent));
            ops.emplace_back(zkutil::makeCreateRequest(
                fs::path(part_path) / "columns", part->getColumns().toString(), zkutil::CreateMode::Persistent));
            ops.emplace_back(zkutil::makeCreateRequest(
                fs::path(part_path) / "checksums", getChecksumsForZooKeeper(part->checksums), zkutil::CreateMode::Persistent));
        }
    }
    else
    {
        LOG_WARNING(log, "checkPartAndAddToZooKeeper: node {} already exists. Will not commit any nodes.",
                    (fs::path(replica_path) / "parts" / part_name).string());
    }
}

MergeTreeData::DataPartsVector StorageReplicatedMergeTree::checkPartChecksumsAndCommit(Transaction & transaction,
    const DataPartPtr & part)
{
    auto zookeeper = getZooKeeper();

    while (true)
    {
        Coordination::Requests ops;
        NameSet absent_part_paths_on_replicas;

        /// Checksums are checked here and `ops` is filled. In fact, the part is added to ZK just below, when executing `multi`.
        checkPartChecksumsAndAddCommitOps(zookeeper, part, ops, part->name, &absent_part_paths_on_replicas);

        /// Do not commit if the part is obsolete, we have just briefly checked its checksums
        if (transaction.isEmpty())
            return {};

        /// Will check that the part did not suddenly appear on skipped replicas
        if (!absent_part_paths_on_replicas.empty())
        {
            Coordination::Requests new_ops;
            for (const String & part_path : absent_part_paths_on_replicas)
            {
                new_ops.emplace_back(zkutil::makeCreateRequest(part_path, "", zkutil::CreateMode::Persistent));
                new_ops.emplace_back(zkutil::makeRemoveRequest(part_path, -1));
            }

            /// Add check ops at the beginning
            new_ops.insert(new_ops.end(), ops.begin(), ops.end());
            ops = std::move(new_ops);
        }

        try
        {
            zookeeper->multi(ops);
            return transaction.commit();
        }
        catch (const zkutil::KeeperMultiException & e)
        {
            size_t num_check_ops = 2 * absent_part_paths_on_replicas.size();
            size_t failed_op_index = e.failed_op_index;

            if (failed_op_index < num_check_ops && e.code == Coordination::Error::ZNODEEXISTS)
            {
                LOG_INFO(log, "The part {} on a replica suddenly appeared, will recheck checksums", e.getPathForFirstFailedOp());
            }
            else
                throw;
        }
    }
}

String StorageReplicatedMergeTree::getChecksumsForZooKeeper(const MergeTreeDataPartChecksums & checksums) const
{
    return MinimalisticDataPartChecksums::getSerializedString(checksums,
        getSettings()->use_minimalistic_checksums_in_zookeeper);
}

MergeTreeData::MutableDataPartPtr StorageReplicatedMergeTree::attachPartHelperFoundValidPart(const LogEntry& entry) const
{
    const MergeTreePartInfo actual_part_info = MergeTreePartInfo::fromPartName(entry.new_part_name, format_version);
    const String part_new_name = actual_part_info.getPartName();

    for (const DiskPtr & disk : getStoragePolicy()->getDisks())
        for (const auto it = disk->iterateDirectory(fs::path(relative_data_path) / "detached/"); it->isValid(); it->next())
        {
            const auto part_info = MergeTreePartInfo::tryParsePartName(it->name(), format_version);

            if (!part_info || part_info->partition_id != actual_part_info.partition_id)
                continue;

            const String part_old_name = part_info->getPartName();
            const String part_path = fs::path("detached") / part_old_name;

            const VolumePtr volume = std::make_shared<SingleDiskVolume>("volume_" + part_old_name, disk);

            /// actual_part_info is more recent than part_info so we use it
            MergeTreeData::MutableDataPartPtr part = createPart(part_new_name, actual_part_info, volume, part_path);

            try
            {
                part->loadColumnsChecksumsIndexes(true, true);
            }
            catch (const Exception&)
            {
                /// This method throws if the part data is corrupted or partly missing. In this case, we simply don't
                /// process the part.
                continue;
            }

            if (entry.part_checksum == part->checksums.getTotalChecksumHex())
            {
                part->modification_time = disk->getLastModified(part->getFullRelativePath()).epochTime();
                return part;
            }
        }

    return {};
}

bool StorageReplicatedMergeTree::executeLogEntry(LogEntry & entry)
{
    if (entry.type == LogEntry::DROP_RANGE)
    {
        executeDropRange(entry);
        return true;
    }

    if (entry.type == LogEntry::REPLACE_RANGE)
    {
        executeReplaceRange(entry);
        return true;
    }

    const bool is_get_or_attach = entry.type == LogEntry::GET_PART || entry.type == LogEntry::ATTACH_PART;

    if (is_get_or_attach || entry.type == LogEntry::MERGE_PARTS || entry.type == LogEntry::MUTATE_PART)
    {
        /// If we already have this part or a part covering it, we do not need to do anything.
        /// The part may be still in the PreCommitted -> Committed transition so we first search
        /// among PreCommitted parts to definitely find the desired part if it exists.
        DataPartPtr existing_part = getPartIfExists(entry.new_part_name, {MergeTreeDataPartState::PreCommitted});

        if (!existing_part)
            existing_part = getActiveContainingPart(entry.new_part_name);

        /// Even if the part is local, it (in exceptional cases) may not be in ZooKeeper. Let's check that it is there.
        if (existing_part && getZooKeeper()->exists(fs::path(replica_path) / "parts" / existing_part->name))
        {
            if (!is_get_or_attach || entry.source_replica != replica_name)
                LOG_DEBUG(log, "Skipping action for part {} because part {} already exists.",
                    entry.new_part_name, existing_part->name);

            return true;
        }
    }

    if (entry.type == LogEntry::ATTACH_PART)
    {
        if (MutableDataPartPtr part = attachPartHelperFoundValidPart(entry); part)
        {
            LOG_TRACE(log, "Found valid local part for {}, preparing the transaction", part->name);

            Transaction transaction(*this);

            renameTempPartAndReplace(part, nullptr, &transaction);
            checkPartChecksumsAndCommit(transaction, part);

            writePartLog(PartLogElement::Type::NEW_PART, {}, 0 /** log entry is fake so we don't measure the time */,
                part->name, part, {} /** log entry is fake so there are no initial parts */, nullptr);

            return true;
        }

        LOG_TRACE(log, "Didn't find valid local part for {} ({}), will fetch it from other replica",
            entry.new_part_name,
            entry.actual_new_part_name);
    }

    if (is_get_or_attach && entry.source_replica == replica_name)
        LOG_WARNING(log, "Part {} from own log doesn't exist.", entry.new_part_name);

    /// Perhaps we don't need this part, because during write with quorum, the quorum has failed
    /// (see below about `/quorum/failed_parts`).
    if (entry.quorum && getZooKeeper()->exists(fs::path(zookeeper_path) / "quorum" / "failed_parts" / entry.new_part_name))
    {
        LOG_DEBUG(log, "Skipping action for part {} because quorum for that part was failed.", entry.new_part_name);
        return true;    /// NOTE Deletion from `virtual_parts` is not done, but it is only necessary for merge.
    }

    // bool do_fetch = false;

    switch (entry.type)
    {
        case LogEntry::ATTACH_PART:
            /// We surely don't have this part locally as we've checked it before, so download it.
            [[fallthrough]];
        case LogEntry::GET_PART:
            return executeFetch(entry);
            // do_fetch = true;
        case LogEntry::MERGE_PARTS:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Merge has to be executed by another function");
        case LogEntry::MUTATE_PART:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Mutation has to be executed by another function");
        case LogEntry::ALTER_METADATA:
            return executeMetadataAlter(entry);
        case LogEntry::SYNC_PINNED_PART_UUIDS:
            syncPinnedPartUUIDs();
            return true;
        case LogEntry::CLONE_PART_FROM_SHARD:
            executeClonePartFromShard(entry);
            return true;
        default:
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected log entry type: {}", static_cast<int>(entry.type));
    }

    // return true;
}


bool StorageReplicatedMergeTree::executeFetch(LogEntry & entry)
{
    /// Looking for covering part. After that entry.actual_new_part_name may be filled.
    String replica = findReplicaHavingCoveringPart(entry, true);
    const auto storage_settings_ptr = getSettings();
    auto metadata_snapshot = getInMemoryMetadataPtr();

    try
    {
        if (replica.empty())
        {
            /** If a part is to be written with a quorum and the quorum is not reached yet,
              *  then (due to the fact that a part is impossible to download right now),
              *  the quorum entry should be considered unsuccessful.
              * TODO Complex code, extract separately.
              */
            if (entry.quorum)
            {
                if (entry.type != LogEntry::GET_PART)
                    throw Exception("Logical error: log entry with quorum but type is not GET_PART", ErrorCodes::LOGICAL_ERROR);

                LOG_DEBUG(log, "No active replica has part {} which needs to be written with quorum. Will try to mark that quorum as failed.", entry.new_part_name);

                /** Atomically:
                  * - if replicas do not become active;
                  * - if there is a `quorum` node with this part;
                  * - delete `quorum` node;
                  * - add a part to the list `quorum/failed_parts`;
                  * - if the part is not already removed from the list for deduplication `blocks/block_num`, then delete it;
                  *
                  * If something changes, then we will nothing - we'll get here again next time.
                  */

                /** We collect the `host` node versions from the replicas.
                  * When the replica becomes active, it changes the value of host in the same transaction (with the creation of `is_active`).
                  * This will ensure that the replicas do not become active.
                  */

                auto zookeeper = getZooKeeper();

                Strings replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");

                Coordination::Requests ops;

                for (const auto & path_part : replicas)
                {
                    Coordination::Stat stat;
                    String path = fs::path(zookeeper_path) / "replicas" / path_part / "host";
                    zookeeper->get(path, &stat);
                    ops.emplace_back(zkutil::makeCheckRequest(path, stat.version));
                }

                /// We verify that while we were collecting versions, the replica with the necessary part did not come alive.
                replica = findReplicaHavingPart(entry.new_part_name, true);

                /// Also during this time a completely new replica could be created.
                /// But if a part does not appear on the old, then it can not be on the new one either.

                if (replica.empty())
                {
                    Coordination::Stat quorum_stat;
                    const String quorum_unparallel_path = fs::path(zookeeper_path) / "quorum" / "status";
                    const String quorum_parallel_path = fs::path(zookeeper_path) / "quorum" / "parallel" / entry.new_part_name;
                    String quorum_str, quorum_path;
                    ReplicatedMergeTreeQuorumEntry quorum_entry;

                    if (zookeeper->tryGet(quorum_unparallel_path, quorum_str, &quorum_stat))
                        quorum_path = quorum_unparallel_path;
                    else
                    {
                        quorum_str = zookeeper->get(quorum_parallel_path, &quorum_stat);
                        quorum_path = quorum_parallel_path;
                    }

                    quorum_entry.fromString(quorum_str);

                    if (quorum_entry.part_name == entry.new_part_name)
                    {
                        ops.emplace_back(zkutil::makeRemoveRequest(quorum_path, quorum_stat.version));
                        auto part_info = MergeTreePartInfo::fromPartName(entry.new_part_name, format_version);

                        if (part_info.min_block != part_info.max_block)
                            throw Exception("Logical error: log entry with quorum for part covering more than one block number",
                                ErrorCodes::LOGICAL_ERROR);

                        ops.emplace_back(zkutil::makeCreateRequest(
                            fs::path(zookeeper_path) / "quorum" / "failed_parts" / entry.new_part_name,
                            "",
                            zkutil::CreateMode::Persistent));

                        /// Deleting from `blocks`.
                        if (!entry.block_id.empty() && zookeeper->exists(fs::path(zookeeper_path) / "blocks" / entry.block_id))
                            ops.emplace_back(zkutil::makeRemoveRequest(fs::path(zookeeper_path) / "blocks" / entry.block_id, -1));

                        Coordination::Responses responses;
                        auto code = zookeeper->tryMulti(ops, responses);

                        if (code == Coordination::Error::ZOK)
                        {
                            LOG_DEBUG(log, "Marked quorum for part {} as failed.", entry.new_part_name);
                            queue.removeFailedQuorumPart(part_info);
                            return true;
                        }
                        else if (code == Coordination::Error::ZBADVERSION || code == Coordination::Error::ZNONODE || code == Coordination::Error::ZNODEEXISTS)
                        {
                            LOG_DEBUG(log, "State was changed or isn't expected when trying to mark quorum for part {} as failed. Code: {}",
                                      entry.new_part_name, Coordination::errorMessage(code));
                        }
                        else
                            throw Coordination::Exception(code);
                    }
                    else
                    {
                        LOG_WARNING(log, "No active replica has part {}, "
                                         "but that part needs quorum and /quorum/status contains entry about another part {}. "
                                         "It means that part was successfully written to {} replicas, but then all of them goes offline. "
                                         "Or it is a bug.", entry.new_part_name, quorum_entry.part_name, entry.quorum);
                    }
                }
            }

            if (replica.empty())
            {
                ProfileEvents::increment(ProfileEvents::ReplicatedPartFailedFetches);
                throw Exception("No active replica has part " + entry.new_part_name + " or covering part", ErrorCodes::NO_REPLICA_HAS_PART);
            }
        }

        try
        {
            String part_name = entry.actual_new_part_name.empty() ? entry.new_part_name : entry.actual_new_part_name;

            if (!entry.actual_new_part_name.empty())
                LOG_DEBUG(log, "Will fetch part {} instead of {}", entry.actual_new_part_name, entry.new_part_name);

            if (!fetchPart(part_name, metadata_snapshot, fs::path(zookeeper_path) / "replicas" / replica, false, entry.quorum))
                return false;
        }
        catch (Exception & e)
        {
            /// No stacktrace, just log message
            if (e.code() == ErrorCodes::RECEIVED_ERROR_TOO_MANY_REQUESTS)
                e.addMessage("Too busy replica. Will try later.");
            throw;
        }

        if (entry.type == LogEntry::MERGE_PARTS)
            ProfileEvents::increment(ProfileEvents::ReplicatedPartFetchesOfMerged);
    }
    catch (...)
    {
        /** If we can not download the part we need for some merge, it's better not to try to get other parts for this merge,
          * but try to get already merged part. To do this, move the action to get the remaining parts
          * for this merge at the end of the queue.
          */
        try
        {
            auto parts_for_merge = queue.moveSiblingPartsForMergeToEndOfQueue(entry.new_part_name);

            if (!parts_for_merge.empty() && replica.empty())
            {
                LOG_INFO(log, "No active replica has part {}. Will fetch merged part instead.", entry.new_part_name);
                /// We should enqueue it for check, because merged part may never appear if source part is lost
                enqueuePartForCheck(entry.new_part_name);
                return false;
            }

            /** If no active replica has a part, and there is no merge in the queue with its participation,
              * check to see if any (active or inactive) replica has such a part or covering it.
              */
            if (replica.empty())
                enqueuePartForCheck(entry.new_part_name);
        }
        catch (...)
        {
            tryLogCurrentException(log, __PRETTY_FUNCTION__);
        }

        throw;
    }

    return true;
}


bool StorageReplicatedMergeTree::executeFetchShared(
    const String & source_replica,
    const String & new_part_name,
    const DiskPtr & disk,
    const String & path)
{
    if (source_replica.empty())
    {
        LOG_INFO(log, "No active replica has part {} on shared storage.", new_part_name);
        return false;
    }

    const auto storage_settings_ptr = getSettings();
    auto metadata_snapshot = getInMemoryMetadataPtr();

    try
    {
        if (!fetchExistsPart(new_part_name, metadata_snapshot, fs::path(zookeeper_path) / "replicas" / source_replica, disk, path))
            return false;
    }
    catch (Exception & e)
    {
        if (e.code() == ErrorCodes::RECEIVED_ERROR_TOO_MANY_REQUESTS)
            e.addMessage("Too busy replica. Will try later.");
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
        throw;
    }

    return true;
}


void StorageReplicatedMergeTree::executeDropRange(const LogEntry & entry)
{
    auto drop_range_info = MergeTreePartInfo::fromPartName(entry.new_part_name, format_version);
    getContext()->getMergeList().cancelInPartition(getStorageID(), drop_range_info.partition_id, drop_range_info.max_block);
    queue.removePartProducingOpsInRange(getZooKeeper(), drop_range_info, entry);

    /// Delete the parts contained in the range to be deleted.
    /// It's important that no old parts remain (after the merge), because otherwise,
    ///  after adding a new replica, this new replica downloads them, but does not delete them.
    /// And, if you do not, the parts will come to life after the server is restarted.
    /// Therefore, we use all data parts.

    auto metadata_snapshot = getInMemoryMetadataPtr();
    DataPartsVector parts_to_remove;
    {
        auto data_parts_lock = lockParts();
        parts_to_remove = removePartsInRangeFromWorkingSet(drop_range_info, true, data_parts_lock);
        if (parts_to_remove.empty())
        {
            if (!drop_range_info.isFakeDropRangePart())
                LOG_INFO(log, "Log entry {} tried to drop single part {}, but part does not exist", entry.znode_name, entry.new_part_name);
            return;
        }
    }

    if (entry.detach)
        LOG_DEBUG(log, "Detaching parts.");
    else
        LOG_DEBUG(log, "Removing parts.");

    if (entry.detach)
    {
        /// If DETACH clone parts to detached/ directory
        for (const auto & part : parts_to_remove)
        {
            LOG_INFO(log, "Detaching {}", part->relative_path);
            part->makeCloneInDetached("", metadata_snapshot);
        }
    }

    /// Forcibly remove parts from ZooKeeper
    tryRemovePartsFromZooKeeperWithRetries(parts_to_remove);

    if (entry.detach)
        LOG_DEBUG(log, "Detached {} parts inside {}.", parts_to_remove.size(), entry.new_part_name);
    else
        LOG_DEBUG(log, "Removed {} parts inside {}.", parts_to_remove.size(), entry.new_part_name);

    /// We want to remove dropped parts from disk as soon as possible
    /// To be removed a partition should have zero refcount, therefore call the cleanup thread at exit
    parts_to_remove.clear();
    cleanup_thread.wakeup();
}


bool StorageReplicatedMergeTree::executeReplaceRange(const LogEntry & entry)
{
    Stopwatch watch;
    auto & entry_replace = *entry.replace_range_entry;
    LOG_DEBUG(log, "Executing log entry {} to replace parts range {} with {} parts from {}.{}",
              entry.znode_name, entry_replace.drop_range_part_name, entry_replace.new_part_names.size(),
              entry_replace.from_database, entry_replace.from_table);
    auto metadata_snapshot = getInMemoryMetadataPtr();

    MergeTreePartInfo drop_range = MergeTreePartInfo::fromPartName(entry_replace.drop_range_part_name, format_version);
    /// Range with only one block has special meaning: it's ATTACH PARTITION or MOVE PARTITION, so there is no drop range
    bool replace = !LogEntry::ReplaceRangeEntry::isMovePartitionOrAttachFrom(drop_range);

    if (replace)
    {
        getContext()->getMergeList().cancelInPartition(getStorageID(), drop_range.partition_id, drop_range.max_block);
        queue.removePartProducingOpsInRange(getZooKeeper(), drop_range, entry);
    }
    else
    {
        drop_range = {};
    }

    struct PartDescription
    {
        PartDescription(
            size_t index_,
            const String & src_part_name_,
            const String & new_part_name_,
            const String & checksum_hex_,
            MergeTreeDataFormatVersion format_version)
            : index(index_)
            , src_part_name(src_part_name_)
            , src_part_info(MergeTreePartInfo::fromPartName(src_part_name_, format_version))
            , new_part_name(new_part_name_)
            , new_part_info(MergeTreePartInfo::fromPartName(new_part_name_, format_version))
            , checksum_hex(checksum_hex_)
        {
        }

        size_t index; // in log entry arrays
        String src_part_name;
        MergeTreePartInfo src_part_info;
        String new_part_name;
        MergeTreePartInfo new_part_info;
        String checksum_hex;

        /// Part which will be committed
        MutableDataPartPtr res_part;

        /// We could find a covering part
        MergeTreePartInfo found_new_part_info;
        String found_new_part_name;

        /// Hold pointer to part in source table if will clone it from local table
        DataPartPtr src_table_part;

        /// A replica that will be used to fetch part
        String replica;
    };

    using PartDescriptionPtr = std::shared_ptr<PartDescription>;
    using PartDescriptions = std::vector<PartDescriptionPtr>;

    PartDescriptions all_parts;
    PartDescriptions parts_to_add;
    DataPartsVector parts_to_remove;

    auto table_lock_holder_dst_table = lockForShare(
            RWLockImpl::NO_QUERY, getSettings()->lock_acquire_timeout_for_background_operations);
    auto dst_metadata_snapshot = getInMemoryMetadataPtr();

    for (size_t i = 0; i < entry_replace.new_part_names.size(); ++i)
    {
        all_parts.emplace_back(std::make_shared<PartDescription>(i,
            entry_replace.src_part_names.at(i),
            entry_replace.new_part_names.at(i),
            entry_replace.part_names_checksums.at(i),
            format_version));
    }

    /// What parts we should add? Or we have already added all required parts (we an replica-initializer)
    {
        auto data_parts_lock = lockParts();

        for (const PartDescriptionPtr & part_desc : all_parts)
        {
            if (!getActiveContainingPart(part_desc->new_part_info, MergeTreeDataPartState::Committed, data_parts_lock))
                parts_to_add.emplace_back(part_desc);
        }

        if (parts_to_add.empty() && replace)
        {
            parts_to_remove = removePartsInRangeFromWorkingSet(drop_range, true, data_parts_lock);
            String parts_to_remove_str;
            for (const auto & part : parts_to_remove)
            {
                parts_to_remove_str += part->name;
                parts_to_remove_str += " ";
            }
            LOG_TRACE(log, "Replacing {} parts {}with empty set", parts_to_remove.size(), parts_to_remove_str);
        }
    }

    if (parts_to_add.empty())
    {
        LOG_INFO(log, "All parts from REPLACE PARTITION command have been already attached");
        tryRemovePartsFromZooKeeperWithRetries(parts_to_remove);
        return true;
    }

    if (parts_to_add.size() < all_parts.size())
    {
        LOG_WARNING(log, "Some (but not all) parts from REPLACE PARTITION command already exist. REPLACE PARTITION will not be atomic.");
    }

    StoragePtr source_table;
    TableLockHolder table_lock_holder_src_table;
    StorageID source_table_id{entry_replace.from_database, entry_replace.from_table};

    auto clone_data_parts_from_source_table = [&] () -> size_t
    {
        source_table = DatabaseCatalog::instance().tryGetTable(source_table_id, getContext());
        if (!source_table)
        {
            LOG_DEBUG(log, "Can't use {} as source table for REPLACE PARTITION command. It does not exist.", source_table_id.getNameForLogs());
            return 0;
        }

        auto src_metadata_snapshot = source_table->getInMemoryMetadataPtr();
        MergeTreeData * src_data = nullptr;
        try
        {
            src_data = &checkStructureAndGetMergeTreeData(source_table, src_metadata_snapshot, dst_metadata_snapshot);
        }
        catch (Exception &)
        {
            LOG_INFO(log, "Can't use {} as source table for REPLACE PARTITION command. Will fetch all parts. Reason: {}", source_table_id.getNameForLogs(), getCurrentExceptionMessage(false));
            return 0;
        }

        table_lock_holder_src_table = source_table->lockForShare(
                RWLockImpl::NO_QUERY, getSettings()->lock_acquire_timeout_for_background_operations);

        DataPartStates valid_states{
            MergeTreeDataPartState::PreCommitted, MergeTreeDataPartState::Committed, MergeTreeDataPartState::Outdated};

        size_t num_clonable_parts = 0;
        for (PartDescriptionPtr & part_desc : parts_to_add)
        {
            auto src_part = src_data->getPartIfExists(part_desc->src_part_info, valid_states);
            if (!src_part)
            {
                LOG_DEBUG(log, "There is no part {} in {}", part_desc->src_part_name, source_table_id.getNameForLogs());
                continue;
            }

            String checksum_hex  = src_part->checksums.getTotalChecksumHex();

            if (checksum_hex != part_desc->checksum_hex)
            {
                LOG_DEBUG(log, "Part {} of {} has inappropriate checksum", part_desc->src_part_name, source_table_id.getNameForLogs());
                /// TODO: check version
                continue;
            }

            part_desc->found_new_part_name = part_desc->new_part_name;
            part_desc->found_new_part_info = part_desc->new_part_info;
            part_desc->src_table_part = src_part;

            ++num_clonable_parts;
        }

        return num_clonable_parts;
    };

    size_t num_clonable_parts = clone_data_parts_from_source_table();
    LOG_DEBUG(log, "Found {} parts that could be cloned (of {} required parts)", num_clonable_parts, parts_to_add.size());

    ActiveDataPartSet adding_parts_active_set(format_version);
    std::unordered_map<String, PartDescriptionPtr> part_name_to_desc;

    for (PartDescriptionPtr & part_desc : parts_to_add)
    {
        if (part_desc->src_table_part)
        {
            /// It is clonable part
            adding_parts_active_set.add(part_desc->new_part_name);
            part_name_to_desc.emplace(part_desc->new_part_name, part_desc);
            continue;
        }

        /// Firstly, try find exact part to produce more accurate part set
        String replica = findReplicaHavingPart(part_desc->new_part_name, true);
        String found_part_name;
        /// TODO: check version

        if (replica.empty())
        {
            LOG_DEBUG(log, "Part {} is not found on remote replicas", part_desc->new_part_name);

            /// Fallback to covering part
            replica = findReplicaHavingCoveringPart(part_desc->new_part_name, true, found_part_name);

            if (replica.empty())
            {
                /// It is not fail, since adjacent parts could cover current part
                LOG_DEBUG(log, "Parts covering {} are not found on remote replicas", part_desc->new_part_name);
                continue;
            }
        }
        else
        {
            found_part_name = part_desc->new_part_name;
        }

        part_desc->found_new_part_name = found_part_name;
        part_desc->found_new_part_info = MergeTreePartInfo::fromPartName(found_part_name, format_version);
        part_desc->replica = replica;

        adding_parts_active_set.add(part_desc->found_new_part_name);
        part_name_to_desc.emplace(part_desc->found_new_part_name, part_desc);
    }

    /// Check that we could cover whole range
    for (PartDescriptionPtr & part_desc : parts_to_add)
    {
        if (adding_parts_active_set.getContainingPart(part_desc->new_part_info).empty())
        {
            throw Exception("Not found part " + part_desc->new_part_name +
                            " (or part covering it) neither source table neither remote replicas" , ErrorCodes::NO_REPLICA_HAS_PART);
        }
    }

    /// Filter covered parts
    PartDescriptions final_parts;
    Strings final_part_names;
    {
        final_part_names = adding_parts_active_set.getParts();

        for (const String & final_part_name : final_part_names)
        {
            auto part_desc = part_name_to_desc[final_part_name];
            if (!part_desc)
                throw Exception("There is no final part " + final_part_name + ". This is a bug", ErrorCodes::LOGICAL_ERROR);

            final_parts.emplace_back(part_desc);

            if (final_parts.size() > 1)
            {
                auto & prev = *final_parts[final_parts.size() - 2];
                auto & curr = *final_parts[final_parts.size() - 1];

                if (!prev.found_new_part_info.isDisjoint(curr.found_new_part_info))
                {
                    throw Exception("Intersected final parts detected: " + prev.found_new_part_name
                        + " and " + curr.found_new_part_name + ". It should be investigated.", ErrorCodes::LOGICAL_ERROR);
                }
            }
        }
    }

    static const String TMP_PREFIX = "tmp_replace_from_";

    auto obtain_part = [&] (PartDescriptionPtr & part_desc)
    {
        if (part_desc->src_table_part)
        {

            if (part_desc->checksum_hex != part_desc->src_table_part->checksums.getTotalChecksumHex())
                throw Exception("Checksums of " + part_desc->src_table_part->name + " is suddenly changed", ErrorCodes::UNFINISHED);

            part_desc->res_part = cloneAndLoadDataPartOnSameDisk(
                part_desc->src_table_part, TMP_PREFIX + "clone_", part_desc->new_part_info, metadata_snapshot);
        }
        else if (!part_desc->replica.empty())
        {
            String source_replica_path = fs::path(zookeeper_path) / "replicas" / part_desc->replica;
            ReplicatedMergeTreeAddress address(getZooKeeper()->get(fs::path(source_replica_path) / "host"));
            auto timeouts = getFetchPartHTTPTimeouts(getContext());

            auto credentials = getContext()->getInterserverCredentials();
            String interserver_scheme = getContext()->getInterserverScheme();

            if (interserver_scheme != address.scheme)
                throw Exception("Interserver schemas are different '" + interserver_scheme + "' != '" + address.scheme + "', can't fetch part from " + address.host, ErrorCodes::LOGICAL_ERROR);

            part_desc->res_part = fetcher.fetchPart(
                metadata_snapshot, getContext(), part_desc->found_new_part_name, source_replica_path,
                address.host, address.replication_port, timeouts, credentials->getUser(), credentials->getPassword(),
                interserver_scheme, replicated_fetches_throttler, false, TMP_PREFIX + "fetch_");

            /// TODO: check columns_version of fetched part

            ProfileEvents::increment(ProfileEvents::ReplicatedPartFetches);
        }
        else
            throw Exception("There is no receipt to produce part " + part_desc->new_part_name + ". This is bug", ErrorCodes::LOGICAL_ERROR);
    };

    /// Download or clone parts
    /// TODO: make it in parallel
    for (PartDescriptionPtr & part_desc : final_parts)
        obtain_part(part_desc);

    MutableDataPartsVector res_parts;
    for (PartDescriptionPtr & part_desc : final_parts)
        res_parts.emplace_back(part_desc->res_part);

    try
    {
        /// Commit parts
        auto zookeeper = getZooKeeper();
        Transaction transaction(*this);

        Coordination::Requests ops;
        for (PartDescriptionPtr & part_desc : final_parts)
        {
            renameTempPartAndReplace(part_desc->res_part, nullptr, &transaction);
            getCommitPartOps(ops, part_desc->res_part);

            if (ops.size() > zkutil::MULTI_BATCH_SIZE)
            {
                zookeeper->multi(ops);
                ops.clear();
            }
        }

        if (!ops.empty())
            zookeeper->multi(ops);

        {
            auto data_parts_lock = lockParts();

            transaction.commit(&data_parts_lock);
            if (replace)
            {
                parts_to_remove = removePartsInRangeFromWorkingSet(drop_range, true, data_parts_lock);
                String parts_to_remove_str;
                for (const auto & part : parts_to_remove)
                {
                    parts_to_remove_str += part->name;
                    parts_to_remove_str += " ";
                }
                LOG_TRACE(log, "Replacing {} parts {}with {} parts {}", parts_to_remove.size(), parts_to_remove_str,
                          final_parts.size(), boost::algorithm::join(final_part_names, ", "));
            }
        }

        PartLog::addNewParts(getContext(), res_parts, watch.elapsed());
    }
    catch (...)
    {
        PartLog::addNewParts(getContext(), res_parts, watch.elapsed(), ExecutionStatus::fromCurrentException());
        throw;
    }

    tryRemovePartsFromZooKeeperWithRetries(parts_to_remove);
    res_parts.clear();
    parts_to_remove.clear();
    cleanup_thread.wakeup();

    return true;
}


void StorageReplicatedMergeTree::executeClonePartFromShard(const LogEntry & entry)
{
    auto zookeeper = getZooKeeper();

    Strings replicas = zookeeper->getChildren(entry.source_shard + "/replicas");
    std::shuffle(replicas.begin(), replicas.end(), thread_local_rng);
    String replica;
    for (const String & candidate : replicas)
    {
        if (zookeeper->exists(entry.source_shard + "/replicas/" + candidate + "/is_active"))
        {
            replica = candidate;
            break;
        }
    }

    if (replica.empty())
        throw Exception(ErrorCodes::NO_REPLICA_HAS_PART, "Not found active replica on shard {} to clone part {}", entry.source_shard, entry.new_part_name);

    LOG_INFO(log, "Will clone part from shard " + entry.source_shard + " and replica " + replica);

    MutableDataPartPtr part;

    {
        auto metadata_snapshot = getInMemoryMetadataPtr();
        String source_replica_path = entry.source_shard + "/replicas/" + replica;
        ReplicatedMergeTreeAddress address(getZooKeeper()->get(source_replica_path + "/host"));
        auto timeouts = ConnectionTimeouts::getHTTPTimeouts(getContext());
        auto credentials = getContext()->getInterserverCredentials();
        String interserver_scheme = getContext()->getInterserverScheme();

        auto get_part = [&, address, timeouts, credentials, interserver_scheme]()
        {
            if (interserver_scheme != address.scheme)
                throw Exception("Interserver schemes are different: '" + interserver_scheme
                                + "' != '" + address.scheme + "', can't fetch part from " + address.host,
                                ErrorCodes::LOGICAL_ERROR);

            return fetcher.fetchPart(
                metadata_snapshot, getContext(), entry.new_part_name, source_replica_path,
                address.host, address.replication_port,
                timeouts, credentials->getUser(), credentials->getPassword(), interserver_scheme,
                replicated_fetches_throttler, true);
        };

        part = get_part();
        // The fetched part is valuable and should not be cleaned like a temp part.
        part->is_temp = false;
        part->renameTo("detached/" + entry.new_part_name, true);
        LOG_INFO(log, "Cloned part {} to detached directory", part->name);
    }
}


void StorageReplicatedMergeTree::cloneReplica(const String & source_replica, Coordination::Stat source_is_lost_stat, zkutil::ZooKeeperPtr & zookeeper)
{
    String source_path = fs::path(zookeeper_path) / "replicas" / source_replica;

    /// The order of the following three actions is important.

    Strings source_queue_names;
    /// We are trying to get consistent /log_pointer and /queue state. Otherwise
    /// we can possibly duplicate entries in queue of cloned replica.
    while (true)
    {
        Coordination::Stat log_pointer_stat;
        String raw_log_pointer = zookeeper->get(fs::path(source_path) / "log_pointer", &log_pointer_stat);

        Coordination::Requests ops;
        ops.push_back(zkutil::makeSetRequest(fs::path(replica_path) / "log_pointer", raw_log_pointer, -1));

        /// For support old versions CH.
        if (source_is_lost_stat.version == -1)
        {
            /// We check that it was not suddenly upgraded to new version.
            /// Otherwise it can be upgraded and instantly become lost, but we cannot notice that.
            ops.push_back(zkutil::makeCreateRequest(fs::path(source_path) / "is_lost", "0", zkutil::CreateMode::Persistent));
            ops.push_back(zkutil::makeRemoveRequest(fs::path(source_path) / "is_lost", -1));
        }
        else /// The replica we clone should not suddenly become lost.
            ops.push_back(zkutil::makeCheckRequest(fs::path(source_path) / "is_lost", source_is_lost_stat.version));

        Coordination::Responses responses;

        /// Let's remember the queue of the reference/master replica.
        source_queue_names = zookeeper->getChildren(fs::path(source_path) / "queue");

        /// Check that log pointer of source replica didn't changed while we read queue entries
        ops.push_back(zkutil::makeCheckRequest(fs::path(source_path) / "log_pointer", log_pointer_stat.version));

        auto rc = zookeeper->tryMulti(ops, responses);

        if (rc == Coordination::Error::ZOK)
        {
            break;
        }
        else if (rc == Coordination::Error::ZNODEEXISTS)
        {
            throw Exception(
                "Can not clone replica, because the " + source_replica + " updated to new ClickHouse version",
                ErrorCodes::REPLICA_STATUS_CHANGED);
        }
        else if (responses[1]->error == Coordination::Error::ZBADVERSION)
        {
            /// If is_lost node version changed than source replica also lost,
            /// so we cannot clone from it.
            throw Exception(
                "Can not clone replica, because the " + source_replica + " became lost", ErrorCodes::REPLICA_STATUS_CHANGED);
        }
        else if (responses.back()->error == Coordination::Error::ZBADVERSION)
        {
            /// If source replica's log_pointer changed than we probably read
            /// stale state of /queue and have to try one more time.
            LOG_WARNING(log, "Log pointer of source replica {} changed while we loading queue nodes. Will retry.", source_replica);
            continue;
        }
        else
        {
            zkutil::KeeperMultiException::check(rc, ops, responses);
        }
    }

    std::sort(source_queue_names.begin(), source_queue_names.end());

    Strings source_queue;
    for (const String & entry_name : source_queue_names)
    {
        String entry;
        if (!zookeeper->tryGet(fs::path(source_path) / "queue" / entry_name, entry))
            continue;
        source_queue.push_back(entry);
    }

    /// We should do it after copying queue, because some ALTER_METADATA entries can be lost otherwise.
    cloneMetadataIfNeeded(source_replica, source_path, zookeeper);

    /// Add to the queue jobs to receive all the active parts that the reference/master replica has.
    Strings source_replica_parts = zookeeper->getChildren(fs::path(source_path) / "parts");
    ActiveDataPartSet active_parts_set(format_version, source_replica_parts);

    Strings active_parts = active_parts_set.getParts();

    /// Remove local parts if source replica does not have them, because such parts will never be fetched by other replicas.
    Strings local_parts_in_zk = zookeeper->getChildren(fs::path(replica_path) / "parts");
    Strings parts_to_remove_from_zk;

    for (const auto & part : local_parts_in_zk)
    {
        if (active_parts_set.getContainingPart(part).empty())
        {
            parts_to_remove_from_zk.emplace_back(part);
            LOG_WARNING(log, "Source replica does not have part {}. Removing it from ZooKeeper.", part);
        }
    }

    {
        /// Check "is_lost" version after retrieving queue and parts.
        /// If version has changed, then replica most likely has been dropped and parts set is inconsistent,
        /// so throw exception and retry cloning.
        Coordination::Stat is_lost_stat_new;
        zookeeper->get(fs::path(source_path) / "is_lost", &is_lost_stat_new);
        if (is_lost_stat_new.version != source_is_lost_stat.version)
            throw Exception(ErrorCodes::REPLICA_STATUS_CHANGED, "Cannot clone {}, because it suddenly become lost", source_replica);
    }

    tryRemovePartsFromZooKeeperWithRetries(parts_to_remove_from_zk);

    auto local_active_parts = getDataParts();

    DataPartsVector parts_to_remove_from_working_set;

    for (const auto & part : local_active_parts)
    {
        if (active_parts_set.getContainingPart(part->name).empty())
        {
            parts_to_remove_from_working_set.emplace_back(part);
            LOG_WARNING(log, "Source replica does not have part {}. Removing it from working set.", part->name);
        }
    }

    if (getSettings()->detach_old_local_parts_when_cloning_replica)
    {
        auto metadata_snapshot = getInMemoryMetadataPtr();

        for (const auto & part : parts_to_remove_from_working_set)
        {
            LOG_INFO(log, "Detaching {}", part->relative_path);
            part->makeCloneInDetached("clone", metadata_snapshot);
        }
    }

    removePartsFromWorkingSet(parts_to_remove_from_working_set, true);

    for (const String & name : active_parts)
    {
        LogEntry log_entry;

        if (!are_restoring_replica)
            log_entry.type = LogEntry::GET_PART;
        else
        {
            LOG_DEBUG(log, "Obtaining checksum for path {}", name);

            // The part we want to fetch is probably present in detached/ folder.
            // However, we need to get part's checksum to check if it's not corrupt.
            log_entry.type = LogEntry::ATTACH_PART;

            MinimalisticDataPartChecksums desired_checksums;

            const fs::path part_path = fs::path(source_path) / "parts" / name;

            const String part_znode = zookeeper->get(part_path);

            if (!part_znode.empty())
                desired_checksums = ReplicatedMergeTreePartHeader::fromString(part_znode).getChecksums();
            else
            {
                String desired_checksums_str = zookeeper->get(part_path / "checksums");
                desired_checksums = MinimalisticDataPartChecksums::deserializeFrom(desired_checksums_str);
            }

            const auto [lo, hi] = desired_checksums.hash_of_all_files;
            log_entry.part_checksum = getHexUIntUppercase(hi) + getHexUIntUppercase(lo);
        }

        log_entry.source_replica = "";
        log_entry.new_part_name = name;
        log_entry.create_time = tryGetPartCreateTime(zookeeper, source_path, name);

        zookeeper->create(fs::path(replica_path) / "queue/queue-", log_entry.toString(), zkutil::CreateMode::PersistentSequential);
    }

    LOG_DEBUG(log, "Queued {} parts to be fetched", active_parts.size());

    /// Add content of the reference/master replica queue to the queue.
    for (const String & entry : source_queue)
    {
        zookeeper->create(fs::path(replica_path) / "queue/queue-", entry, zkutil::CreateMode::PersistentSequential);
    }

    LOG_DEBUG(log, "Copied {} queue entries", source_queue.size());
}


void StorageReplicatedMergeTree::cloneMetadataIfNeeded(const String & source_replica, const String & source_path, zkutil::ZooKeeperPtr & zookeeper)
{
    String source_metadata_version_str;
    bool metadata_version_exists = zookeeper->tryGet(source_path + "/metadata_version", source_metadata_version_str);
    if (!metadata_version_exists)
    {
        /// For compatibility with version older than 20.3
        /// TODO fix tests and delete it
        LOG_WARNING(log, "Node {} does not exist. "
                         "Most likely it's because too old version of ClickHouse is running on replica {}. "
                         "Will not check metadata consistency",
                         source_path + "/metadata_version", source_replica);
        return;
    }

    Int32 source_metadata_version = parse<Int32>(source_metadata_version_str);
    if (metadata_version == source_metadata_version)
        return;

    /// Our metadata it not up to date with source replica metadata.
    /// Metadata is updated by ALTER_METADATA entries, but some entries are probably cleaned up from the log.
    /// It's also possible that some newer ALTER_METADATA entries are present in source_queue list,
    /// and source replica are executing such entry right now (or had executed recently).
    /// More than that, /metadata_version update is not atomic with /columns and /metadata update...

    /// Fortunately, ALTER_METADATA seems to be idempotent,
    /// and older entries of such type can be replaced with newer entries.
    /// Let's try to get consistent values of source replica's /columns and /metadata
    /// and prepend dummy ALTER_METADATA to our replication queue.
    /// It should not break anything if source_queue already contains ALTER_METADATA entry
    /// with greater or equal metadata_version, but it will update our metadata
    /// if all such entries were cleaned up from the log and source_queue.

    LOG_WARNING(log, "Metadata version ({}) on replica is not up to date with metadata ({}) on source replica {}",
                metadata_version, source_metadata_version, source_replica);

    String source_metadata;
    String source_columns;
    while (true)
    {
        Coordination::Stat metadata_stat;
        Coordination::Stat columns_stat;
        source_metadata = zookeeper->get(source_path + "/metadata", &metadata_stat);
        source_columns = zookeeper->get(source_path + "/columns", &columns_stat);

        Coordination::Requests ops;
        Coordination::Responses responses;
        ops.emplace_back(zkutil::makeCheckRequest(source_path + "/metadata", metadata_stat.version));
        ops.emplace_back(zkutil::makeCheckRequest(source_path + "/columns", columns_stat.version));

        Coordination::Error code = zookeeper->tryMulti(ops, responses);
        if (code == Coordination::Error::ZOK)
            break;
        else if (code == Coordination::Error::ZBADVERSION)
            LOG_WARNING(log, "Metadata of replica {} was changed", source_path);
        else
            zkutil::KeeperMultiException::check(code, ops, responses);
    }

    ReplicatedMergeTreeLogEntryData dummy_alter;
    dummy_alter.type = LogEntry::ALTER_METADATA;
    dummy_alter.source_replica = source_replica;
    dummy_alter.metadata_str = source_metadata;
    dummy_alter.columns_str = source_columns;
    dummy_alter.alter_version = source_metadata_version;
    dummy_alter.create_time = time(nullptr);

    zookeeper->create(replica_path + "/queue/queue-", dummy_alter.toString(), zkutil::CreateMode::PersistentSequential);

    /// We don't need to do anything with mutation_pointer, because mutation log cleanup process is different from
    /// replication log cleanup. A mutation is removed from ZooKeeper only if all replicas had executed the mutation,
    /// so all mutations which are greater or equal to our mutation pointer are still present in ZooKeeper.
}


void StorageReplicatedMergeTree::cloneReplicaIfNeeded(zkutil::ZooKeeperPtr zookeeper)
{
    Coordination::Stat is_lost_stat;
    bool is_new_replica = true;
    String res;

    if (zookeeper->tryGet(fs::path(replica_path) / "is_lost", res, &is_lost_stat))
    {
        if (res == "0")
            return;
        if (is_lost_stat.version)
            is_new_replica = false;
    }
    else
    {
        /// Replica was created by old version of CH, so me must create "/is_lost".
        /// Note that in old version of CH there was no "lost" replicas possible.
        /// TODO is_lost node should always exist since v18.12, maybe we can replace `tryGet` with `get` and remove old code?
        zookeeper->create(fs::path(replica_path) / "is_lost", "0", zkutil::CreateMode::Persistent);
        return;
    }

    /// is_lost is "1": it means that we are in repair mode.
    /// Try choose source replica to clone.
    /// Source replica must not be lost and should have minimal queue size and maximal log pointer.
    Strings replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");
    std::vector<zkutil::ZooKeeper::FutureGet> futures;
    for (const String & source_replica_name : replicas)
    {
        /// Do not clone from myself.
        if (source_replica_name == replica_name)
            continue;

        String source_replica_path = fs::path(zookeeper_path) / "replicas" / source_replica_name;

        /// Obviously the following get operations are not atomic, but it's ok to choose good enough replica, not the best one.
        /// NOTE: We may count some entries twice if log_pointer is moved.
        futures.emplace_back(zookeeper->asyncTryGet(fs::path(source_replica_path) / "is_lost"));
        futures.emplace_back(zookeeper->asyncTryGet(fs::path(source_replica_path) / "log_pointer"));
        futures.emplace_back(zookeeper->asyncTryGet(fs::path(source_replica_path) / "queue"));
    }

    /// Wait for results before getting log entries
    for (auto & future : futures)
        future.wait();

    Strings log_entries = zookeeper->getChildren(fs::path(zookeeper_path) / "log");
    size_t max_log_entry = 0;
    if (!log_entries.empty())
    {
        String last_entry = *std::max_element(log_entries.begin(), log_entries.end());
        max_log_entry = parse<UInt64>(last_entry.substr(strlen("log-")));
    }
    /// log_pointer can point to future entry, which was not created yet
    ++max_log_entry;

    size_t min_replication_lag = std::numeric_limits<size_t>::max();
    String source_replica;
    Coordination::Stat source_is_lost_stat;
    size_t future_num = 0;

    for (const String & source_replica_name : replicas)
    {
        if (source_replica_name == replica_name)
            continue;

        auto get_is_lost     = futures[future_num++].get();
        auto get_log_pointer = futures[future_num++].get();
        auto get_queue       = futures[future_num++].get();

        if (get_is_lost.error != Coordination::Error::ZOK)
        {
            LOG_INFO(log, "Not cloning {}, cannot get '/is_lost': {}", source_replica_name, Coordination::errorMessage(get_is_lost.error));
            continue;
        }
        else if (get_is_lost.data != "0")
        {
            LOG_INFO(log, "Not cloning {}, it's lost", source_replica_name);
            continue;
        }

        if (get_log_pointer.error != Coordination::Error::ZOK)
        {
            LOG_INFO(log, "Not cloning {}, cannot get '/log_pointer': {}", source_replica_name, Coordination::errorMessage(get_log_pointer.error));
            continue;
        }
        if (get_queue.error != Coordination::Error::ZOK)
        {
            LOG_INFO(log, "Not cloning {}, cannot get '/queue': {}", source_replica_name, Coordination::errorMessage(get_queue.error));
            continue;
        }

        /// Replica is not lost and we can clone it. Let's calculate approx replication lag.
        size_t source_log_pointer = get_log_pointer.data.empty() ? 0 : parse<UInt64>(get_log_pointer.data);
        assert(source_log_pointer <= max_log_entry);
        size_t replica_queue_lag = max_log_entry - source_log_pointer;
        size_t replica_queue_size = get_queue.stat.numChildren;
        size_t replication_lag = replica_queue_lag + replica_queue_size;
        LOG_INFO(log, "Replica {} has log pointer '{}', approximate {} queue lag and {} queue size",
                 source_replica_name, get_log_pointer.data, replica_queue_lag, replica_queue_size);
        if (replication_lag < min_replication_lag)
        {
            source_replica = source_replica_name;
            source_is_lost_stat = get_is_lost.stat;
            min_replication_lag = replication_lag;
        }
    }

    if (source_replica.empty())
        throw Exception("All replicas are lost", ErrorCodes::ALL_REPLICAS_LOST);

    if (is_new_replica)
        LOG_INFO(log, "Will mimic {}", source_replica);
    else
        LOG_WARNING(log, "Will mimic {}", source_replica);

    /// Clear obsolete queue that we no longer need.
    zookeeper->removeChildren(fs::path(replica_path) / "queue");
    queue.clear();

    /// Will do repair from the selected replica.
    cloneReplica(source_replica, source_is_lost_stat, zookeeper);
    /// If repair fails to whatever reason, the exception is thrown, is_lost will remain "1" and the replica will be repaired later.

    /// If replica is repaired successfully, we remove is_lost flag.
    zookeeper->set(fs::path(replica_path) / "is_lost", "0");
}

String StorageReplicatedMergeTree::getLastQueueUpdateException() const
{
    std::unique_lock lock(last_queue_update_exception_lock);
    return last_queue_update_exception;
}


void StorageReplicatedMergeTree::queueUpdatingTask()
{
    if (!queue_update_in_progress)
    {
        last_queue_update_start_time.store(time(nullptr));
        queue_update_in_progress = true;
    }
    try
    {
        queue.pullLogsToQueue(getZooKeeper(), queue_updating_task->getWatchCallback(), ReplicatedMergeTreeQueue::UPDATE);
        last_queue_update_finish_time.store(time(nullptr));
        queue_update_in_progress = false;
    }
    catch (const Coordination::Exception & e)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);

        std::unique_lock lock(last_queue_update_exception_lock);
        last_queue_update_exception = getCurrentExceptionMessage(false);

        if (e.code == Coordination::Error::ZSESSIONEXPIRED)
        {
            restarting_thread.wakeup();
            return;
        }

        queue_updating_task->scheduleAfter(QUEUE_UPDATE_ERROR_SLEEP_MS);
    }
    catch (...)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);

        std::unique_lock lock(last_queue_update_exception_lock);
        last_queue_update_exception = getCurrentExceptionMessage(false);

        queue_updating_task->scheduleAfter(QUEUE_UPDATE_ERROR_SLEEP_MS);
    }
}


void StorageReplicatedMergeTree::mutationsUpdatingTask()
{
    try
    {
        queue.updateMutations(getZooKeeper(), mutations_updating_task->getWatchCallback());
    }
    catch (const Coordination::Exception & e)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);

        if (e.code == Coordination::Error::ZSESSIONEXPIRED)
            return;

        mutations_updating_task->scheduleAfter(QUEUE_UPDATE_ERROR_SLEEP_MS);
    }
    catch (...)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
        mutations_updating_task->scheduleAfter(QUEUE_UPDATE_ERROR_SLEEP_MS);
    }
}

ReplicatedMergeTreeQueue::SelectedEntryPtr StorageReplicatedMergeTree::selectQueueEntry()
{
    /// This object will mark the element of the queue as running.
    ReplicatedMergeTreeQueue::SelectedEntryPtr selected;

    try
    {
        selected = queue.selectEntryToProcess(merger_mutator, *this);
    }
    catch (...)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
    }

    return selected;
}


bool StorageReplicatedMergeTree::processQueueEntry(ReplicatedMergeTreeQueue::SelectedEntryPtr selected_entry)
{
    LogEntryPtr & entry = selected_entry->log_entry;
    return queue.processEntry([this]{ return getZooKeeper(); }, entry, [&](LogEntryPtr & entry_to_process)
    {
        try
        {
            return executeLogEntry(*entry_to_process);
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::NO_REPLICA_HAS_PART)
            {
                /// If no one has the right part, probably not all replicas work; We will not write to log with Error level.
                LOG_INFO(log, e.displayText());
            }
            else if (e.code() == ErrorCodes::ABORTED)
            {
                /// Interrupted merge or downloading a part is not an error.
                LOG_INFO(log, e.message());
            }
            else if (e.code() == ErrorCodes::PART_IS_TEMPORARILY_LOCKED)
            {
                /// Part cannot be added temporarily
                LOG_INFO(log, e.displayText());
                cleanup_thread.wakeup();
            }
            else
                tryLogCurrentException(log, __PRETTY_FUNCTION__);

            /** This exception will be written to the queue element, and it can be looked up using `system.replication_queue` table.
              * The thread that performs this action will sleep a few seconds after the exception.
              * See `queue.processEntry` function.
              */
            throw;
        }
        catch (...)
        {
            tryLogCurrentException(log, __PRETTY_FUNCTION__);
            throw;
        }
    });
}

bool StorageReplicatedMergeTree::scheduleDataProcessingJob(BackgroundJobsAssignee & assignee)
{
    /// If replication queue is stopped exit immediately as we successfully executed the task
    if (queue.actions_blocker.isCancelled())
        return false;

    /// This object will mark the element of the queue as running.
    ReplicatedMergeTreeQueue::SelectedEntryPtr selected_entry = selectQueueEntry();

    if (!selected_entry)
        return false;

    auto job_type = selected_entry->log_entry->type;

    /// Depending on entry type execute in fetches (small) pool or big merge_mutate pool
    if (job_type == LogEntry::GET_PART)
    {
        assignee.scheduleFetchTask(ExecutableLambdaAdapter::create(
            [this, selected_entry] () mutable
            {
                return processQueueEntry(selected_entry);
            }, common_assignee_trigger, getStorageID()));
        return true;
    }
    else if (job_type == LogEntry::MERGE_PARTS)
    {
        auto task = MergeFromLogEntryTask::create(selected_entry, *this, common_assignee_trigger);
        assignee.scheduleMergeMutateTask(task);
        return true;
    }
    else if (job_type == LogEntry::MUTATE_PART)
    {
        auto task = MutateFromLogEntryTask::create(selected_entry, *this, common_assignee_trigger);
        assignee.scheduleMergeMutateTask(task);
        return true;
    }
    else
    {
        assignee.scheduleCommonTask(ExecutableLambdaAdapter::create(
            [this, selected_entry] () mutable
            {
                return processQueueEntry(selected_entry);
            }, common_assignee_trigger, getStorageID()));
        return true;
    }
}


bool StorageReplicatedMergeTree::canExecuteFetch(const ReplicatedMergeTreeLogEntry & entry, String & disable_reason) const
{
    if (fetcher.blocker.isCancelled())
    {
        disable_reason = fmt::format("Not executing fetch of part {} because replicated fetches are cancelled now.", entry.new_part_name);
        return false;
    }

    size_t busy_threads_in_pool = CurrentMetrics::values[CurrentMetrics::BackgroundFetchesPoolTask].load(std::memory_order_relaxed);
    if (busy_threads_in_pool >= replicated_fetches_pool_size)
    {
        disable_reason = fmt::format("Not executing fetch of part {} because {} fetches already executing, max {}.", entry.new_part_name, busy_threads_in_pool, replicated_fetches_pool_size);
        return false;
    }

    if (replicated_fetches_throttler->isThrottling())
    {
        disable_reason = fmt::format("Not executing fetch of part {} because fetches have already throttled by network settings "
                                     "<max_replicated_fetches_network_bandwidth> or <max_replicated_fetches_network_bandwidth_for_server>.", entry.new_part_name);
        return false;
    }

    return true;
}

bool StorageReplicatedMergeTree::partIsAssignedToBackgroundOperation(const DataPartPtr & part) const
{
    return queue.isVirtualPart(part);
}

void StorageReplicatedMergeTree::mergeSelectingTask()
{
    if (!is_leader)
        return;

    const auto storage_settings_ptr = getSettings();
    const bool deduplicate = false; /// TODO: read deduplicate option from table config
    const Names deduplicate_by_columns = {};
    CreateMergeEntryResult create_result = CreateMergeEntryResult::Other;

    try
    {
        /// We must select parts for merge under merge_selecting_mutex because other threads
        /// (OPTIMIZE queries) can assign new merges.
        std::lock_guard merge_selecting_lock(merge_selecting_mutex);

        auto zookeeper = getZooKeeper();

        ReplicatedMergeTreeMergePredicate merge_pred = queue.getMergePredicate(zookeeper);

        /// If many merges is already queued, then will queue only small enough merges.
        /// Otherwise merge queue could be filled with only large merges,
        /// and in the same time, many small parts could be created and won't be merged.

        auto merges_and_mutations_queued = queue.countMergesAndPartMutations();
        size_t merges_and_mutations_sum = merges_and_mutations_queued.merges + merges_and_mutations_queued.mutations;
        if (merges_and_mutations_sum >= storage_settings_ptr->max_replicated_merges_in_queue)
        {
            LOG_TRACE(log, "Number of queued merges ({}) and part mutations ({})"
                " is greater than max_replicated_merges_in_queue ({}), so won't select new parts to merge or mutate.",
                merges_and_mutations_queued.merges,
                merges_and_mutations_queued.mutations,
                storage_settings_ptr->max_replicated_merges_in_queue);
        }
        else
        {
            UInt64 max_source_parts_size_for_merge = merger_mutator.getMaxSourcePartsSizeForMerge(
                storage_settings_ptr->max_replicated_merges_in_queue, merges_and_mutations_sum);

            UInt64 max_source_part_size_for_mutation = merger_mutator.getMaxSourcePartSizeForMutation();

            bool merge_with_ttl_allowed = merges_and_mutations_queued.merges_with_ttl < storage_settings_ptr->max_replicated_merges_with_ttl_in_queue &&
                getTotalMergesWithTTLInMergeList() < storage_settings_ptr->max_number_of_merges_with_ttl_in_pool;

            auto future_merged_part = std::make_shared<FutureMergedMutatedPart>();
            if (storage_settings.get()->assign_part_uuids)
                future_merged_part->uuid = UUIDHelpers::generateV4();

            if (max_source_parts_size_for_merge > 0 &&
                merger_mutator.selectPartsToMerge(future_merged_part, false, max_source_parts_size_for_merge, merge_pred, merge_with_ttl_allowed, nullptr) == SelectPartsDecision::SELECTED)
            {
                create_result = createLogEntryToMergeParts(
                    zookeeper,
                    future_merged_part->parts,
                    future_merged_part->name,
                    future_merged_part->uuid,
                    future_merged_part->type,
                    deduplicate,
                    deduplicate_by_columns,
                    nullptr,
                    merge_pred.getVersion(),
                    future_merged_part->merge_type);
            }
            /// If there are many mutations in queue, it may happen, that we cannot enqueue enough merges to merge all new parts
            else if (max_source_part_size_for_mutation > 0 && queue.countMutations() > 0
                     && merges_and_mutations_queued.mutations < storage_settings_ptr->max_replicated_mutations_in_queue)
            {
                /// Choose a part to mutate.
                DataPartsVector data_parts = getDataPartsVector();
                for (const auto & part : data_parts)
                {
                    if (part->getBytesOnDisk() > max_source_part_size_for_mutation)
                        continue;

                    std::optional<std::pair<Int64, int>> desired_mutation_version = merge_pred.getDesiredMutationVersion(part);
                    if (!desired_mutation_version)
                        continue;

                    create_result = createLogEntryToMutatePart(
                        *part,
                        future_merged_part->uuid,
                        desired_mutation_version->first,
                        desired_mutation_version->second,
                        merge_pred.getVersion());

                    if (create_result == CreateMergeEntryResult::Ok)
                        break;
                }
            }
        }
    }
    catch (...)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
    }

    if (!is_leader)
        return;

    if (create_result != CreateMergeEntryResult::Ok
        && create_result != CreateMergeEntryResult::LogUpdated)
    {
        merge_selecting_task->scheduleAfter(storage_settings_ptr->merge_selecting_sleep_ms);
    }
    else
    {
        merge_selecting_task->schedule();
    }
}


void StorageReplicatedMergeTree::mutationsFinalizingTask()
{
    bool needs_reschedule = false;

    try
    {
        needs_reschedule = queue.tryFinalizeMutations(getZooKeeper());
    }
    catch (...)
    {
        tryLogCurrentException(log, __PRETTY_FUNCTION__);
        needs_reschedule = true;
    }

    if (needs_reschedule)
    {
        mutations_finalizing_task->scheduleAfter(MUTATIONS_FINALIZING_SLEEP_MS);
    }
    else
    {
        /// Even if no mutations seems to be done or appeared we are trying to
        /// finalize them in background because manual control the launch of
        /// this function is error prone. This can lead to mutations that
        /// processed all the parts but have is_done=0 state for a long time. Or
        /// killed mutations, which are also considered as undone.
        mutations_finalizing_task->scheduleAfter(MUTATIONS_FINALIZING_IDLE_SLEEP_MS);
    }
}


StorageReplicatedMergeTree::CreateMergeEntryResult StorageReplicatedMergeTree::createLogEntryToMergeParts(
    zkutil::ZooKeeperPtr & zookeeper,
    const DataPartsVector & parts,
    const String & merged_name,
    const UUID & merged_part_uuid,
    const MergeTreeDataPartType & merged_part_type,
    bool deduplicate,
    const Names & deduplicate_by_columns,
    ReplicatedMergeTreeLogEntryData * out_log_entry,
    int32_t log_version,
    MergeType merge_type)
{
    std::vector<std::future<Coordination::ExistsResponse>> exists_futures;
    exists_futures.reserve(parts.size());
    for (const auto & part : parts)
        exists_futures.emplace_back(zookeeper->asyncExists(fs::path(replica_path) / "parts" / part->name));

    bool all_in_zk = true;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        /// If there is no information about part in ZK, we will not merge it.
        if (exists_futures[i].get().error == Coordination::Error::ZNONODE)
        {
            all_in_zk = false;

            const auto & part = parts[i];
            if (part->modification_time + MAX_AGE_OF_LOCAL_PART_THAT_WASNT_ADDED_TO_ZOOKEEPER < time(nullptr))
            {
                LOG_WARNING(log, "Part {} (that was selected for merge) with age {} seconds exists locally but not in ZooKeeper. Won't do merge with that part and will check it.", part->name, (time(nullptr) - part->modification_time));
                enqueuePartForCheck(part->name);
            }
        }
    }

    if (!all_in_zk)
        return CreateMergeEntryResult::MissingPart;

    ReplicatedMergeTreeLogEntryData entry;
    entry.type = LogEntry::MERGE_PARTS;
    entry.source_replica = replica_name;
    entry.new_part_name = merged_name;
    entry.new_part_uuid = merged_part_uuid;
    entry.new_part_type = merged_part_type;
    entry.merge_type = merge_type;
    entry.deduplicate = deduplicate;
    entry.deduplicate_by_columns = deduplicate_by_columns;
    entry.create_time = time(nullptr);

    for (const auto & part : parts)
        entry.source_parts.push_back(part->name);

    Coordination::Requests ops;
    Coordination::Responses responses;

    ops.emplace_back(zkutil::makeCreateRequest(
        fs::path(zookeeper_path) / "log/log-", entry.toString(),
        zkutil::CreateMode::PersistentSequential));

    ops.emplace_back(zkutil::makeSetRequest(
        fs::path(zookeeper_path) / "log", "", log_version)); /// Check and update version.

    Coordination::Error code = zookeeper->tryMulti(ops, responses);

    if (code == Coordination::Error::ZOK)
    {
        String path_created = dynamic_cast<const Coordination::CreateResponse &>(*responses.front()).path_created;
        entry.znode_name = path_created.substr(path_created.find_last_of('/') + 1);

        ProfileEvents::increment(ProfileEvents::CreatedLogEntryForMerge);
        LOG_TRACE(log, "Created log entry {} for merge {}", path_created, merged_name);
    }
    else if (code == Coordination::Error::ZBADVERSION)
    {
        ProfileEvents::increment(ProfileEvents::NotCreatedLogEntryForMerge);
        LOG_TRACE(log, "Log entry is not created for merge {} because log was updated", merged_name);
        return CreateMergeEntryResult::LogUpdated;
    }
    else
    {
        zkutil::KeeperMultiException::check(code, ops, responses);
    }

    if (out_log_entry)
        *out_log_entry = entry;

    return CreateMergeEntryResult::Ok;
}


StorageReplicatedMergeTree::CreateMergeEntryResult StorageReplicatedMergeTree::createLogEntryToMutatePart(
    const IMergeTreeDataPart & part, const UUID & new_part_uuid, Int64 mutation_version, int32_t alter_version, int32_t log_version)
{
    auto zookeeper = getZooKeeper();

    /// If there is no information about part in ZK, we will not mutate it.
    if (!zookeeper->exists(fs::path(replica_path) / "parts" / part.name))
    {
        if (part.modification_time + MAX_AGE_OF_LOCAL_PART_THAT_WASNT_ADDED_TO_ZOOKEEPER < time(nullptr))
        {
            LOG_WARNING(log, "Part {} (that was selected for mutation) with age {} seconds exists locally but not in ZooKeeper."
                " Won't mutate that part and will check it.", part.name, (time(nullptr) - part.modification_time));
            enqueuePartForCheck(part.name);
        }

        return CreateMergeEntryResult::MissingPart;
    }

    MergeTreePartInfo new_part_info = part.info;
    new_part_info.mutation = mutation_version;

    String new_part_name = part.getNewName(new_part_info);

    ReplicatedMergeTreeLogEntryData entry;
    entry.type = LogEntry::MUTATE_PART;
    entry.source_replica = replica_name;
    entry.source_parts.push_back(part.name);
    entry.new_part_name = new_part_name;
    entry.new_part_uuid = new_part_uuid;
    entry.create_time = time(nullptr);
    entry.alter_version = alter_version;

    Coordination::Requests ops;
    Coordination::Responses responses;

    ops.emplace_back(zkutil::makeCreateRequest(
        fs::path(zookeeper_path) / "log/log-", entry.toString(),
        zkutil::CreateMode::PersistentSequential));

    ops.emplace_back(zkutil::makeSetRequest(
        fs::path(zookeeper_path) / "log", "", log_version)); /// Check and update version.

    Coordination::Error code = zookeeper->tryMulti(ops, responses);

    if (code == Coordination::Error::ZBADVERSION)
    {
        ProfileEvents::increment(ProfileEvents::NotCreatedLogEntryForMutation);
        LOG_TRACE(log, "Log entry is not created for mutation {} because log was updated", new_part_name);
        return CreateMergeEntryResult::LogUpdated;
    }

    zkutil::KeeperMultiException::check(code, ops, responses);

    ProfileEvents::increment(ProfileEvents::CreatedLogEntryForMutation);
    LOG_TRACE(log, "Created log entry for mutation {}", new_part_name);
    return CreateMergeEntryResult::Ok;
}


void StorageReplicatedMergeTree::removePartFromZooKeeper(const String & part_name, Coordination::Requests & ops, bool has_children)
{
    String part_path = fs::path(replica_path) / "parts" / part_name;

    if (has_children)
    {
        ops.emplace_back(zkutil::makeRemoveRequest(fs::path(part_path) / "checksums", -1));
        ops.emplace_back(zkutil::makeRemoveRequest(fs::path(part_path) / "columns", -1));
    }
    ops.emplace_back(zkutil::makeRemoveRequest(part_path, -1));
}

void StorageReplicatedMergeTree::removePartFromZooKeeper(const String & part_name)
{
    auto zookeeper = getZooKeeper();
    String part_path = fs::path(replica_path) / "parts" / part_name;
    Coordination::Stat stat;

    /// Part doesn't exist, nothing to remove
    if (!zookeeper->exists(part_path, &stat))
        return;

    Coordination::Requests ops;

    removePartFromZooKeeper(part_name, ops, stat.numChildren > 0);
    zookeeper->multi(ops);
}

void StorageReplicatedMergeTree::removePartAndEnqueueFetch(const String & part_name)
{
    auto zookeeper = getZooKeeper();

    String part_path = fs::path(replica_path) / "parts" / part_name;

    Coordination::Requests ops;

    time_t part_create_time = 0;
    Coordination::Stat stat;
    if (zookeeper->exists(part_path, &stat))
    {
        part_create_time = stat.ctime / 1000;
        removePartFromZooKeeper(part_name, ops, stat.numChildren > 0);
    }

    LogEntryPtr log_entry = std::make_shared<LogEntry>();
    log_entry->type = LogEntry::GET_PART;
    log_entry->create_time = part_create_time;
    log_entry->source_replica = "";
    log_entry->new_part_name = part_name;

    ops.emplace_back(zkutil::makeCreateRequest(
        fs::path(replica_path) / "queue/queue-", log_entry->toString(),
        zkutil::CreateMode::PersistentSequential));

    auto results = zookeeper->multi(ops);

    String path_created = dynamic_cast<const Coordination::CreateResponse &>(*results.back()).path_created;
    log_entry->znode_name = path_created.substr(path_created.find_last_of('/') + 1);
    queue.insert(zookeeper, log_entry);
}


void StorageReplicatedMergeTree::enterLeaderElection()
{
    auto callback = [this]()
    {
        LOG_INFO(log, "Became leader");

        is_leader = true;
        merge_selecting_task->activateAndSchedule();
    };

    try
    {
        leader_election = std::make_shared<zkutil::LeaderElection>(
            getContext()->getSchedulePool(),
            fs::path(zookeeper_path) / "leader_election",
            *current_zookeeper,    /// current_zookeeper lives for the lifetime of leader_election,
                                   ///  since before changing `current_zookeeper`, `leader_election` object is destroyed in `partialShutdown` method.
            callback,
            replica_name);
    }
    catch (...)
    {
        leader_election = nullptr;
        throw;
    }
}

void StorageReplicatedMergeTree::exitLeaderElection()
{
    if (!leader_election)
        return;

    /// Shut down the leader election thread to avoid suddenly becoming the leader again after
    /// we have stopped the merge_selecting_thread, but before we have deleted the leader_election object.
    leader_election->shutdown();

    if (is_leader)
    {
        LOG_INFO(log, "Stopped being leader");

        is_leader = false;
        merge_selecting_task->deactivate();
    }

    /// Delete the node in ZK only after we have stopped the merge_selecting_thread - so that only one
    /// replica assigns merges at any given time.
    leader_election = nullptr;
}

ConnectionTimeouts StorageReplicatedMergeTree::getFetchPartHTTPTimeouts(ContextPtr local_context)
{
    auto timeouts = ConnectionTimeouts::getHTTPTimeouts(local_context);
    auto settings = getSettings();

    if (settings->replicated_fetches_http_connection_timeout.changed)
        timeouts.connection_timeout = settings->replicated_fetches_http_connection_timeout;

    if (settings->replicated_fetches_http_send_timeout.changed)
        timeouts.send_timeout = settings->replicated_fetches_http_send_timeout;

    if (settings->replicated_fetches_http_receive_timeout.changed)
        timeouts.receive_timeout = settings->replicated_fetches_http_receive_timeout;

    return timeouts;
}

bool StorageReplicatedMergeTree::checkReplicaHavePart(const String & replica, const String & part_name)
{
    auto zookeeper = getZooKeeper();
    return zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "parts" / part_name);
}

String StorageReplicatedMergeTree::findReplicaHavingPart(const String & part_name, bool active)
{
    auto zookeeper = getZooKeeper();
    Strings replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");

    /// Select replicas in uniformly random order.
    std::shuffle(replicas.begin(), replicas.end(), thread_local_rng);

    LOG_TRACE(log, "Candidate replicas: {}", replicas.size());

    for (const String & replica : replicas)
    {
        /// We aren't interested in ourself.
        if (replica == replica_name)
            continue;

        LOG_TRACE(log, "Candidate replica: {}", replica);

        if (checkReplicaHavePart(replica, part_name) &&
            (!active || zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "is_active")))
            return replica;

        /// Obviously, replica could become inactive or even vanish after return from this method.
    }

    return {};
}

String StorageReplicatedMergeTree::findReplicaHavingCoveringPart(LogEntry & entry, bool active)
{
    auto zookeeper = getZooKeeper();
    Strings replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");

    /// Select replicas in uniformly random order.
    std::shuffle(replicas.begin(), replicas.end(), thread_local_rng);

    for (const String & replica : replicas)
    {
        if (replica == replica_name)
            continue;

        if (active && !zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "is_active"))
            continue;

        String largest_part_found;
        Strings parts = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas" / replica / "parts");
        for (const String & part_on_replica : parts)
        {
            if (part_on_replica == entry.new_part_name
                || MergeTreePartInfo::contains(part_on_replica, entry.new_part_name, format_version))
            {
                if (largest_part_found.empty()
                    || MergeTreePartInfo::contains(part_on_replica, largest_part_found, format_version))
                {
                    largest_part_found = part_on_replica;
                }
            }
        }

        if (!largest_part_found.empty())
        {
            bool the_same_part = largest_part_found == entry.new_part_name;

            /// Make a check in case if selected part differs from source part
            if (!the_same_part)
            {
                String reject_reason;
                if (!queue.addFuturePartIfNotCoveredByThem(largest_part_found, entry, reject_reason))
                {
                    LOG_INFO(log, "Will not fetch part {} covering {}. {}", largest_part_found, entry.new_part_name, reject_reason);
                    return {};
                }
            }

            return replica;
        }
    }

    return {};
}


String StorageReplicatedMergeTree::findReplicaHavingCoveringPart(
    const String & part_name, bool active, String & found_part_name)
{
    auto zookeeper = getZooKeeper();
    Strings replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");

    /// Select replicas in uniformly random order.
    std::shuffle(replicas.begin(), replicas.end(), thread_local_rng);

    String largest_part_found;
    String largest_replica_found;

    for (const String & replica : replicas)
    {
        if (replica == replica_name)
            continue;

        if (active && !zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "is_active"))
            continue;

        Strings parts = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas" / replica / "parts");
        for (const String & part_on_replica : parts)
        {
            if (part_on_replica == part_name
                || MergeTreePartInfo::contains(part_on_replica, part_name, format_version))
            {
                if (largest_part_found.empty()
                    || MergeTreePartInfo::contains(part_on_replica, largest_part_found, format_version))
                {
                    largest_part_found = part_on_replica;
                    largest_replica_found = replica;
                }
            }
        }
    }

    found_part_name = largest_part_found;
    return largest_replica_found;
}


/** If a quorum is tracked for a part, update information about it in ZK.
  */
void StorageReplicatedMergeTree::updateQuorum(const String & part_name, bool is_parallel)
{
    auto zookeeper = getZooKeeper();

    /// Information on which replicas a part has been added, if the quorum has not yet been reached.
    String quorum_status_path = fs::path(zookeeper_path) / "quorum" / "status";
    if (is_parallel)
        quorum_status_path = fs::path(zookeeper_path) / "quorum" / "parallel" / part_name;
    /// The name of the previous part for which the quorum was reached.
    const String quorum_last_part_path = fs::path(zookeeper_path) / "quorum" / "last_part";

    String value;
    Coordination::Stat stat;

    /// If there is no node, then all quorum INSERTs have already reached the quorum, and nothing is needed.
    while (zookeeper->tryGet(quorum_status_path, value, &stat))
    {
        ReplicatedMergeTreeQuorumEntry quorum_entry(value);
        if (quorum_entry.part_name != part_name)
        {
            LOG_TRACE(log, "Quorum {}, already achieved for part {} current part {}",
                      quorum_status_path, part_name, quorum_entry.part_name);
            /// The quorum has already been achieved. Moreover, another INSERT with a quorum has already started.
            break;
        }

        quorum_entry.replicas.insert(replica_name);

        if (quorum_entry.replicas.size() >= quorum_entry.required_number_of_replicas)
        {
            /// The quorum is reached. Delete the node, and update information about the last part that was successfully written with quorum.
            LOG_TRACE(log, "Got {} replicas confirmed quorum {}, going to remove node",
                      quorum_entry.replicas.size(), quorum_status_path);

            Coordination::Requests ops;
            Coordination::Responses responses;

            if (!is_parallel)
            {
                Coordination::Stat added_parts_stat;
                String old_added_parts = zookeeper->get(quorum_last_part_path, &added_parts_stat);

                ReplicatedMergeTreeQuorumAddedParts parts_with_quorum(format_version);

                if (!old_added_parts.empty())
                    parts_with_quorum.fromString(old_added_parts);

                auto part_info = MergeTreePartInfo::fromPartName(part_name, format_version);
                /// We store one last part which reached quorum for each partition.
                parts_with_quorum.added_parts[part_info.partition_id] = part_name;

                String new_added_parts = parts_with_quorum.toString();

                ops.emplace_back(zkutil::makeRemoveRequest(quorum_status_path, stat.version));
                ops.emplace_back(zkutil::makeSetRequest(quorum_last_part_path, new_added_parts, added_parts_stat.version));
            }
            else
                ops.emplace_back(zkutil::makeRemoveRequest(quorum_status_path, stat.version));

            auto code = zookeeper->tryMulti(ops, responses);

            if (code == Coordination::Error::ZOK)
            {
                break;
            }
            else if (code == Coordination::Error::ZNONODE)
            {
                /// The quorum has already been achieved.
                break;
            }
            else if (code == Coordination::Error::ZBADVERSION)
            {
                /// Node was updated meanwhile. We must re-read it and repeat all the actions.
                continue;
            }
            else
                throw Coordination::Exception(code, quorum_status_path);
        }
        else
        {
            LOG_TRACE(log, "Quorum {} still not satisfied (have only {} replicas), updating node",
                      quorum_status_path, quorum_entry.replicas.size());
            /// We update the node, registering there one more replica.
            auto code = zookeeper->trySet(quorum_status_path, quorum_entry.toString(), stat.version);

            if (code == Coordination::Error::ZOK)
            {
                break;
            }
            else if (code == Coordination::Error::ZNONODE)
            {
                /// The quorum has already been achieved.
                break;
            }
            else if (code == Coordination::Error::ZBADVERSION)
            {
                /// Node was updated meanwhile. We must re-read it and repeat all the actions.
                continue;
            }
            else
                throw Coordination::Exception(code, quorum_status_path);
        }
    }
}


void StorageReplicatedMergeTree::cleanLastPartNode(const String & partition_id)
{
    auto zookeeper = getZooKeeper();

    /// The name of the previous part for which the quorum was reached.
    const String quorum_last_part_path = fs::path(zookeeper_path) / "quorum" / "last_part";

    /// Delete information from "last_part" node.

    while (true)
    {
        Coordination::Stat added_parts_stat;
        String old_added_parts = zookeeper->get(quorum_last_part_path, &added_parts_stat);

        ReplicatedMergeTreeQuorumAddedParts parts_with_quorum(format_version);

        if (!old_added_parts.empty())
            parts_with_quorum.fromString(old_added_parts);

        /// Delete information about particular partition.
        if (!parts_with_quorum.added_parts.count(partition_id))
        {
            /// There is no information about interested part.
            break;
        }

        parts_with_quorum.added_parts.erase(partition_id);

        String new_added_parts = parts_with_quorum.toString();

        auto code = zookeeper->trySet(quorum_last_part_path, new_added_parts, added_parts_stat.version);

        if (code == Coordination::Error::ZOK)
        {
            break;
        }
        else if (code == Coordination::Error::ZNONODE)
        {
            /// Node is deleted. It is impossible, but it is Ok.
            break;
        }
        else if (code == Coordination::Error::ZBADVERSION)
        {
            /// Node was updated meanwhile. We must re-read it and repeat all the actions.
            continue;
        }
        else
            throw Coordination::Exception(code, quorum_last_part_path);
    }
}


bool StorageReplicatedMergeTree::partIsInsertingWithParallelQuorum(const MergeTreePartInfo & part_info) const
{
    auto zookeeper = getZooKeeper();
    return zookeeper->exists(fs::path(zookeeper_path) / "quorum" / "parallel" / part_info.getPartName());
}


bool StorageReplicatedMergeTree::partIsLastQuorumPart(const MergeTreePartInfo & part_info) const
{
    auto zookeeper = getZooKeeper();

    const String parts_with_quorum_path = fs::path(zookeeper_path) / "quorum" / "last_part";

    String parts_with_quorum_str = zookeeper->get(parts_with_quorum_path);

    if (parts_with_quorum_str.empty())
        return false;

    ReplicatedMergeTreeQuorumAddedParts parts_with_quorum(format_version);
    parts_with_quorum.fromString(parts_with_quorum_str);

    auto partition_it = parts_with_quorum.added_parts.find(part_info.partition_id);
    if (partition_it == parts_with_quorum.added_parts.end())
        return false;

    return partition_it->second == part_info.getPartName();
}


bool StorageReplicatedMergeTree::fetchPart(const String & part_name, const StorageMetadataPtr & metadata_snapshot,
    const String & source_replica_path, bool to_detached, size_t quorum, zkutil::ZooKeeper::Ptr zookeeper_)
{
    auto zookeeper = zookeeper_ ? zookeeper_ : getZooKeeper();
    const auto part_info = MergeTreePartInfo::fromPartName(part_name, format_version);

    if (!to_detached)
    {
        if (auto part = getPartIfExists(part_info, {IMergeTreeDataPart::State::Outdated, IMergeTreeDataPart::State::Deleting}))
        {
            LOG_DEBUG(log, "Part {} should be deleted after previous attempt before fetch", part->name);
            /// Force immediate parts cleanup to delete the part that was left from the previous fetch attempt.
            cleanup_thread.wakeup();
            return false;
        }
    }

    {
        std::lock_guard lock(currently_fetching_parts_mutex);
        if (!currently_fetching_parts.insert(part_name).second)
        {
            LOG_DEBUG(log, "Part {} is already fetching right now", part_name);
            return false;
        }
    }

    SCOPE_EXIT_MEMORY
    ({
        std::lock_guard lock(currently_fetching_parts_mutex);
        currently_fetching_parts.erase(part_name);
    });

    LOG_DEBUG(log, "Fetching part {} from {}", part_name, source_replica_path);

    TableLockHolder table_lock_holder;
    if (!to_detached)
        table_lock_holder = lockForShare(RWLockImpl::NO_QUERY, getSettings()->lock_acquire_timeout_for_background_operations);

    /// Logging
    Stopwatch stopwatch;
    MutableDataPartPtr part;
    DataPartsVector replaced_parts;

    auto write_part_log = [&] (const ExecutionStatus & execution_status)
    {
        writePartLog(
            PartLogElement::DOWNLOAD_PART, execution_status, stopwatch.elapsed(),
            part_name, part, replaced_parts, nullptr);
    };

    DataPartPtr part_to_clone;
    {
        /// If the desired part is a result of a part mutation, try to find the source part and compare
        /// its checksums to the checksums of the desired part. If they match, we can just clone the local part.

        /// If we have the source part, its part_info will contain covered_part_info.
        auto covered_part_info = part_info;
        covered_part_info.mutation = 0;
        auto source_part = getActiveContainingPart(covered_part_info);

        if (source_part)
        {
            MinimalisticDataPartChecksums source_part_checksums;
            source_part_checksums.computeTotalChecksums(source_part->checksums);

            MinimalisticDataPartChecksums desired_checksums;
            String part_path = fs::path(source_replica_path) / "parts" / part_name;
            String part_znode = zookeeper->get(part_path);

            if (!part_znode.empty())
                desired_checksums = ReplicatedMergeTreePartHeader::fromString(part_znode).getChecksums();
            else
            {
                String desired_checksums_str = zookeeper->get(fs::path(part_path) / "checksums");
                desired_checksums = MinimalisticDataPartChecksums::deserializeFrom(desired_checksums_str);
            }

            if (source_part_checksums == desired_checksums)
            {
                LOG_TRACE(log, "Found local part {} with the same checksums as {}", source_part->name, part_name);
                part_to_clone = source_part;
            }
        }

    }

    ReplicatedMergeTreeAddress address;
    ConnectionTimeouts timeouts;
    String interserver_scheme;
    InterserverCredentialsPtr credentials;
    std::optional<CurrentlySubmergingEmergingTagger> tagger_ptr;
    std::function<MutableDataPartPtr()> get_part;

    if (part_to_clone)
    {
        get_part = [&, part_to_clone]()
        {
            return cloneAndLoadDataPartOnSameDisk(part_to_clone, "tmp_clone_", part_info, metadata_snapshot);
        };
    }
    else
    {
        address.fromString(zookeeper->get(fs::path(source_replica_path) / "host"));
        timeouts = getFetchPartHTTPTimeouts(getContext());

        credentials = getContext()->getInterserverCredentials();
        interserver_scheme = getContext()->getInterserverScheme();

        get_part = [&, address, timeouts, credentials, interserver_scheme]()
        {
            if (interserver_scheme != address.scheme)
                throw Exception("Interserver schemes are different: '" + interserver_scheme
                    + "' != '" + address.scheme + "', can't fetch part from " + address.host,
                    ErrorCodes::INTERSERVER_SCHEME_DOESNT_MATCH);

            return fetcher.fetchPart(
                metadata_snapshot,
                getContext(),
                part_name,
                source_replica_path,
                address.host,
                address.replication_port,
                timeouts,
                credentials->getUser(),
                credentials->getPassword(),
                interserver_scheme,
                replicated_fetches_throttler,
                to_detached,
                "",
                &tagger_ptr,
                true);
        };
    }

    try
    {
        part = get_part();

        if (!to_detached)
        {
            Transaction transaction(*this);
            renameTempPartAndReplace(part, nullptr, &transaction);

            replaced_parts = checkPartChecksumsAndCommit(transaction, part);

            /** If a quorum is tracked for this part, you must update it.
              * If you do not have time, in case of losing the session, when you restart the server - see the `ReplicatedMergeTreeRestartingThread::updateQuorumIfWeHavePart` method.
              */
            if (quorum)
            {
                /// Check if this quorum insert is parallel or not
                if (zookeeper->exists(fs::path(zookeeper_path) / "quorum" / "parallel" / part_name))
                    updateQuorum(part_name, true);
                else if (zookeeper->exists(fs::path(zookeeper_path) / "quorum" / "status"))
                    updateQuorum(part_name, false);
            }

            /// merged parts that are still inserted with quorum. if it only contains one block, it hasn't been merged before
            if (part_info.level != 0 || part_info.mutation != 0)
            {
                Strings quorum_parts = zookeeper->getChildren(fs::path(zookeeper_path) / "quorum" / "parallel");
                for (const String & quorum_part : quorum_parts)
                {
                    auto quorum_part_info = MergeTreePartInfo::fromPartName(quorum_part, format_version);
                    if (part_info.contains(quorum_part_info))
                        updateQuorum(quorum_part, true);
                }
            }

            merge_selecting_task->schedule();

            for (const auto & replaced_part : replaced_parts)
            {
                LOG_DEBUG(log, "Part {} is rendered obsolete by fetching part {}", replaced_part->name, part_name);
                ProfileEvents::increment(ProfileEvents::ObsoleteReplicatedParts);
            }

            write_part_log({});
        }
        else
        {
            // The fetched part is valuable and should not be cleaned like a temp part.
            part->is_temp = false;
            part->renameTo(fs::path("detached") / part_name, true);
        }
    }
    catch (const Exception & e)
    {
        /// The same part is being written right now (but probably it's not committed yet).
        /// We will check the need for fetch later.
        if (e.code() == ErrorCodes::DIRECTORY_ALREADY_EXISTS)
            return false;

        throw;
    }
    catch (...)
    {
        if (!to_detached)
            write_part_log(ExecutionStatus::fromCurrentException());

        throw;
    }

    ProfileEvents::increment(ProfileEvents::ReplicatedPartFetches);

    if (part_to_clone)
        LOG_DEBUG(log, "Cloned part {} from {}{}", part_name, part_to_clone->name, to_detached ? " (to 'detached' directory)" : "");
    else
        LOG_DEBUG(log, "Fetched part {} from {}{}", part_name, source_replica_path, to_detached ? " (to 'detached' directory)" : "");

    return true;
}


bool StorageReplicatedMergeTree::fetchExistsPart(const String & part_name, const StorageMetadataPtr & metadata_snapshot,
    const String & source_replica_path, DiskPtr replaced_disk, String replaced_part_path)
{
    auto zookeeper = getZooKeeper();
    const auto part_info = MergeTreePartInfo::fromPartName(part_name, format_version);

    if (auto part = getPartIfExists(part_info, {IMergeTreeDataPart::State::Outdated, IMergeTreeDataPart::State::Deleting}))
    {
        LOG_DEBUG(log, "Part {} should be deleted after previous attempt before fetch", part->name);
        /// Force immediate parts cleanup to delete the part that was left from the previous fetch attempt.
        cleanup_thread.wakeup();
        return false;
    }

    {
        std::lock_guard lock(currently_fetching_parts_mutex);
        if (!currently_fetching_parts.insert(part_name).second)
        {
            LOG_DEBUG(log, "Part {} is already fetching right now", part_name);
            return false;
        }
    }

    SCOPE_EXIT_MEMORY
    ({
        std::lock_guard lock(currently_fetching_parts_mutex);
        currently_fetching_parts.erase(part_name);
    });

    LOG_DEBUG(log, "Fetching part {} from {}", part_name, source_replica_path);

    TableLockHolder table_lock_holder = lockForShare(RWLockImpl::NO_QUERY, getSettings()->lock_acquire_timeout_for_background_operations);

    /// Logging
    Stopwatch stopwatch;
    MutableDataPartPtr part;
    DataPartsVector replaced_parts;

    auto write_part_log = [&] (const ExecutionStatus & execution_status)
    {
        writePartLog(
            PartLogElement::DOWNLOAD_PART, execution_status, stopwatch.elapsed(),
            part_name, part, replaced_parts, nullptr);
    };

    std::function<MutableDataPartPtr()> get_part;

    ReplicatedMergeTreeAddress address(zookeeper->get(fs::path(source_replica_path) / "host"));
    auto timeouts = ConnectionTimeouts::getHTTPTimeouts(getContext());
    auto credentials = getContext()->getInterserverCredentials();
    String interserver_scheme = getContext()->getInterserverScheme();

    get_part = [&, address, timeouts, interserver_scheme, credentials]()
    {
        if (interserver_scheme != address.scheme)
            throw Exception("Interserver schemes are different: '" + interserver_scheme
                + "' != '" + address.scheme + "', can't fetch part from " + address.host,
                ErrorCodes::INTERSERVER_SCHEME_DOESNT_MATCH);

        return fetcher.fetchPart(
            metadata_snapshot, getContext(), part_name, source_replica_path,
            address.host, address.replication_port,
            timeouts, credentials->getUser(), credentials->getPassword(),
            interserver_scheme, replicated_fetches_throttler, false, "", nullptr, true,
            replaced_disk);
    };

    try
    {
        part = get_part();

        if (part->volume->getDisk()->getName() != replaced_disk->getName())
            throw Exception("Part " + part->name + " fetched on wrong disk " + part->volume->getDisk()->getName(), ErrorCodes::LOGICAL_ERROR);
        replaced_disk->removeFileIfExists(replaced_part_path);
        replaced_disk->moveDirectory(part->getFullRelativePath(), replaced_part_path);
    }
    catch (const Exception & e)
    {
        /// The same part is being written right now (but probably it's not committed yet).
        /// We will check the need for fetch later.
        if (e.code() == ErrorCodes::DIRECTORY_ALREADY_EXISTS)
            return false;

        throw;
    }
    catch (...)
    {
        write_part_log(ExecutionStatus::fromCurrentException());
        throw;
    }

    ProfileEvents::increment(ProfileEvents::ReplicatedPartFetches);

    LOG_DEBUG(log, "Fetched part {} from {}", part_name, source_replica_path);

    return true;
}


void StorageReplicatedMergeTree::startup()
{
    if (is_readonly)
        return;

    try
    {
        InterserverIOEndpointPtr data_parts_exchange_ptr = std::make_shared<DataPartsExchange::Service>(*this);
        [[maybe_unused]] auto prev_ptr = std::atomic_exchange(&data_parts_exchange_endpoint, data_parts_exchange_ptr);
        assert(prev_ptr == nullptr);
        getContext()->getInterserverIOHandler().addEndpoint(data_parts_exchange_ptr->getId(replica_path), data_parts_exchange_ptr);

        /// In this thread replica will be activated.
        restarting_thread.start();

        /// Wait while restarting_thread initializes LeaderElection (and so on) or makes first attempt to do it
        startup_event.wait();

        startBackgroundMovesIfNeeded();

        part_moves_between_shards_orchestrator.start();
    }
    catch (...)
    {
        /// Exception safety: failed "startup" does not require a call to "shutdown" from the caller.
        /// And it should be able to safely destroy table after exception in "startup" method.
        /// It means that failed "startup" must not create any background tasks that we will have to wait.
        try
        {
            shutdown();
        }
        catch (...)
        {
            std::terminate();
        }

        /// Note: after failed "startup", the table will be in a state that only allows to destroy the object.
        throw;
    }
}


void StorageReplicatedMergeTree::shutdown()
{
    /// Cancel fetches, merges and mutations to force the queue_task to finish ASAP.
    fetcher.blocker.cancelForever();
    merger_mutator.merges_blocker.cancelForever();
    parts_mover.moves_blocker.cancelForever();

    restarting_thread.shutdown();
    background_operations_assignee.finish();
    part_moves_between_shards_orchestrator.shutdown();

    {
        auto lock = queue.lockQueue();
        /// Cancel logs pulling after background task were cancelled. It's still
        /// required because we can trigger pullLogsToQueue during manual OPTIMIZE,
        /// MUTATE, etc. query.
        queue.pull_log_blocker.cancelForever();
    }
    background_moves_assignee.finish();

    auto data_parts_exchange_ptr = std::atomic_exchange(&data_parts_exchange_endpoint, InterserverIOEndpointPtr{});
    if (data_parts_exchange_ptr)
    {
        getContext()->getInterserverIOHandler().removeEndpointIfExists(data_parts_exchange_ptr->getId(replica_path));
        /// Ask all parts exchange handlers to finish asap. New ones will fail to start
        data_parts_exchange_ptr->blocker.cancelForever();
        /// Wait for all of them
        std::unique_lock lock(data_parts_exchange_ptr->rwlock);
    }
}


StorageReplicatedMergeTree::~StorageReplicatedMergeTree()
{
    try
    {
        shutdown();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}


ReplicatedMergeTreeQuorumAddedParts::PartitionIdToMaxBlock StorageReplicatedMergeTree::getMaxAddedBlocks() const
{
    ReplicatedMergeTreeQuorumAddedParts::PartitionIdToMaxBlock max_added_blocks;

    for (const auto & data_part : getDataParts())
    {
        max_added_blocks[data_part->info.partition_id]
            = std::max(max_added_blocks[data_part->info.partition_id], data_part->info.max_block);
    }

    auto zookeeper = getZooKeeper();

    const String quorum_status_path = fs::path(zookeeper_path) / "quorum" / "status";

    String value;
    Coordination::Stat stat;

    if (zookeeper->tryGet(quorum_status_path, value, &stat))
    {
        ReplicatedMergeTreeQuorumEntry quorum_entry;
        quorum_entry.fromString(value);

        auto part_info = MergeTreePartInfo::fromPartName(quorum_entry.part_name, format_version);

        max_added_blocks[part_info.partition_id] = part_info.max_block - 1;
    }

    String added_parts_str;
    if (zookeeper->tryGet(fs::path(zookeeper_path) / "quorum" / "last_part", added_parts_str))
    {
        if (!added_parts_str.empty())
        {
            ReplicatedMergeTreeQuorumAddedParts part_with_quorum(format_version);
            part_with_quorum.fromString(added_parts_str);

            auto added_parts = part_with_quorum.added_parts;

            for (const auto & added_part : added_parts)
                if (!getActiveContainingPart(added_part.second))
                    throw Exception(
                        "Replica doesn't have part " + added_part.second
                            + " which was successfully written to quorum of other replicas."
                              " Send query to another replica or disable 'select_sequential_consistency' setting.",
                        ErrorCodes::REPLICA_IS_NOT_IN_QUORUM);

            for (const auto & max_block : part_with_quorum.getMaxInsertedBlocks())
                max_added_blocks[max_block.first] = max_block.second;
        }
    }
    return max_added_blocks;
}


void StorageReplicatedMergeTree::read(
    QueryPlan & query_plan,
    const Names & column_names,
    const StorageMetadataPtr & metadata_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum processed_stage,
    const size_t max_block_size,
    const unsigned num_streams)
{
    /** The `select_sequential_consistency` setting has two meanings:
    * 1. To throw an exception if on a replica there are not all parts which have been written down on quorum of remaining replicas.
    * 2. Do not read parts that have not yet been written to the quorum of the replicas.
    * For this you have to synchronously go to ZooKeeper.
    */
    if (local_context->getSettingsRef().select_sequential_consistency)
    {
        auto max_added_blocks = std::make_shared<ReplicatedMergeTreeQuorumAddedParts::PartitionIdToMaxBlock>(getMaxAddedBlocks());
        if (auto plan = reader.read(
                column_names, metadata_snapshot, query_info, local_context, max_block_size, num_streams, processed_stage, std::move(max_added_blocks)))
            query_plan = std::move(*plan);
        return;
    }

    if (auto plan = reader.read(column_names, metadata_snapshot, query_info, local_context, max_block_size, num_streams, processed_stage))
        query_plan = std::move(*plan);
}

Pipe StorageReplicatedMergeTree::read(
    const Names & column_names,
    const StorageMetadataPtr & metadata_snapshot,
    SelectQueryInfo & query_info,
    ContextPtr local_context,
    QueryProcessingStage::Enum processed_stage,
    const size_t max_block_size,
    const unsigned num_streams)
{
    QueryPlan plan;
    read(plan, column_names, metadata_snapshot, query_info, local_context, processed_stage, max_block_size, num_streams);
    return plan.convertToPipe(
        QueryPlanOptimizationSettings::fromContext(local_context),
        BuildQueryPipelineSettings::fromContext(local_context));
}


template <class Func>
void StorageReplicatedMergeTree::foreachCommittedParts(Func && func, bool select_sequential_consistency) const
{
    std::optional<ReplicatedMergeTreeQuorumAddedParts::PartitionIdToMaxBlock> max_added_blocks = {};

    /**
     * Synchronously go to ZooKeeper when select_sequential_consistency enabled
     */
    if (select_sequential_consistency)
        max_added_blocks = getMaxAddedBlocks();

    auto lock = lockParts();
    for (const auto & part : getDataPartsStateRange(DataPartState::Committed))
    {
        if (part->isEmpty())
            continue;

        if (max_added_blocks)
        {
            auto blocks_iterator = max_added_blocks->find(part->info.partition_id);
            if (blocks_iterator == max_added_blocks->end() || part->info.max_block > blocks_iterator->second)
                continue;
        }

        func(part);
    }
}

std::optional<UInt64> StorageReplicatedMergeTree::totalRows(const Settings & settings) const
{
    UInt64 res = 0;
    foreachCommittedParts([&res](auto & part) { res += part->rows_count; }, settings.select_sequential_consistency);
    return res;
}

std::optional<UInt64> StorageReplicatedMergeTree::totalRowsByPartitionPredicate(const SelectQueryInfo & query_info, ContextPtr local_context) const
{
    DataPartsVector parts;
    foreachCommittedParts([&](auto & part) { parts.push_back(part); }, local_context->getSettingsRef().select_sequential_consistency);
    return totalRowsByPartitionPredicateImpl(query_info, local_context, parts);
}

std::optional<UInt64> StorageReplicatedMergeTree::totalBytes(const Settings & settings) const
{
    UInt64 res = 0;
    foreachCommittedParts([&res](auto & part) { res += part->getBytesOnDisk(); }, settings.select_sequential_consistency);
    return res;
}


void StorageReplicatedMergeTree::assertNotReadonly() const
{
    if (is_readonly)
        throw Exception(ErrorCodes::TABLE_IS_READ_ONLY, "Table is in readonly mode (zookeeper path: {})", zookeeper_path);
}


SinkToStoragePtr StorageReplicatedMergeTree::write(const ASTPtr & /*query*/, const StorageMetadataPtr & metadata_snapshot, ContextPtr local_context)
{
    const auto storage_settings_ptr = getSettings();
    assertNotReadonly();

    const Settings & query_settings = local_context->getSettingsRef();
    bool deduplicate = storage_settings_ptr->replicated_deduplication_window != 0 && query_settings.insert_deduplicate;

    // TODO: should we also somehow pass list of columns to deduplicate on to the ReplicatedMergeTreeBlockOutputStream ?
    return std::make_shared<ReplicatedMergeTreeSink>(
        *this, metadata_snapshot, query_settings.insert_quorum,
        query_settings.insert_quorum_timeout.totalMilliseconds(),
        query_settings.max_partitions_per_insert_block,
        query_settings.insert_quorum_parallel,
        deduplicate,
        local_context);
}


bool StorageReplicatedMergeTree::optimize(
    const ASTPtr &,
    const StorageMetadataPtr &,
    const ASTPtr & partition,
    bool final,
    bool deduplicate,
    const Names & deduplicate_by_columns,
    ContextPtr query_context)
{
    /// NOTE: exclusive lock cannot be used here, since this may lead to deadlock (see comments below),
    /// but it should be safe to use non-exclusive to avoid dropping parts that may be required for processing queue.
    auto table_lock = lockForShare(query_context->getCurrentQueryId(), query_context->getSettingsRef().lock_acquire_timeout);

    assertNotReadonly();

    if (!is_leader)
        throw Exception("OPTIMIZE cannot be done on this replica because it is not a leader", ErrorCodes::NOT_A_LEADER);

    auto handle_noop = [&] (const String & message)
    {
        if (query_context->getSettingsRef().optimize_throw_if_noop)
            throw Exception(message, ErrorCodes::CANNOT_ASSIGN_OPTIMIZE);
        return false;
    };

    auto zookeeper = getZooKeeper();
    UInt64 disk_space = getStoragePolicy()->getMaxUnreservedFreeSpace();
    const auto storage_settings_ptr = getSettings();
    auto metadata_snapshot = getInMemoryMetadataPtr();
    std::vector<ReplicatedMergeTreeLogEntryData> merge_entries;

    auto try_assign_merge = [&](const String & partition_id) -> bool
    {
        constexpr size_t max_retries = 10;
        size_t try_no = 0;
        for (; try_no < max_retries; ++try_no)
        {
            /// We must select parts for merge under merge_selecting_mutex because other threads
            /// (merge_selecting_thread or OPTIMIZE queries) could assign new merges.
            std::lock_guard merge_selecting_lock(merge_selecting_mutex);
            ReplicatedMergeTreeMergePredicate can_merge = queue.getMergePredicate(zookeeper);

            auto future_merged_part = std::make_shared<FutureMergedMutatedPart>();
            if (storage_settings.get()->assign_part_uuids)
                future_merged_part->uuid = UUIDHelpers::generateV4();

            constexpr const char * unknown_disable_reason = "unknown reason";
            String disable_reason = unknown_disable_reason;
            SelectPartsDecision select_decision = SelectPartsDecision::CANNOT_SELECT;

            if (partition_id.empty())
            {
                select_decision = merger_mutator.selectPartsToMerge(
                    future_merged_part, /* aggressive */ true, storage_settings_ptr->max_bytes_to_merge_at_max_space_in_pool,
                    can_merge, /* merge_with_ttl_allowed */ false, &disable_reason);
            }
            else
            {
                select_decision = merger_mutator.selectAllPartsToMergeWithinPartition(
                    future_merged_part, disk_space, can_merge, partition_id, final, metadata_snapshot,
                    &disable_reason, query_context->getSettingsRef().optimize_skip_merged_partitions);
            }

            /// If there is nothing to merge then we treat this merge as successful (needed for optimize final optimization)
            if (select_decision == SelectPartsDecision::NOTHING_TO_MERGE)
                return false;

            if (select_decision != SelectPartsDecision::SELECTED)
            {
                constexpr const char * message_fmt = "Cannot select parts for optimization: {}";
                assert(disable_reason != unknown_disable_reason);
                if (!partition_id.empty())
                    disable_reason += fmt::format(" (in partition {})", partition_id);
                String message = fmt::format(message_fmt, disable_reason);
                LOG_INFO(log, message);
                return handle_noop(message);
            }

            ReplicatedMergeTreeLogEntryData merge_entry;
            CreateMergeEntryResult create_result = createLogEntryToMergeParts(
                zookeeper, future_merged_part->parts,
                future_merged_part->name, future_merged_part->uuid, future_merged_part->type,
                deduplicate, deduplicate_by_columns,
                &merge_entry, can_merge.getVersion(), future_merged_part->merge_type);

            if (create_result == CreateMergeEntryResult::MissingPart)
            {
                String message = "Can't create merge queue node in ZooKeeper, because some parts are missing";
                LOG_TRACE(log, message);
                return handle_noop(message);
            }

            if (create_result == CreateMergeEntryResult::LogUpdated)
                continue;

            merge_entries.push_back(std::move(merge_entry));
            return true;
        }

        assert(try_no == max_retries);
        String message = fmt::format("Can't create merge queue node in ZooKeeper, because log was updated in every of {} tries", try_no);
        LOG_TRACE(log, message);
        return handle_noop(message);
    };

    bool assigned = false;
    if (!partition && final)
    {
        DataPartsVector data_parts = getDataPartsVector();
        std::unordered_set<String> partition_ids;

        for (const DataPartPtr & part : data_parts)
            partition_ids.emplace(part->info.partition_id);

        for (const String & partition_id : partition_ids)
        {
            assigned = try_assign_merge(partition_id);
            if (!assigned)
                break;
        }
    }
    else
    {
        String partition_id;
        if (partition)
            partition_id = getPartitionIDFromQuery(partition, query_context);
        assigned = try_assign_merge(partition_id);
    }

    table_lock.reset();

    for (auto & merge_entry : merge_entries)
        waitForLogEntryToBeProcessedIfNecessary(merge_entry, query_context);

    return assigned;
}

bool StorageReplicatedMergeTree::executeMetadataAlter(const StorageReplicatedMergeTree::LogEntry & entry)
{
    if (entry.alter_version < metadata_version)
    {
        /// TODO Can we replace it with LOGICAL_ERROR?
        /// As for now, it may rerely happen due to reordering of ALTER_METADATA entries in the queue of
        /// non-initial replica and also may happen after stale replica recovery.
        LOG_WARNING(log, "Attempt to update metadata of version {} "
                         "to older version {} when processing log entry {}: {}",
                         metadata_version, entry.alter_version, entry.znode_name, entry.toString());
        return true;
    }

    auto zookeeper = getZooKeeper();

    auto columns_from_entry = ColumnsDescription::parse(entry.columns_str);
    auto metadata_from_entry = ReplicatedMergeTreeTableMetadata::parse(entry.metadata_str);

    MergeTreeData::DataParts parts;

    /// If metadata nodes have changed, we will update table structure locally.
    Coordination::Requests requests;
    requests.emplace_back(zkutil::makeSetRequest(fs::path(replica_path) / "columns", entry.columns_str, -1));
    requests.emplace_back(zkutil::makeSetRequest(fs::path(replica_path) / "metadata", entry.metadata_str, -1));

    zookeeper->multi(requests);

    {
        auto lock = lockForAlter(RWLockImpl::NO_QUERY, getSettings()->lock_acquire_timeout_for_background_operations);
        LOG_INFO(log, "Metadata changed in ZooKeeper. Applying changes locally.");

        auto metadata_diff = ReplicatedMergeTreeTableMetadata(*this, getInMemoryMetadataPtr()).checkAndFindDiff(metadata_from_entry);
        setTableStructure(std::move(columns_from_entry), metadata_diff);
        metadata_version = entry.alter_version;

        LOG_INFO(log, "Applied changes to the metadata of the table. Current metadata version: {}", metadata_version);
    }

    /// This transaction may not happen, but it's OK, because on the next retry we will eventually create/update this node
    zookeeper->createOrUpdate(fs::path(replica_path) / "metadata_version", std::to_string(metadata_version), zkutil::CreateMode::Persistent);

    return true;
}


std::set<String> StorageReplicatedMergeTree::getPartitionIdsAffectedByCommands(
    const MutationCommands & commands, ContextPtr query_context) const
{
    std::set<String> affected_partition_ids;

    for (const auto & command : commands)
    {
        if (!command.partition)
        {
            affected_partition_ids.clear();
            break;
        }

        affected_partition_ids.insert(
            getPartitionIDFromQuery(command.partition, query_context)
        );
    }

    return affected_partition_ids;
}


PartitionBlockNumbersHolder StorageReplicatedMergeTree::allocateBlockNumbersInAffectedPartitions(
    const MutationCommands & commands, ContextPtr query_context, const zkutil::ZooKeeperPtr & zookeeper) const
{
    const std::set<String> mutation_affected_partition_ids = getPartitionIdsAffectedByCommands(commands, query_context);

    if (mutation_affected_partition_ids.size() == 1)
    {
        const auto & affected_partition_id = *mutation_affected_partition_ids.cbegin();
        auto block_number_holder = allocateBlockNumber(affected_partition_id, zookeeper);
        if (!block_number_holder.has_value())
            return {};
        auto block_number = block_number_holder->getNumber();  /// Avoid possible UB due to std::move
        return {{{affected_partition_id, block_number}}, std::move(block_number_holder)};
    }
    else
    {
        /// TODO: Implement optimal block number aqcuisition algorithm in multiple (but not all) partitions
        EphemeralLocksInAllPartitions lock_holder(
            fs::path(zookeeper_path) / "block_numbers", "block-", fs::path(zookeeper_path) / "temp", *zookeeper);

        PartitionBlockNumbersHolder::BlockNumbersType block_numbers;
        for (const auto & lock : lock_holder.getLocks())
        {
            if (mutation_affected_partition_ids.empty() || mutation_affected_partition_ids.count(lock.partition_id))
                block_numbers[lock.partition_id] = lock.number;
        }

        return {std::move(block_numbers), std::move(lock_holder)};
    }
}


void StorageReplicatedMergeTree::alter(
    const AlterCommands & commands, ContextPtr query_context, TableLockHolder & table_lock_holder)
{
    assertNotReadonly();

    auto table_id = getStorageID();

    if (commands.isSettingsAlter())
    {
        /// We don't replicate storage_settings_ptr ALTER. It's local operation.
        /// Also we don't upgrade alter lock to table structure lock.
        StorageInMemoryMetadata future_metadata = getInMemoryMetadata();
        commands.apply(future_metadata, query_context);

        merge_strategy_picker.refreshState();

        changeSettings(future_metadata.settings_changes, table_lock_holder);

        DatabaseCatalog::instance().getDatabase(table_id.database_name)->alterTable(query_context, table_id, future_metadata);
        return;
    }

    auto ast_to_str = [](ASTPtr query) -> String
    {
        if (!query)
            return "";
        return queryToString(query);
    };

    const auto zookeeper = getZooKeeper();

    std::optional<ReplicatedMergeTreeLogEntryData> alter_entry;
    std::optional<String> mutation_znode;

    while (true)
    {
        /// Clear nodes from previous iteration
        alter_entry.emplace();
        mutation_znode.reset();

        auto current_metadata = getInMemoryMetadataPtr();

        StorageInMemoryMetadata future_metadata = *current_metadata;
        commands.apply(future_metadata, query_context);

        ReplicatedMergeTreeTableMetadata future_metadata_in_zk(*this, current_metadata);
        if (ast_to_str(future_metadata.sorting_key.definition_ast) != ast_to_str(current_metadata->sorting_key.definition_ast))
        {
            /// We serialize definition_ast as list, because code which apply ALTER (setTableStructure) expect serialized non empty expression
            /// list here and we cannot change this representation for compatibility. Also we have preparsed AST `sorting_key.expression_list_ast`
            /// in KeyDescription, but it contain version column for VersionedCollapsingMergeTree, which shouldn't be defined as a part of key definition AST.
            /// So the best compatible way is just to convert definition_ast to list and serialize it. In all other places key.expression_list_ast should be used.
            future_metadata_in_zk.sorting_key = serializeAST(*extractKeyExpressionList(future_metadata.sorting_key.definition_ast));
        }

        if (ast_to_str(future_metadata.sampling_key.definition_ast) != ast_to_str(current_metadata->sampling_key.definition_ast))
            future_metadata_in_zk.sampling_expression = serializeAST(*extractKeyExpressionList(future_metadata.sampling_key.definition_ast));

        if (ast_to_str(future_metadata.partition_key.definition_ast) != ast_to_str(current_metadata->partition_key.definition_ast))
            future_metadata_in_zk.partition_key = serializeAST(*extractKeyExpressionList(future_metadata.partition_key.definition_ast));

        if (ast_to_str(future_metadata.table_ttl.definition_ast) != ast_to_str(current_metadata->table_ttl.definition_ast))
        {
            if (future_metadata.table_ttl.definition_ast)
                future_metadata_in_zk.ttl_table = serializeAST(*future_metadata.table_ttl.definition_ast);
            else /// TTL was removed
                future_metadata_in_zk.ttl_table = "";
        }

        String new_indices_str = future_metadata.secondary_indices.toString();
        if (new_indices_str != current_metadata->secondary_indices.toString())
            future_metadata_in_zk.skip_indices = new_indices_str;

        String new_projections_str = future_metadata.projections.toString();
        if (new_projections_str != current_metadata->projections.toString())
            future_metadata_in_zk.projections = new_projections_str;

        String new_constraints_str = future_metadata.constraints.toString();
        if (new_constraints_str != current_metadata->constraints.toString())
            future_metadata_in_zk.constraints = new_constraints_str;

        Coordination::Requests ops;
        size_t alter_path_idx = std::numeric_limits<size_t>::max();
        size_t mutation_path_idx = std::numeric_limits<size_t>::max();

        String new_metadata_str = future_metadata_in_zk.toString();
        ops.emplace_back(zkutil::makeSetRequest(fs::path(zookeeper_path) / "metadata", new_metadata_str, metadata_version));

        String new_columns_str = future_metadata.columns.toString();
        ops.emplace_back(zkutil::makeSetRequest(fs::path(zookeeper_path) / "columns", new_columns_str, -1));

        if (ast_to_str(current_metadata->settings_changes) != ast_to_str(future_metadata.settings_changes))
        {
            /// Just change settings
            StorageInMemoryMetadata metadata_copy = *current_metadata;
            metadata_copy.settings_changes = future_metadata.settings_changes;
            changeSettings(metadata_copy.settings_changes, table_lock_holder);
            DatabaseCatalog::instance().getDatabase(table_id.database_name)->alterTable(query_context, table_id, metadata_copy);
        }

        /// We can be sure, that in case of successful commit in zookeeper our
        /// version will increments by 1. Because we update with version check.
        int new_metadata_version = metadata_version + 1;

        alter_entry->type = LogEntry::ALTER_METADATA;
        alter_entry->source_replica = replica_name;
        alter_entry->metadata_str = new_metadata_str;
        alter_entry->columns_str = new_columns_str;
        alter_entry->alter_version = new_metadata_version;
        alter_entry->create_time = time(nullptr);

        auto maybe_mutation_commands = commands.getMutationCommands(
            *current_metadata, query_context->getSettingsRef().materialize_ttl_after_modify, query_context);
        bool have_mutation = !maybe_mutation_commands.empty();
        alter_entry->have_mutation = have_mutation;

        alter_path_idx = ops.size();
        ops.emplace_back(zkutil::makeCreateRequest(
            fs::path(zookeeper_path) / "log/log-", alter_entry->toString(), zkutil::CreateMode::PersistentSequential));

        PartitionBlockNumbersHolder partition_block_numbers_holder;
        if (have_mutation)
        {
            const String mutations_path(fs::path(zookeeper_path) / "mutations");

            ReplicatedMergeTreeMutationEntry mutation_entry;
            mutation_entry.alter_version = new_metadata_version;
            mutation_entry.source_replica = replica_name;
            mutation_entry.commands = std::move(maybe_mutation_commands);

            Coordination::Stat mutations_stat;
            zookeeper->get(mutations_path, &mutations_stat);

            partition_block_numbers_holder =
                allocateBlockNumbersInAffectedPartitions(mutation_entry.commands, query_context, zookeeper);

            mutation_entry.block_numbers = partition_block_numbers_holder.getBlockNumbers();
            mutation_entry.create_time = time(nullptr);

            ops.emplace_back(zkutil::makeSetRequest(mutations_path, String(), mutations_stat.version));
            mutation_path_idx = ops.size();
            ops.emplace_back(
                zkutil::makeCreateRequest(fs::path(mutations_path) / "", mutation_entry.toString(), zkutil::CreateMode::PersistentSequential));
        }

        if (auto txn = query_context->getZooKeeperMetadataTransaction())
        {
            /// It would be better to clone ops instead of moving, so we could retry on ZBADVERSION,
            /// but clone() is not implemented for Coordination::Request.
            txn->moveOpsTo(ops);
            /// NOTE: IDatabase::alterTable(...) is called when executing ALTER_METADATA queue entry without query context,
            /// so we have to update metadata of DatabaseReplicated here.
            String metadata_zk_path = fs::path(txn->getDatabaseZooKeeperPath()) / "metadata" / escapeForFileName(table_id.table_name);
            auto ast = DatabaseCatalog::instance().getDatabase(table_id.database_name)->getCreateTableQuery(table_id.table_name, query_context);
            applyMetadataChangesToCreateQuery(ast, future_metadata);
            ops.emplace_back(zkutil::makeSetRequest(metadata_zk_path, getObjectDefinitionFromCreateQuery(ast), -1));
        }

        Coordination::Responses results;
        Coordination::Error rc = zookeeper->tryMulti(ops, results);

        /// For the sake of constitency with mechanics of concurrent background process of assigning parts merge tasks
        /// this placeholder must be held up until the moment of committing into ZK of the mutation entry
        /// See ReplicatedMergeTreeMergePredicate::canMergeTwoParts() method
        partition_block_numbers_holder.reset();

        if (rc == Coordination::Error::ZOK)
        {
            if (have_mutation)
            {
                /// ALTER_METADATA record in replication /log
                String alter_path = dynamic_cast<const Coordination::CreateResponse &>(*results[alter_path_idx]).path_created;
                alter_entry->znode_name = alter_path.substr(alter_path.find_last_of('/') + 1);

                /// ReplicatedMergeTreeMutationEntry record in /mutations
                String mutation_path = dynamic_cast<const Coordination::CreateResponse &>(*results[mutation_path_idx]).path_created;
                mutation_znode = mutation_path.substr(mutation_path.find_last_of('/') + 1);
            }
            else
            {
                /// ALTER_METADATA record in replication /log
                String alter_path = dynamic_cast<const Coordination::CreateResponse &>(*results[alter_path_idx]).path_created;
                alter_entry->znode_name = alter_path.substr(alter_path.find_last_of('/') + 1);
            }
            break;
        }
        else if (rc == Coordination::Error::ZBADVERSION)
        {
            if (results[0]->error != Coordination::Error::ZOK)
                throw Exception("Metadata on replica is not up to date with common metadata in Zookeeper. It means that this replica still not applied some of previous alters."
                                " Probably too many alters executing concurrently (highly not recommended). You can retry this error",
                    ErrorCodes::CANNOT_ASSIGN_ALTER);

            /// Cannot retry automatically, because some zookeeper ops were lost on the first attempt. Will retry on DDLWorker-level.
            if (query_context->getZooKeeperMetadataTransaction())
                throw Exception("Cannot execute alter, because mutations version was suddenly changed due to concurrent alter",
                                ErrorCodes::CANNOT_ASSIGN_ALTER);

            continue;
        }
        else
        {
            throw Coordination::Exception("Alter cannot be assigned because of Zookeeper error", rc);
        }
    }

    table_lock_holder.reset();

    LOG_DEBUG(log, "Updated shared metadata nodes in ZooKeeper. Waiting for replicas to apply changes.");
    waitForLogEntryToBeProcessedIfNecessary(*alter_entry, query_context, "Some replicas doesn't finish metadata alter: ");

    if (mutation_znode)
    {
        LOG_DEBUG(log, "Metadata changes applied. Will wait for data changes.");
        waitMutation(*mutation_znode, query_context->getSettingsRef().replication_alter_partitions_sync);
        LOG_DEBUG(log, "Data changes applied.");
    }
}

/// If new version returns ordinary name, else returns part name containing the first and last month of the month
/// NOTE: use it in pair with getFakePartCoveringAllPartsInPartition(...)
static String getPartNamePossiblyFake(MergeTreeDataFormatVersion format_version, const MergeTreePartInfo & part_info)
{
    if (format_version < MERGE_TREE_DATA_MIN_FORMAT_VERSION_WITH_CUSTOM_PARTITIONING)
    {
        /// The date range is all month long.
        const auto & lut = DateLUT::instance();
        time_t start_time = lut.YYYYMMDDToDate(parse<UInt32>(part_info.partition_id + "01"));
        DayNum left_date = DayNum(lut.toDayNum(start_time).toUnderType());
        DayNum right_date = DayNum(static_cast<size_t>(left_date) + lut.daysInMonth(start_time) - 1);
        return part_info.getPartNameV0(left_date, right_date);
    }

    return part_info.getPartName();
}

bool StorageReplicatedMergeTree::getFakePartCoveringAllPartsInPartition(const String & partition_id, MergeTreePartInfo & part_info,
                                                                        std::optional<EphemeralLockInZooKeeper> & delimiting_block_lock, bool for_replace_range)
{
    /// Even if there is no data in the partition, you still need to mark the range for deletion.
    /// - Because before executing DETACH, tasks for downloading parts to this partition can be executed.
    Int64 left = 0;

    /** Let's skip one number in `block_numbers` for the partition being deleted, and we will only delete parts until this number.
      * This prohibits merges of deleted parts with the new inserted
      * Invariant: merges of deleted parts with other parts do not appear in the log.
      * NOTE: If you need to similarly support a `DROP PART` request, you will have to think of some new mechanism for it,
      *     to guarantee this invariant.
      */
    Int64 right;
    Int64 mutation_version;

    {
        auto zookeeper = getZooKeeper();
        delimiting_block_lock = allocateBlockNumber(partition_id, zookeeper);
        right = delimiting_block_lock->getNumber();
        /// Make sure we cover all parts in drop range.
        /// There might be parts with mutation version greater than current block number
        /// if some part mutation has been assigned after block number allocation, but before creation of DROP_RANGE entry.
        mutation_version = MergeTreePartInfo::MAX_BLOCK_NUMBER;
    }

    if (for_replace_range)
    {
        /// NOTE Do not decrement max block number for REPLACE_RANGE, because there are invariants:
        /// - drop range for REPLACE PARTITION must contain at least 2 blocks (1 skipped block and at least 1 real block)
        /// - drop range for MOVE PARTITION/ATTACH PARTITION FROM always contains 1 block

        /// NOTE UINT_MAX was previously used as max level for REPLACE/MOVE PARTITION (it was incorrect)
        part_info = MergeTreePartInfo(partition_id, left, right, MergeTreePartInfo::MAX_LEVEL, mutation_version);
        return right != 0;
    }

    /// Empty partition.
    if (right == 0)
        return false;

    --right;

    /// Artificial high level is chosen, to make this part "covering" all parts inside.
    part_info = MergeTreePartInfo(partition_id, left, right, MergeTreePartInfo::MAX_LEVEL, mutation_version);
    return true;
}

void StorageReplicatedMergeTree::restoreMetadataInZooKeeper()
{
    LOG_INFO(log, "Restoring replica metadata");

    if (!is_readonly || has_metadata_in_zookeeper)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "It's a bug: replica is not readonly");

    if (are_restoring_replica.exchange(true))
        throw Exception(ErrorCodes::CONCURRENT_ACCESS_NOT_SUPPORTED, "Replica restoration in progress");

    auto metadata_snapshot = getInMemoryMetadataPtr();

    const DataPartsVector all_parts = getAllDataPartsVector();
    Strings active_parts_names;

    /// Why all parts (not only Committed) are moved to detached/:
    /// After ZK metadata restoration ZK resets sequential counters (including block number counters), so one may
    /// potentially encounter a situation that a part we want to attach already exists.
    for (const auto & part : all_parts)
    {
        if (part->getState() == DataPartState::Committed)
            active_parts_names.push_back(part->name);

        forgetPartAndMoveToDetached(part);
    }

    LOG_INFO(log, "Moved all parts to detached/");

    const bool is_first_replica = createTableIfNotExists(metadata_snapshot);

    LOG_INFO(log, "Created initial ZK nodes, replica is first: {}", is_first_replica);

    if (!is_first_replica)
        createReplica(metadata_snapshot);

    createNewZooKeeperNodes();

    LOG_INFO(log, "Created ZK nodes for table");

    is_readonly = false;
    has_metadata_in_zookeeper = true;

    if (is_first_replica)
        for (const String& part_name : active_parts_names)
            attachPartition(std::make_shared<ASTLiteral>(part_name), metadata_snapshot, true, getContext());

    LOG_INFO(log, "Attached all partitions, starting table");

    startup();

    are_restoring_replica.store(false);
}

void StorageReplicatedMergeTree::dropPartNoWaitNoThrow(const String & part_name)
{
    assertNotReadonly();
    if (!is_leader)
        throw Exception("DROP PART cannot be done on this replica because it is not a leader", ErrorCodes::NOT_A_LEADER);

    zkutil::ZooKeeperPtr zookeeper = getZooKeeper();
    LogEntry entry;

    dropPartImpl(zookeeper, part_name, entry, /*detach=*/ false, /*throw_if_noop=*/ false);
}

void StorageReplicatedMergeTree::dropPart(const String & part_name, bool detach, ContextPtr query_context)
{
    assertNotReadonly();
    if (!is_leader)
        throw Exception("DROP PART cannot be done on this replica because it is not a leader", ErrorCodes::NOT_A_LEADER);

    zkutil::ZooKeeperPtr zookeeper = getZooKeeper();
    LogEntry entry;

    dropPartImpl(zookeeper, part_name, entry, detach, /*throw_if_noop=*/ true);

    waitForLogEntryToBeProcessedIfNecessary(entry, query_context);
}

void StorageReplicatedMergeTree::dropPartition(const ASTPtr & partition, bool detach, ContextPtr query_context)
{
    assertNotReadonly();
    if (!is_leader)
        throw Exception("DROP PARTITION cannot be done on this replica because it is not a leader", ErrorCodes::NOT_A_LEADER);

    zkutil::ZooKeeperPtr zookeeper = getZooKeeper();
    LogEntry entry;

    String partition_id = getPartitionIDFromQuery(partition, query_context);
    bool did_drop = dropAllPartsInPartition(*zookeeper, partition_id, entry, query_context, detach);

    if (did_drop)
    {
        waitForLogEntryToBeProcessedIfNecessary(entry, query_context);
        cleanLastPartNode(partition_id);
    }
}


void StorageReplicatedMergeTree::truncate(
    const ASTPtr &, const StorageMetadataPtr &, ContextPtr query_context, TableExclusiveLockHolder & table_lock)
{
    table_lock.release();   /// Truncate is done asynchronously.

    assertNotReadonly();
    if (!is_leader)
        throw Exception("TRUNCATE cannot be done on this replica because it is not a leader", ErrorCodes::NOT_A_LEADER);

    zkutil::ZooKeeperPtr zookeeper = getZooKeeper();

    Strings partitions = zookeeper->getChildren(fs::path(zookeeper_path) / "block_numbers");

    std::vector<std::unique_ptr<LogEntry>> entries_to_wait;
    entries_to_wait.reserve(partitions.size());
    for (String & partition_id : partitions)
    {
        auto entry = std::make_unique<LogEntry>();
        if (dropAllPartsInPartition(*zookeeper, partition_id, *entry, query_context, false))
            entries_to_wait.push_back(std::move(entry));
    }

    for (const auto & entry : entries_to_wait)
        waitForLogEntryToBeProcessedIfNecessary(*entry, query_context);
}


PartitionCommandsResultInfo StorageReplicatedMergeTree::attachPartition(
    const ASTPtr & partition,
    const StorageMetadataPtr & metadata_snapshot,
    bool attach_part,
    ContextPtr query_context)
{
    assertNotReadonly();

    PartitionCommandsResultInfo results;
    PartsTemporaryRename renamed_parts(*this, "detached/");
    MutableDataPartsVector loaded_parts = tryLoadPartsToAttach(partition, attach_part, query_context, renamed_parts);

    /// TODO Allow to use quorum here.
    ReplicatedMergeTreeSink output(*this, metadata_snapshot, 0, 0, 0, false, false, query_context,
        /*is_attach*/true);

    for (size_t i = 0; i < loaded_parts.size(); ++i)
    {
        const String old_name = loaded_parts[i]->name;

        output.writeExistingPart(loaded_parts[i]);

        renamed_parts.old_and_new_names[i].first.clear();

        LOG_DEBUG(log, "Attached part {} as {}", old_name, loaded_parts[i]->name);

        results.push_back(PartitionCommandResultInfo{
            .partition_id = loaded_parts[i]->info.partition_id,
            .part_name = loaded_parts[i]->name,
            .old_part_name = old_name,
        });
    }
    return results;
}


void StorageReplicatedMergeTree::checkTableCanBeDropped() const
{
    auto table_id = getStorageID();
    getContext()->checkTableCanBeDropped(table_id.database_name, table_id.table_name, getTotalActiveSizeInBytes());
}

void StorageReplicatedMergeTree::checkTableCanBeRenamed() const
{
    if (!allow_renaming)
        throw Exception("Cannot rename Replicated table, because zookeeper_path contains implicit 'database' or 'table' macro. "
                        "We cannot rename path in ZooKeeper, so path may become inconsistent with table name. If you really want to rename table, "
                        "you should edit metadata file first and restart server or reattach the table.", ErrorCodes::NOT_IMPLEMENTED);
}

void StorageReplicatedMergeTree::rename(const String & new_path_to_table_data, const StorageID & new_table_id)
{
    checkTableCanBeRenamed();
    MergeTreeData::rename(new_path_to_table_data, new_table_id);

    /// Update table name in zookeeper
    if (!is_readonly)
    {
        /// We don't do it for readonly tables, because it will be updated on next table startup.
        /// It is also Ok to skip ZK error for the same reason.
        try
        {
            auto zookeeper = getZooKeeper();
            zookeeper->set(fs::path(replica_path) / "host", getReplicatedMergeTreeAddress().toString());
        }
        catch (Coordination::Exception & e)
        {
            LOG_WARNING(log, "Cannot update the value of 'host' node (replica address) in ZooKeeper: {}", e.displayText());
        }
    }

    /// TODO: You can update names of loggers.
}


bool StorageReplicatedMergeTree::existsNodeCached(const std::string & path) const
{
    {
        std::lock_guard lock(existing_nodes_cache_mutex);
        if (existing_nodes_cache.count(path))
            return true;
    }

    bool res = getZooKeeper()->exists(path);

    if (res)
    {
        std::lock_guard lock(existing_nodes_cache_mutex);
        existing_nodes_cache.insert(path);
    }

    return res;
}


std::optional<EphemeralLockInZooKeeper>
StorageReplicatedMergeTree::allocateBlockNumber(
    const String & partition_id, const zkutil::ZooKeeperPtr & zookeeper, const String & zookeeper_block_id_path, const String & zookeeper_path_prefix) const
{
    String zookeeper_table_path;
    if (zookeeper_path_prefix.empty())
        zookeeper_table_path = zookeeper_path;
    else
        zookeeper_table_path = zookeeper_path_prefix;

    /// Lets check for duplicates in advance, to avoid superfluous block numbers allocation
    Coordination::Requests deduplication_check_ops;
    if (!zookeeper_block_id_path.empty())
    {
        deduplication_check_ops.emplace_back(zkutil::makeCreateRequest(zookeeper_block_id_path, "", zkutil::CreateMode::Persistent));
        deduplication_check_ops.emplace_back(zkutil::makeRemoveRequest(zookeeper_block_id_path, -1));
    }

    String block_numbers_path = fs::path(zookeeper_table_path) / "block_numbers";
    String partition_path = fs::path(block_numbers_path) / partition_id;

    if (!existsNodeCached(partition_path))
    {
        Coordination::Requests ops;
        ops.push_back(zkutil::makeCreateRequest(partition_path, "", zkutil::CreateMode::Persistent));
        /// We increment data version of the block_numbers node so that it becomes possible
        /// to check in a ZK transaction that the set of partitions didn't change
        /// (unfortunately there is no CheckChildren op).
        ops.push_back(zkutil::makeSetRequest(block_numbers_path, "", -1));

        Coordination::Responses responses;
        Coordination::Error code = zookeeper->tryMulti(ops, responses);
        if (code != Coordination::Error::ZOK && code != Coordination::Error::ZNODEEXISTS)
            zkutil::KeeperMultiException::check(code, ops, responses);
    }

    EphemeralLockInZooKeeper lock;
    /// 2 RTT
    try
    {
        lock = EphemeralLockInZooKeeper(
            fs::path(partition_path) / "block-", fs::path(zookeeper_table_path) / "temp", *zookeeper, &deduplication_check_ops);
    }
    catch (const zkutil::KeeperMultiException & e)
    {
        if (e.code == Coordination::Error::ZNODEEXISTS && e.getPathForFirstFailedOp() == zookeeper_block_id_path)
            return {};

        throw Exception("Cannot allocate block number in ZooKeeper: " + e.displayText(), ErrorCodes::KEEPER_EXCEPTION);
    }
    catch (const Coordination::Exception & e)
    {
        throw Exception("Cannot allocate block number in ZooKeeper: " + e.displayText(), ErrorCodes::KEEPER_EXCEPTION);
    }

    return {std::move(lock)};
}


Strings StorageReplicatedMergeTree::tryWaitForAllReplicasToProcessLogEntry(
    const String & table_zookeeper_path, const ReplicatedMergeTreeLogEntryData & entry, Int64 wait_for_inactive_timeout)
{
    LOG_DEBUG(log, "Waiting for all replicas to process {}", entry.znode_name);

    auto zookeeper = getZooKeeper();
    Strings replicas = zookeeper->getChildren(fs::path(table_zookeeper_path) / "replicas");
    Strings unwaited;
    bool wait_for_inactive = wait_for_inactive_timeout != 0;
    for (const String & replica : replicas)
    {
        if (wait_for_inactive || zookeeper->exists(fs::path(table_zookeeper_path) / "replicas" / replica / "is_active"))
        {
            if (!tryWaitForReplicaToProcessLogEntry(table_zookeeper_path, replica, entry, wait_for_inactive_timeout))
                unwaited.push_back(replica);
        }
        else
        {
            unwaited.push_back(replica);
        }
    }

    LOG_DEBUG(log, "Finished waiting for all replicas to process {}", entry.znode_name);
    return unwaited;
}

void StorageReplicatedMergeTree::waitForAllReplicasToProcessLogEntry(
    const String & table_zookeeper_path, const ReplicatedMergeTreeLogEntryData & entry, Int64 wait_for_inactive_timeout, const String & error_context)
{
    Strings unfinished_replicas = tryWaitForAllReplicasToProcessLogEntry(table_zookeeper_path, entry, wait_for_inactive_timeout);
    if (unfinished_replicas.empty())
        return;

    throw Exception(ErrorCodes::UNFINISHED, "{}Timeout exceeded while waiting for replicas {} to process entry {}. "
                    "Probably some replicas are inactive", error_context, fmt::join(unfinished_replicas, ", "), entry.znode_name);
}

void StorageReplicatedMergeTree::waitForLogEntryToBeProcessedIfNecessary(const ReplicatedMergeTreeLogEntryData & entry, ContextPtr query_context, const String & error_context)
{
    /// If necessary, wait until the operation is performed on itself or on all replicas.
    Int64 wait_for_inactive_timeout = query_context->getSettingsRef().replication_wait_for_inactive_replica_timeout;
    if (query_context->getSettingsRef().replication_alter_partitions_sync == 1)
    {
        bool finished = tryWaitForReplicaToProcessLogEntry(zookeeper_path, replica_name, entry, wait_for_inactive_timeout);
        if (!finished)
        {
            throw Exception(ErrorCodes::UNFINISHED, "{}Log entry {} is not precessed on local replica, "
                            "most likely because the replica was shut down.", error_context, entry.znode_name);
        }
    }
    else if (query_context->getSettingsRef().replication_alter_partitions_sync == 2)
    {
        waitForAllReplicasToProcessLogEntry(zookeeper_path, entry, wait_for_inactive_timeout, error_context);
    }
}

bool StorageReplicatedMergeTree::tryWaitForReplicaToProcessLogEntry(
    const String & table_zookeeper_path, const String & replica, const ReplicatedMergeTreeLogEntryData & entry, Int64 wait_for_inactive_timeout)
{
    String entry_str = entry.toString();
    String log_node_name;

    /** Wait for entries from `log` directory (a common log, from where replicas copy entries to their queue) to be processed.
      *
      * The problem is that the numbers (`sequential` node) of the queue elements in `log` and in `queue` do not match.
      * (And the numbers of the same log element for different replicas do not match in the `queue`.)
      */

    /** First, you need to wait until replica takes `queue` element from the `log` to its queue,
      *  if it has not been done already (see the `pullLogsToQueue` function).
      *
      * To do this, check its node `log_pointer` - the maximum number of the element taken from `log` + 1.
      */

    bool waiting_itself = replica == replica_name;
    /// Do not wait if timeout is zero
    bool wait_for_inactive = wait_for_inactive_timeout != 0;
    /// Wait for unlimited time if timeout is negative
    bool check_timeout = wait_for_inactive_timeout > 0;
    Stopwatch time_waiting;

    const auto & stop_waiting = [&]()
    {
        bool stop_waiting_itself = waiting_itself && partial_shutdown_called;
        bool timeout_exceeded = check_timeout && wait_for_inactive_timeout < time_waiting.elapsedSeconds();
        bool stop_waiting_inactive = (!wait_for_inactive || timeout_exceeded)
            && !getZooKeeper()->exists(fs::path(table_zookeeper_path) / "replicas" / replica / "is_active");
        return is_dropped || stop_waiting_itself || stop_waiting_inactive;
    };

    /// Don't recheck ZooKeeper too often
    constexpr auto event_wait_timeout_ms = 3000;

    if (!startsWith(entry.znode_name, "log-"))
        throw Exception("Logical error: unexpected name of log node: " + entry.znode_name, ErrorCodes::LOGICAL_ERROR);

    {
        /// Take the number from the node name `log-xxxxxxxxxx`.
        UInt64 log_index = parse<UInt64>(entry.znode_name.substr(entry.znode_name.size() - 10));
        log_node_name = entry.znode_name;

        LOG_DEBUG(log, "Waiting for {} to pull {} to queue", replica, log_node_name);

        /// Let's wait until entry gets into the replica queue.
        bool pulled_to_queue = false;
        while (!stop_waiting())
        {
            zkutil::EventPtr event = std::make_shared<Poco::Event>();

            String log_pointer = getZooKeeper()->get(fs::path(table_zookeeper_path) / "replicas" / replica / "log_pointer", nullptr, event);
            if (!log_pointer.empty() && parse<UInt64>(log_pointer) > log_index)
            {
                pulled_to_queue = true;
                break;
            }

            /// Wait with timeout because we can be already shut down, but not dropped.
            /// So log_pointer node will exist, but we will never update it because all background threads already stopped.
            /// It can lead to query hung because table drop query can wait for some query (alter, optimize, etc) which called this method,
            /// but the query will never finish because the drop already shut down the table.
            event->tryWait(event_wait_timeout_ms);
        }

        if (!pulled_to_queue)
            return false;
    }

    LOG_DEBUG(log, "Looking for node corresponding to {} in {} queue", log_node_name, replica);

    /** Second - find the corresponding entry in the queue of the specified replica.
      * Its number may not match the `log` node. Therefore, we search by comparing the content.
      */

    Strings queue_entries = getZooKeeper()->getChildren(fs::path(table_zookeeper_path) / "replicas" / replica / "queue");
    String queue_entry_to_wait_for;

    for (const String & entry_name : queue_entries)
    {
        String queue_entry_str;
        bool exists = getZooKeeper()->tryGet(fs::path(table_zookeeper_path) / "replicas" / replica / "queue" / entry_name, queue_entry_str);
        if (exists && queue_entry_str == entry_str)
        {
            queue_entry_to_wait_for = entry_name;
            break;
        }
    }

    /// While looking for the record, it has already been executed and deleted.
    if (queue_entry_to_wait_for.empty())
    {
        LOG_DEBUG(log, "No corresponding node found. Assuming it has been already processed. Found {} nodes", queue_entries.size());
        return true;
    }

    LOG_DEBUG(log, "Waiting for {} to disappear from {} queue", queue_entry_to_wait_for, replica);

    /// Third - wait until the entry disappears from the replica queue or replica become inactive.
    String path_to_wait_on = fs::path(table_zookeeper_path) / "replicas" / replica / "queue" / queue_entry_to_wait_for;

    return getZooKeeper()->waitForDisappear(path_to_wait_on, stop_waiting);
}


void StorageReplicatedMergeTree::getStatus(Status & res, bool with_zk_fields)
{
    auto zookeeper = tryGetZooKeeper();
    const auto storage_settings_ptr = getSettings();

    res.is_leader = is_leader;
    res.can_become_leader = storage_settings_ptr->replicated_can_become_leader;
    res.is_readonly = is_readonly;
    res.is_session_expired = !zookeeper || zookeeper->expired();

    res.queue = queue.getStatus();
    res.absolute_delay = getAbsoluteDelay(); /// NOTE: may be slightly inconsistent with queue status.

    res.parts_to_check = part_check_thread.size();

    res.zookeeper_path = zookeeper_path;
    res.replica_name = replica_name;
    res.replica_path = replica_path;
    res.columns_version = -1;

    res.log_max_index = 0;
    res.log_pointer = 0;
    res.total_replicas = 0;
    res.active_replicas = 0;
    res.last_queue_update_exception = getLastQueueUpdateException();

    if (with_zk_fields && !res.is_session_expired)
    {
        try
        {
            auto log_entries = zookeeper->getChildren(fs::path(zookeeper_path) / "log");

            if (!log_entries.empty())
            {
                const String & last_log_entry = *std::max_element(log_entries.begin(), log_entries.end());
                res.log_max_index = parse<UInt64>(last_log_entry.substr(strlen("log-")));
            }

            String log_pointer_str = zookeeper->get(fs::path(replica_path) / "log_pointer");
            res.log_pointer = log_pointer_str.empty() ? 0 : parse<UInt64>(log_pointer_str);

            auto all_replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");
            res.total_replicas = all_replicas.size();

            for (const String & replica : all_replicas)
            {
                bool is_replica_active = zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "is_active");
                res.active_replicas += static_cast<UInt8>(is_replica_active);
                res.replica_is_active.emplace(replica, is_replica_active);
            }
        }
        catch (const Coordination::Exception &)
        {
            res.zookeeper_exception = getCurrentExceptionMessage(false);
        }
    }
}


void StorageReplicatedMergeTree::getQueue(LogEntriesData & res, String & replica_name_)
{
    replica_name_ = replica_name;
    queue.getEntries(res);
}

std::vector<PartMovesBetweenShardsOrchestrator::Entry> StorageReplicatedMergeTree::getPartMovesBetweenShardsEntries()
{
    return part_moves_between_shards_orchestrator.getEntries();
}

time_t StorageReplicatedMergeTree::getAbsoluteDelay() const
{
    time_t min_unprocessed_insert_time = 0;
    time_t max_processed_insert_time = 0;
    queue.getInsertTimes(min_unprocessed_insert_time, max_processed_insert_time);

    /// Load start time, then finish time to avoid reporting false delay when start time is updated
    /// between loading of two variables.
    time_t queue_update_start_time = last_queue_update_start_time.load();
    time_t queue_update_finish_time = last_queue_update_finish_time.load();

    time_t current_time = time(nullptr);

    if (!queue_update_finish_time)
    {
        /// We have not updated queue even once yet (perhaps replica is readonly).
        /// As we have no info about the current state of replication log, return effectively infinite delay.
        return current_time;
    }
    else if (min_unprocessed_insert_time)
    {
        /// There are some unprocessed insert entries in queue.
        return (current_time > min_unprocessed_insert_time) ? (current_time - min_unprocessed_insert_time) : 0;
    }
    else if (queue_update_start_time > queue_update_finish_time)
    {
        /// Queue is empty, but there are some in-flight or failed queue update attempts
        /// (likely because of problems with connecting to ZooKeeper).
        /// Return the time passed since last attempt.
        return (current_time > queue_update_start_time) ? (current_time - queue_update_start_time) : 0;
    }
    else
    {
        /// Everything is up-to-date.
        return 0;
    }
}

void StorageReplicatedMergeTree::getReplicaDelays(time_t & out_absolute_delay, time_t & out_relative_delay)
{
    assertNotReadonly();

    time_t current_time = time(nullptr);

    out_absolute_delay = getAbsoluteDelay();
    out_relative_delay = 0;
    const auto storage_settings_ptr = getSettings();

    /** Relative delay is the maximum difference of absolute delay from any other replica,
      *  (if this replica lags behind any other live replica, or zero, otherwise).
      * Calculated only if the absolute delay is large enough.
      */

    if (out_absolute_delay < static_cast<time_t>(storage_settings_ptr->min_relative_delay_to_measure))
        return;

    auto zookeeper = getZooKeeper();

    time_t max_replicas_unprocessed_insert_time = 0;
    bool have_replica_with_nothing_unprocessed = false;

    Strings replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");

    for (const auto & replica : replicas)
    {
        if (replica == replica_name)
            continue;

        /// Skip dead replicas.
        if (!zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "is_active"))
            continue;

        String value;
        if (!zookeeper->tryGet(fs::path(zookeeper_path) / "replicas" / replica / "min_unprocessed_insert_time", value))
            continue;

        time_t replica_time = value.empty() ? 0 : parse<time_t>(value);

        if (replica_time == 0)
        {
            /** Note
              * The conclusion that the replica does not lag may be incorrect,
              *  because the information about `min_unprocessed_insert_time` is taken
              *  only from that part of the log that has been moved to the queue.
              * If the replica for some reason has stalled `queueUpdatingTask`,
              *  then `min_unprocessed_insert_time` will be incorrect.
              */

            have_replica_with_nothing_unprocessed = true;
            break;
        }

        if (replica_time > max_replicas_unprocessed_insert_time)
            max_replicas_unprocessed_insert_time = replica_time;
    }

    if (have_replica_with_nothing_unprocessed)
        out_relative_delay = out_absolute_delay;
    else
    {
        max_replicas_unprocessed_insert_time = std::min(current_time, max_replicas_unprocessed_insert_time);
        time_t min_replicas_delay = current_time - max_replicas_unprocessed_insert_time;
        if (out_absolute_delay > min_replicas_delay)
            out_relative_delay = out_absolute_delay - min_replicas_delay;
    }
}

void StorageReplicatedMergeTree::fetchPartition(
    const ASTPtr & partition,
    const StorageMetadataPtr & metadata_snapshot,
    const String & from_,
    bool fetch_part,
    ContextPtr query_context)
{
    Macros::MacroExpansionInfo info;
    info.expand_special_macros_only = false; //-V1048
    info.table_id = getStorageID();
    info.table_id.uuid = UUIDHelpers::Nil;
    auto expand_from = query_context->getMacros()->expand(from_, info);
    String auxiliary_zookeeper_name = extractZooKeeperName(expand_from);
    String from = extractZooKeeperPath(expand_from);
    if (from.empty())
        throw Exception("ZooKeeper path should not be empty", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

    zkutil::ZooKeeperPtr zookeeper;
    if (auxiliary_zookeeper_name != default_zookeeper_name)
        zookeeper = getContext()->getAuxiliaryZooKeeper(auxiliary_zookeeper_name);
    else
        zookeeper = getZooKeeper();

    if (from.back() == '/')
        from.resize(from.size() - 1);

    if (fetch_part)
    {
        String part_name = partition->as<ASTLiteral &>().value.safeGet<String>();
        auto part_path = findReplicaHavingPart(part_name, from, zookeeper);

        if (part_path.empty())
            throw Exception(ErrorCodes::NO_REPLICA_HAS_PART, "Part {} does not exist on any replica", part_name);
        /** Let's check that there is no such part in the `detached` directory (where we will write the downloaded parts).
          * Unreliable (there is a race condition) - such a part may appear a little later.
          */
        if (checkIfDetachedPartExists(part_name))
            throw Exception(ErrorCodes::DUPLICATE_DATA_PART, "Detached part " + part_name + " already exists.");
        LOG_INFO(log, "Will fetch part {} from shard {} (zookeeper '{}')", part_name, from_, auxiliary_zookeeper_name);

        try
        {
            /// part name , metadata, part_path , true, 0, zookeeper
            if (!fetchPart(part_name, metadata_snapshot, part_path, true, 0, zookeeper))
                throw Exception(ErrorCodes::UNFINISHED, "Failed to fetch part {} from {}", part_name, from_);
        }
        catch (const DB::Exception & e)
        {
            if (e.code() != ErrorCodes::RECEIVED_ERROR_FROM_REMOTE_IO_SERVER && e.code() != ErrorCodes::RECEIVED_ERROR_TOO_MANY_REQUESTS
                && e.code() != ErrorCodes::CANNOT_READ_ALL_DATA)
                throw;

            LOG_INFO(log, e.displayText());
        }
        return;
    }

    String partition_id = getPartitionIDFromQuery(partition, query_context);
    LOG_INFO(log, "Will fetch partition {} from shard {} (zookeeper '{}')", partition_id, from_, auxiliary_zookeeper_name);

    /** Let's check that there is no such partition in the `detached` directory (where we will write the downloaded parts).
      * Unreliable (there is a race condition) - such a partition may appear a little later.
      */
    if (checkIfDetachedPartitionExists(partition_id))
        throw Exception("Detached partition " + partition_id + " already exists.", ErrorCodes::PARTITION_ALREADY_EXISTS);

    zkutil::Strings replicas;
    zkutil::Strings active_replicas;
    String best_replica;

    {

        /// List of replicas of source shard.
        replicas = zookeeper->getChildren(fs::path(from) / "replicas");

        /// Leave only active replicas.
        active_replicas.reserve(replicas.size());

        for (const String & replica : replicas)
            if (zookeeper->exists(fs::path(from) / "replicas" / replica / "is_active"))
                active_replicas.push_back(replica);

        if (active_replicas.empty())
            throw Exception("No active replicas for shard " + from, ErrorCodes::NO_ACTIVE_REPLICAS);

        /** You must select the best (most relevant) replica.
        * This is a replica with the maximum `log_pointer`, then with the minimum `queue` size.
        * NOTE This is not exactly the best criteria. It does not make sense to download old partitions,
        *  and it would be nice to be able to choose the replica closest by network.
        * NOTE Of course, there are data races here. You can solve it by retrying.
        */
        Int64 max_log_pointer = -1;
        UInt64 min_queue_size = std::numeric_limits<UInt64>::max();

        for (const String & replica : active_replicas)
        {
            String current_replica_path = fs::path(from) / "replicas" / replica;

            String log_pointer_str = zookeeper->get(fs::path(current_replica_path) / "log_pointer");
            Int64 log_pointer = log_pointer_str.empty() ? 0 : parse<UInt64>(log_pointer_str);

            Coordination::Stat stat;
            zookeeper->get(fs::path(current_replica_path) / "queue", &stat);
            size_t queue_size = stat.numChildren;

            if (log_pointer > max_log_pointer
                || (log_pointer == max_log_pointer && queue_size < min_queue_size))
            {
                max_log_pointer = log_pointer;
                min_queue_size = queue_size;
                best_replica = replica;
            }
        }
    }

    if (best_replica.empty())
        throw Exception("Logical error: cannot choose best replica.", ErrorCodes::LOGICAL_ERROR);

    LOG_INFO(log, "Found {} replicas, {} of them are active. Selected {} to fetch from.", replicas.size(), active_replicas.size(), best_replica);

    String best_replica_path = fs::path(from) / "replicas" / best_replica;

    /// Let's find out which parts are on the best replica.

    /** Trying to download these parts.
      * Some of them could be deleted due to the merge.
      * In this case, update the information about the available parts and try again.
      */

    unsigned try_no = 0;
    Strings missing_parts;
    do
    {
        if (try_no)
            LOG_INFO(log, "Some of parts ({}) are missing. Will try to fetch covering parts.", missing_parts.size());

        if (try_no >= query_context->getSettings().max_fetch_partition_retries_count)
            throw Exception("Too many retries to fetch parts from " + best_replica_path, ErrorCodes::TOO_MANY_RETRIES_TO_FETCH_PARTS);

        Strings parts = zookeeper->getChildren(fs::path(best_replica_path) / "parts");
        ActiveDataPartSet active_parts_set(format_version, parts);
        Strings parts_to_fetch;

        if (missing_parts.empty())
        {
            parts_to_fetch = active_parts_set.getParts();

            /// Leaving only the parts of the desired partition.
            Strings parts_to_fetch_partition;
            for (const String & part : parts_to_fetch)
            {
                if (MergeTreePartInfo::fromPartName(part, format_version).partition_id == partition_id)
                    parts_to_fetch_partition.push_back(part);
            }

            parts_to_fetch = std::move(parts_to_fetch_partition);

            if (parts_to_fetch.empty())
                throw Exception("Partition " + partition_id + " on " + best_replica_path + " doesn't exist", ErrorCodes::PARTITION_DOESNT_EXIST);
        }
        else
        {
            for (const String & missing_part : missing_parts)
            {
                String containing_part = active_parts_set.getContainingPart(missing_part);
                if (!containing_part.empty())
                    parts_to_fetch.push_back(containing_part);
                else
                    LOG_WARNING(log, "Part {} on replica {} has been vanished.", missing_part, best_replica_path);
            }
        }

        LOG_INFO(log, "Parts to fetch: {}", parts_to_fetch.size());

        missing_parts.clear();
        for (const String & part : parts_to_fetch)
        {
            bool fetched = false;

            try
            {
                fetched = fetchPart(part, metadata_snapshot, best_replica_path, true, 0, zookeeper);
            }
            catch (const DB::Exception & e)
            {
                if (e.code() != ErrorCodes::RECEIVED_ERROR_FROM_REMOTE_IO_SERVER && e.code() != ErrorCodes::RECEIVED_ERROR_TOO_MANY_REQUESTS
                    && e.code() != ErrorCodes::CANNOT_READ_ALL_DATA)
                    throw;

                LOG_INFO(log, e.displayText());
            }

            if (!fetched)
                missing_parts.push_back(part);
        }

        ++try_no;
    } while (!missing_parts.empty());
}


void StorageReplicatedMergeTree::mutate(const MutationCommands & commands, ContextPtr query_context)
{
    /// Overview of the mutation algorithm.
    ///
    /// When the client executes a mutation, this method is called. It acquires block numbers in all
    /// partitions, saves them in the mutation entry and writes the mutation entry to a new ZK node in
    /// the /mutations folder. This block numbers are needed to determine which parts should be mutated and
    /// which shouldn't (parts inserted after the mutation will have the block number higher than the
    /// block number acquired by the mutation in that partition and so will not be mutatied).
    /// This block number is called "mutation version" in that partition.
    ///
    /// Mutation versions are acquired atomically in all partitions, so the case when an insert in some
    /// partition has the block number higher than the mutation version but the following insert into another
    /// partition acquires the block number lower than the mutation version in that partition is impossible.
    /// Another important invariant: mutation entries appear in /mutations in the order of their mutation
    /// versions (in any partition). This means that mutations form a sequence and we can execute them in
    /// the order of their mutation versions and not worry that some mutation with the smaller version
    /// will suddenly appear.
    ///
    /// During mutations individual parts are immutable - when we want to change the contents of a part
    /// we prepare the new part and add it to MergeTreeData (the original part gets replaced). The fact that
    /// we have mutated the part is recorded in the part->info.mutation field of MergeTreePartInfo.
    /// The relation with the original part is preserved because the new part covers the same block range
    /// as the original one.
    ///
    /// We then can for each part determine its "mutation version": the version of the last mutation in
    /// the mutation sequence that we regard as already applied to that part. All mutations with the greater
    /// version number will still need to be applied to that part.
    ///
    /// Execution of mutations is done asynchronously. All replicas watch the /mutations directory and
    /// load new mutation entries as they appear (see mutationsUpdatingTask()). Next we need to determine
    /// how to mutate individual parts consistently with part merges. This is done by the leader replica
    /// (see mergeSelectingTask() and class ReplicatedMergeTreeMergePredicate for details). Important
    /// invariants here are that a) all source parts for a single merge must have the same mutation version
    /// and b) any part can be mutated only once or merged only once (e.g. once we have decided to mutate
    /// a part then we need to execute that mutation and can assign merges only to the new part and not to the
    /// original part). Multiple consecutive mutations can be executed at once (without writing the
    /// intermediate result to a part).
    ///
    /// Leader replica records its decisions to the replication log (/log directory in ZK) in the form of
    /// MUTATE_PART entries and all replicas then execute them in the background pool
    /// (see MutateTask class). When a replica encounters a MUTATE_PART command, it is
    /// guaranteed that the corresponding mutation entry is already loaded (when we pull entries from
    /// replication log into the replica queue, we also load mutation entries). Note that just as with merges
    /// the replica can decide not to do the mutation locally and fetch the mutated part from another replica
    /// instead.
    ///
    /// Mutations of individual parts are in fact pretty similar to merges, e.g. their assignment and execution
    /// is governed by the same storage_settings. TODO: support a single "merge-mutation" operation when the data
    /// read from the the source parts is first mutated on the fly to some uniform mutation version and then
    /// merged to a resulting part.
    ///
    /// After all needed parts are mutated (i.e. all active parts have the mutation version greater than
    /// the version of this mutation), the mutation is considered done and can be deleted.

    ReplicatedMergeTreeMutationEntry mutation_entry;
    mutation_entry.source_replica = replica_name;
    mutation_entry.commands = commands;

    const String mutations_path = fs::path(zookeeper_path) / "mutations";
    const auto zookeeper = getZooKeeper();

    /// Update the mutations_path node when creating the mutation and check its version to ensure that
    /// nodes for mutations are created in the same order as the corresponding block numbers.
    /// Should work well if the number of concurrent mutation requests is small.
    while (true)
    {
        Coordination::Stat mutations_stat;
        zookeeper->get(mutations_path, &mutations_stat);

        PartitionBlockNumbersHolder partition_block_numbers_holder =
                allocateBlockNumbersInAffectedPartitions(mutation_entry.commands, query_context, zookeeper);

        mutation_entry.block_numbers = partition_block_numbers_holder.getBlockNumbers();
        mutation_entry.create_time = time(nullptr);

        /// The following version check guarantees the linearizability property for any pair of mutations:
        /// mutation with higher sequence number is guaranteed to have higher block numbers in every partition
        /// (and thus will be applied strictly according to sequence numbers of mutations)
        Coordination::Requests requests;
        requests.emplace_back(zkutil::makeSetRequest(mutations_path, String(), mutations_stat.version));
        requests.emplace_back(zkutil::makeCreateRequest(
            fs::path(mutations_path) / "", mutation_entry.toString(), zkutil::CreateMode::PersistentSequential));

        if (auto txn = query_context->getZooKeeperMetadataTransaction())
            txn->moveOpsTo(requests);

        Coordination::Responses responses;
        Coordination::Error rc = zookeeper->tryMulti(requests, responses);

        partition_block_numbers_holder.reset();

        if (rc == Coordination::Error::ZOK)
        {
            const String & path_created =
                dynamic_cast<const Coordination::CreateResponse *>(responses[1].get())->path_created;
            mutation_entry.znode_name = path_created.substr(path_created.find_last_of('/') + 1);
            LOG_TRACE(log, "Created mutation with ID {}", mutation_entry.znode_name);
            break;
        }
        else if (rc == Coordination::Error::ZBADVERSION)
        {
            /// Cannot retry automatically, because some zookeeper ops were lost on the first attempt. Will retry on DDLWorker-level.
            if (query_context->getZooKeeperMetadataTransaction())
                throw Exception("Cannot execute alter, because mutations version was suddenly changed due to concurrent alter",
                                ErrorCodes::CANNOT_ASSIGN_ALTER);
            LOG_TRACE(log, "Version conflict when trying to create a mutation node, retrying...");
            continue;
        }
        else
            throw Coordination::Exception("Unable to create a mutation znode", rc);
    }

    waitMutation(mutation_entry.znode_name, query_context->getSettingsRef().mutations_sync);
}

void StorageReplicatedMergeTree::waitMutation(const String & znode_name, size_t mutations_sync) const
{
    if (!mutations_sync)
        return;

    /// we have to wait
    auto zookeeper = getZooKeeper();
    Strings replicas;
    if (mutations_sync == 2) /// wait for all replicas
    {
        replicas = zookeeper->getChildren(fs::path(zookeeper_path) / "replicas");
        /// This replica should be first, to ensure that the mutation will be loaded into memory
        for (auto it = replicas.begin(); it != replicas.end(); ++it)
        {
            if (*it == replica_name)
            {
                std::iter_swap(it, replicas.rbegin());
                break;
            }
        }
    }
    else if (mutations_sync == 1) /// just wait for ourself
        replicas.push_back(replica_name);

    waitMutationToFinishOnReplicas(replicas, znode_name);
}

std::vector<MergeTreeMutationStatus> StorageReplicatedMergeTree::getMutationsStatus() const
{
    return queue.getMutationsStatus();
}

CancellationCode StorageReplicatedMergeTree::killMutation(const String & mutation_id)
{
    assertNotReadonly();

    zkutil::ZooKeeperPtr zookeeper = getZooKeeper();

    LOG_INFO(log, "Killing mutation {}", mutation_id);

    auto mutation_entry = queue.removeMutation(zookeeper, mutation_id);
    if (!mutation_entry)
        return CancellationCode::NotFound;

    /// After this point no new part mutations will start and part mutations that still exist
    /// in the queue will be skipped.

    /// Cancel already running part mutations.
    for (const auto & pair : mutation_entry->block_numbers)
    {
        const String & partition_id = pair.first;
        Int64 block_number = pair.second;
        getContext()->getMergeList().cancelPartMutations(getStorageID(), partition_id, block_number);
    }
    return CancellationCode::CancelSent;
}

void StorageReplicatedMergeTree::clearOldPartsAndRemoveFromZK()
{
    auto table_lock = lockForShare(
            RWLockImpl::NO_QUERY, getSettings()->lock_acquire_timeout_for_background_operations);
    auto zookeeper = getZooKeeper();

    DataPartsVector parts = grabOldParts();
    if (parts.empty())
        return;

    DataPartsVector parts_to_delete_only_from_filesystem;    // Only duplicates
    DataPartsVector parts_to_delete_completely;              // All parts except duplicates
    DataPartsVector parts_to_retry_deletion;                 // Parts that should be retried due to network problems
    DataPartsVector parts_to_remove_from_filesystem;         // Parts removed from ZK

    for (const auto & part : parts)
    {
        if (!part->is_duplicate)
            parts_to_delete_completely.emplace_back(part);
        else
            parts_to_delete_only_from_filesystem.emplace_back(part);
    }
    parts.clear();

    /// Delete duplicate parts from filesystem
    if (!parts_to_delete_only_from_filesystem.empty())
    {
        clearPartsFromFilesystem(parts_to_delete_only_from_filesystem);
        removePartsFinally(parts_to_delete_only_from_filesystem);

        LOG_DEBUG(log, "Removed {} old duplicate parts", parts_to_delete_only_from_filesystem.size());
    }

    /// Delete normal parts from ZooKeeper
    NameSet part_names_to_retry_deletion;
    try
    {
        Strings part_names_to_delete_completely;
        for (const auto & part : parts_to_delete_completely)
            part_names_to_delete_completely.emplace_back(part->name);

        LOG_DEBUG(log, "Removing {} old parts from ZooKeeper", parts_to_delete_completely.size());
        removePartsFromZooKeeper(zookeeper, part_names_to_delete_completely, &part_names_to_retry_deletion);
    }
    catch (...)
    {
        LOG_ERROR(log, "There is a problem with deleting parts from ZooKeeper: {}", getCurrentExceptionMessage(true));
    }

    /// Part names that were reliably deleted from ZooKeeper should be deleted from filesystem
    auto num_reliably_deleted_parts = parts_to_delete_completely.size() - part_names_to_retry_deletion.size();
    LOG_DEBUG(log, "Removed {} old parts from ZooKeeper. Removing them from filesystem.", num_reliably_deleted_parts);

    /// Delete normal parts on two sets
    for (auto & part : parts_to_delete_completely)
    {
        if (part_names_to_retry_deletion.count(part->name) == 0)
            parts_to_remove_from_filesystem.emplace_back(part);
        else
            parts_to_retry_deletion.emplace_back(part);
    }

    /// Will retry deletion
    if (!parts_to_retry_deletion.empty())
    {
        rollbackDeletingParts(parts_to_retry_deletion);
        LOG_DEBUG(log, "Will retry deletion of {} parts in the next time", parts_to_retry_deletion.size());
    }

    /// Remove parts from filesystem and finally from data_parts
    if (!parts_to_remove_from_filesystem.empty())
    {
        clearPartsFromFilesystem(parts_to_remove_from_filesystem);
        removePartsFinally(parts_to_remove_from_filesystem);

        LOG_DEBUG(log, "Removed {} old parts", parts_to_remove_from_filesystem.size());
    }
}


bool StorageReplicatedMergeTree::tryRemovePartsFromZooKeeperWithRetries(DataPartsVector & parts, size_t max_retries)
{
    Strings part_names_to_remove;
    for (const auto & part : parts)
        part_names_to_remove.emplace_back(part->name);

    return tryRemovePartsFromZooKeeperWithRetries(part_names_to_remove, max_retries);
}

bool StorageReplicatedMergeTree::tryRemovePartsFromZooKeeperWithRetries(const Strings & part_names, size_t max_retries)
{
    size_t num_tries = 0;
    bool success = false;

    while (!success && (max_retries == 0 || num_tries < max_retries))
    {
        try
        {
            ++num_tries;
            success = true;

            auto zookeeper = getZooKeeper();

            std::vector<std::future<Coordination::ExistsResponse>> exists_futures;
            exists_futures.reserve(part_names.size());
            for (const String & part_name : part_names)
            {
                String part_path = fs::path(replica_path) / "parts" / part_name;
                exists_futures.emplace_back(zookeeper->asyncExists(part_path));
            }

            std::vector<std::future<Coordination::MultiResponse>> remove_futures;
            remove_futures.reserve(part_names.size());
            for (size_t i = 0; i < part_names.size(); ++i)
            {
                Coordination::ExistsResponse exists_resp = exists_futures[i].get();
                if (exists_resp.error == Coordination::Error::ZOK)
                {
                    Coordination::Requests ops;
                    removePartFromZooKeeper(part_names[i], ops, exists_resp.stat.numChildren > 0);
                    remove_futures.emplace_back(zookeeper->asyncTryMultiNoThrow(ops));
                }
            }

            for (auto & future : remove_futures)
            {
                auto response = future.get();

                if (response.error == Coordination::Error::ZOK || response.error == Coordination::Error::ZNONODE)
                    continue;

                if (Coordination::isHardwareError(response.error))
                {
                    success = false;
                    continue;
                }

                throw Coordination::Exception(response.error);
            }
        }
        catch (Coordination::Exception & e)
        {
            success = false;

            if (Coordination::isHardwareError(e.code))
                tryLogCurrentException(log, __PRETTY_FUNCTION__);
            else
                throw;
        }

        if (!success && num_tries < max_retries)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    return success;
}

void StorageReplicatedMergeTree::removePartsFromZooKeeper(
    zkutil::ZooKeeperPtr & zookeeper, const Strings & part_names, NameSet * parts_should_be_retried)
{
    std::vector<std::future<Coordination::ExistsResponse>> exists_futures;
    std::vector<std::future<Coordination::MultiResponse>> remove_futures;
    exists_futures.reserve(part_names.size());
    remove_futures.reserve(part_names.size());
    try
    {
        /// Exception can be thrown from loop
        /// if zk session will be dropped
        for (const String & part_name : part_names)
        {
            String part_path = fs::path(replica_path) / "parts" / part_name;
            exists_futures.emplace_back(zookeeper->asyncExists(part_path));
        }

        for (size_t i = 0; i < part_names.size(); ++i)
        {
            Coordination::ExistsResponse exists_resp = exists_futures[i].get();
            if (exists_resp.error == Coordination::Error::ZOK)
            {
                Coordination::Requests ops;
                removePartFromZooKeeper(part_names[i], ops, exists_resp.stat.numChildren > 0);
                remove_futures.emplace_back(zookeeper->asyncTryMultiNoThrow(ops));
            }
            else
            {
                LOG_DEBUG(log, "There is no part {} in ZooKeeper, it was only in filesystem", part_names[i]);
                // emplace invalid future so that the total number of futures is the same as part_names.size();
                remove_futures.emplace_back();
            }
        }
    }
    catch (const Coordination::Exception & e)
    {
        if (parts_should_be_retried && Coordination::isHardwareError(e.code))
            parts_should_be_retried->insert(part_names.begin(), part_names.end());
        throw;
    }

    for (size_t i = 0; i < remove_futures.size(); ++i)
    {
        auto & future = remove_futures[i];

        if (!future.valid())
            continue;

        auto response = future.get();
        if (response.error == Coordination::Error::ZOK)
            continue;
        else if (response.error == Coordination::Error::ZNONODE)
        {
            LOG_DEBUG(log, "There is no part {} in ZooKeeper, it was only in filesystem", part_names[i]);
            continue;
        }
        else if (Coordination::isHardwareError(response.error))
        {
            if (parts_should_be_retried)
                parts_should_be_retried->insert(part_names[i]);
            continue;
        }
        else
            LOG_WARNING(log, "Cannot remove part {} from ZooKeeper: {}", part_names[i], Coordination::errorMessage(response.error));
    }
}


void StorageReplicatedMergeTree::getClearBlocksInPartitionOps(
    Coordination::Requests & ops, zkutil::ZooKeeper & zookeeper, const String & partition_id, Int64 min_block_num, Int64 max_block_num)
{
    Strings blocks;
    if (Coordination::Error::ZOK != zookeeper.tryGetChildren(fs::path(zookeeper_path) / "blocks", blocks))
        throw Exception(zookeeper_path + "/blocks doesn't exist", ErrorCodes::NOT_FOUND_NODE);

    String partition_prefix = partition_id + "_";
    zkutil::AsyncResponses<Coordination::GetResponse> get_futures;

    for (const String & block_id : blocks)
    {
        if (startsWith(block_id, partition_prefix))
        {
            String path = fs::path(zookeeper_path) / "blocks" / block_id;
            get_futures.emplace_back(path, zookeeper.asyncTryGet(path));
        }
    }

    for (auto & pair : get_futures)
    {
        const String & path = pair.first;
        auto result = pair.second.get();

        if (result.error == Coordination::Error::ZNONODE)
            continue;

        ReadBufferFromString buf(result.data);

        const auto part_info = MergeTreePartInfo::tryParsePartName(result.data, format_version);

        if (!part_info || (min_block_num <= part_info->min_block && part_info->max_block <= max_block_num))
            ops.emplace_back(zkutil::makeRemoveRequest(path, -1));
    }
}

void StorageReplicatedMergeTree::clearBlocksInPartition(
    zkutil::ZooKeeper & zookeeper, const String & partition_id, Int64 min_block_num, Int64 max_block_num)
{
    Coordination::Requests delete_requests;
    getClearBlocksInPartitionOps(delete_requests, zookeeper, partition_id, min_block_num, max_block_num);
    Coordination::Responses delete_responses;
    auto code = zookeeper.tryMulti(delete_requests, delete_responses);
    if (code != Coordination::Error::ZOK)
    {
        for (size_t i = 0; i < delete_requests.size(); ++i)
            if (delete_responses[i]->error != Coordination::Error::ZOK)
                LOG_WARNING(log, "Error while deleting ZooKeeper path `{}`: {}, ignoring.", delete_requests[i]->getPath(), Coordination::errorMessage(delete_responses[i]->error));
    }

    LOG_TRACE(log, "Deleted {} deduplication block IDs in partition ID {}", delete_requests.size(), partition_id);
}

void StorageReplicatedMergeTree::replacePartitionFrom(
    const StoragePtr & source_table, const ASTPtr & partition, bool replace, ContextPtr query_context)
{
    /// First argument is true, because we possibly will add new data to current table.
    auto lock1 = lockForShare(query_context->getCurrentQueryId(), query_context->getSettingsRef().lock_acquire_timeout);
    auto lock2 = source_table->lockForShare(query_context->getCurrentQueryId(), query_context->getSettingsRef().lock_acquire_timeout);

    auto source_metadata_snapshot = source_table->getInMemoryMetadataPtr();
    auto metadata_snapshot = getInMemoryMetadataPtr();

    Stopwatch watch;
    MergeTreeData & src_data = checkStructureAndGetMergeTreeData(source_table, source_metadata_snapshot, metadata_snapshot);
    String partition_id = getPartitionIDFromQuery(partition, query_context);

    /// NOTE: Some covered parts may be missing in src_all_parts if corresponding log entries are not executed yet.
    DataPartsVector src_all_parts = src_data.getDataPartsVectorInPartition(MergeTreeDataPartState::Committed, partition_id);
    DataPartsVector src_parts;
    MutableDataPartsVector dst_parts;
    Strings block_id_paths;
    Strings part_checksums;
    std::vector<EphemeralLockInZooKeeper> ephemeral_locks;

    LOG_DEBUG(log, "Cloning {} parts", src_all_parts.size());

    static const String TMP_PREFIX = "tmp_replace_from_";
    auto zookeeper = getZooKeeper();

    String alter_partition_version_path = zookeeper_path + "/alter_partition_version";
    Coordination::Stat alter_partition_version_stat;
    zookeeper->get(alter_partition_version_path, &alter_partition_version_stat);

    /// Firstly, generate last block number and compute drop_range
    /// NOTE: Even if we make ATTACH PARTITION instead of REPLACE PARTITION drop_range will not be empty, it will contain a block.
    /// So, such case has special meaning, if drop_range contains only one block it means that nothing to drop.
    /// TODO why not to add normal DROP_RANGE entry to replication queue if `replace` is true?
    MergeTreePartInfo drop_range;
    std::optional<EphemeralLockInZooKeeper> delimiting_block_lock;
    bool partition_was_empty = !getFakePartCoveringAllPartsInPartition(partition_id, drop_range, delimiting_block_lock, true);
    if (replace && partition_was_empty)
    {
        /// Nothing to drop, will just attach new parts
        LOG_INFO(log, "Partition {} was empty, REPLACE PARTITION will work as ATTACH PARTITION FROM", drop_range.partition_id);
        replace = false;
    }

    if (!replace)
    {
        /// It's ATTACH PARTITION FROM, not REPLACE PARTITION. We have to reset drop range
        drop_range = makeDummyDropRangeForMovePartitionOrAttachPartitionFrom(partition_id);
    }

    assert(replace == !LogEntry::ReplaceRangeEntry::isMovePartitionOrAttachFrom(drop_range));

    String drop_range_fake_part_name = getPartNamePossiblyFake(format_version, drop_range);

    for (const auto & src_part : src_all_parts)
    {
        /// We also make some kind of deduplication to avoid duplicated parts in case of ATTACH PARTITION
        /// Assume that merges in the partition are quite rare
        /// Save deduplication block ids with special prefix replace_partition

        if (!canReplacePartition(src_part))
            throw Exception(
                "Cannot replace partition '" + partition_id + "' because part '" + src_part->name + "' has inconsistent granularity with table",
                ErrorCodes::LOGICAL_ERROR);

        String hash_hex = src_part->checksums.getTotalChecksumHex();

        if (replace)
            LOG_INFO(log, "Trying to replace {} with hash_hex {}", src_part->name, hash_hex);
        else
            LOG_INFO(log, "Trying to attach {} with hash_hex {}", src_part->name, hash_hex);

        String block_id_path = replace ? "" : (fs::path(zookeeper_path) / "blocks" / (partition_id + "_replace_from_" + hash_hex));

        auto lock = allocateBlockNumber(partition_id, zookeeper, block_id_path);
        if (!lock)
        {
            LOG_INFO(log, "Part {} (hash {}) has been already attached", src_part->name, hash_hex);
            continue;
        }

        UInt64 index = lock->getNumber();
        MergeTreePartInfo dst_part_info(partition_id, index, index, src_part->info.level);
        auto dst_part = cloneAndLoadDataPartOnSameDisk(src_part, TMP_PREFIX, dst_part_info, metadata_snapshot);

        src_parts.emplace_back(src_part);
        dst_parts.emplace_back(dst_part);
        ephemeral_locks.emplace_back(std::move(*lock));
        block_id_paths.emplace_back(block_id_path);
        part_checksums.emplace_back(hash_hex);
    }

    ReplicatedMergeTreeLogEntryData entry;
    {
        auto src_table_id = src_data.getStorageID();
        entry.type = ReplicatedMergeTreeLogEntryData::REPLACE_RANGE;
        entry.source_replica = replica_name;
        entry.create_time = time(nullptr);
        entry.replace_range_entry = std::make_shared<ReplicatedMergeTreeLogEntryData::ReplaceRangeEntry>();

        auto & entry_replace = *entry.replace_range_entry;
        entry_replace.drop_range_part_name = drop_range_fake_part_name;
        entry_replace.from_database = src_table_id.database_name;
        entry_replace.from_table = src_table_id.table_name;
        for (const auto & part : src_parts)
            entry_replace.src_part_names.emplace_back(part->name);
        for (const auto & part : dst_parts)
            entry_replace.new_part_names.emplace_back(part->name);
        for (const String & checksum : part_checksums)
            entry_replace.part_names_checksums.emplace_back(checksum);
        entry_replace.columns_version = -1;
    }

    /// Remove deduplication block_ids of replacing parts
    if (replace)
        clearBlocksInPartition(*zookeeper, drop_range.partition_id, drop_range.max_block, drop_range.max_block);

    DataPartsVector parts_to_remove;
    Coordination::Responses op_results;

    try
    {
        Coordination::Requests ops;
        for (size_t i = 0; i < dst_parts.size(); ++i)
        {
            getCommitPartOps(ops, dst_parts[i], block_id_paths[i]);
            ephemeral_locks[i].getUnlockOps(ops);

            if (ops.size() > zkutil::MULTI_BATCH_SIZE)
            {
                /// It is unnecessary to add parts to working set until we commit log entry
                zookeeper->multi(ops);
                ops.clear();
            }
        }

        if (auto txn = query_context->getZooKeeperMetadataTransaction())
            txn->moveOpsTo(ops);

        delimiting_block_lock->getUnlockOps(ops);
        /// Check and update version to avoid race with DROP_RANGE
        ops.emplace_back(zkutil::makeSetRequest(alter_partition_version_path, "", alter_partition_version_stat.version));
        /// Just update version, because merges assignment relies on it
        ops.emplace_back(zkutil::makeSetRequest(fs::path(zookeeper_path) / "log", "", -1));
        ops.emplace_back(zkutil::makeCreateRequest(fs::path(zookeeper_path) / "log/log-", entry.toString(), zkutil::CreateMode::PersistentSequential));

        Transaction transaction(*this);
        {
            auto data_parts_lock = lockParts();

            for (MutableDataPartPtr & part : dst_parts)
                renameTempPartAndReplace(part, nullptr, &transaction, data_parts_lock);
        }

        Coordination::Error code = zookeeper->tryMulti(ops, op_results);
        if (code == Coordination::Error::ZOK)
            delimiting_block_lock->assumeUnlocked();
        else if (code == Coordination::Error::ZBADVERSION)
            throw Exception(ErrorCodes::CANNOT_ASSIGN_ALTER, "Cannot assign ALTER PARTITION, because another ALTER PARTITION query was concurrently executed");
        else
            zkutil::KeeperMultiException::check(code, ops, op_results);

        {
            auto data_parts_lock = lockParts();

            transaction.commit(&data_parts_lock);
            if (replace)
                parts_to_remove = removePartsInRangeFromWorkingSet(drop_range, true, data_parts_lock);
        }

        PartLog::addNewParts(getContext(), dst_parts, watch.elapsed());
    }
    catch (...)
    {
        PartLog::addNewParts(getContext(), dst_parts, watch.elapsed(), ExecutionStatus::fromCurrentException());
        throw;
    }

    String log_znode_path = dynamic_cast<const Coordination::CreateResponse &>(*op_results.back()).path_created;
    entry.znode_name = log_znode_path.substr(log_znode_path.find_last_of('/') + 1);

    for (auto & lock : ephemeral_locks)
        lock.assumeUnlocked();

    /// Forcibly remove replaced parts from ZooKeeper
    tryRemovePartsFromZooKeeperWithRetries(parts_to_remove);

    /// Speedup removing of replaced parts from filesystem
    parts_to_remove.clear();
    cleanup_thread.wakeup();

    lock2.reset();
    lock1.reset();

    waitForLogEntryToBeProcessedIfNecessary(entry, query_context);
}

void StorageReplicatedMergeTree::movePartitionToTable(const StoragePtr & dest_table, const ASTPtr & partition, ContextPtr query_context)
{
    auto lock1 = lockForShare(query_context->getCurrentQueryId(), query_context->getSettingsRef().lock_acquire_timeout);
    auto lock2 = dest_table->lockForShare(query_context->getCurrentQueryId(), query_context->getSettingsRef().lock_acquire_timeout);

    auto dest_table_storage = std::dynamic_pointer_cast<StorageReplicatedMergeTree>(dest_table);
    if (!dest_table_storage)
        throw Exception("Table " + getStorageID().getNameForLogs() + " supports movePartitionToTable only for ReplicatedMergeTree family of table engines."
                        " Got " + dest_table->getName(), ErrorCodes::NOT_IMPLEMENTED);
    if (dest_table_storage->getStoragePolicy() != this->getStoragePolicy())
        throw Exception("Destination table " + dest_table_storage->getStorageID().getNameForLogs() +
                        " should have the same storage policy of source table " + getStorageID().getNameForLogs() + ". " +
                        getStorageID().getNameForLogs() + ": " + this->getStoragePolicy()->getName() + ", " +
                        getStorageID().getNameForLogs() + ": " + dest_table_storage->getStoragePolicy()->getName(), ErrorCodes::UNKNOWN_POLICY);

    auto dest_metadata_snapshot = dest_table->getInMemoryMetadataPtr();
    auto metadata_snapshot = getInMemoryMetadataPtr();

    Stopwatch watch;
    MergeTreeData & src_data = dest_table_storage->checkStructureAndGetMergeTreeData(*this, metadata_snapshot, dest_metadata_snapshot);
    auto src_data_id = src_data.getStorageID();
    String partition_id = getPartitionIDFromQuery(partition, query_context);

    /// A range for log entry to remove parts from the source table (myself).
    auto zookeeper = getZooKeeper();
    String alter_partition_version_path = zookeeper_path + "/alter_partition_version";
    Coordination::Stat alter_partition_version_stat;
    zookeeper->get(alter_partition_version_path, &alter_partition_version_stat);

    MergeTreePartInfo drop_range;
    std::optional<EphemeralLockInZooKeeper> delimiting_block_lock;
    getFakePartCoveringAllPartsInPartition(partition_id, drop_range, delimiting_block_lock, true);
    String drop_range_fake_part_name = getPartNamePossiblyFake(format_version, drop_range);

    DataPartPtr covering_part;
    DataPartsVector src_all_parts;
    {
        /// NOTE: Some covered parts may be missing in src_all_parts if corresponding log entries are not executed yet.
        auto parts_lock = src_data.lockParts();
        src_all_parts = src_data.getActivePartsToReplace(drop_range, drop_range_fake_part_name, covering_part, parts_lock);
    }

    if (covering_part)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Got part {} covering drop range {}, it's a bug",
                        covering_part->name, drop_range_fake_part_name);

    /// After allocating block number for drop_range we must ensure that it does not intersect block numbers
    /// allocated by concurrent REPLACE query.
    /// We could check it in multi-request atomically with creation of DROP_RANGE entry in source table log,
    /// but it's better to check it here and fail as early as possible (before we have done something to destination table).
    Coordination::Error version_check_code = zookeeper->trySet(alter_partition_version_path, "", alter_partition_version_stat.version);
    if (version_check_code != Coordination::Error::ZOK)
        throw Exception(ErrorCodes::CANNOT_ASSIGN_ALTER, "Cannot DROP PARTITION in {} after copying partition to {}, "
                        "because another ALTER PARTITION query was concurrently executed",
                        getStorageID().getFullTableName(), dest_table_storage->getStorageID().getFullTableName());

    DataPartsVector src_parts;
    MutableDataPartsVector dst_parts;
    Strings block_id_paths;
    Strings part_checksums;
    std::vector<EphemeralLockInZooKeeper> ephemeral_locks;

    LOG_DEBUG(log, "Cloning {} parts", src_all_parts.size());

    static const String TMP_PREFIX = "tmp_move_from_";

    /// Clone parts into destination table.
    String dest_alter_partition_version_path = dest_table_storage->zookeeper_path + "/alter_partition_version";
    Coordination::Stat dest_alter_partition_version_stat;
    zookeeper->get(dest_alter_partition_version_path, &dest_alter_partition_version_stat);
    for (const auto & src_part : src_all_parts)
    {
        if (!dest_table_storage->canReplacePartition(src_part))
            throw Exception(
                "Cannot move partition '" + partition_id + "' because part '" + src_part->name + "' has inconsistent granularity with table",
                ErrorCodes::LOGICAL_ERROR);

        String hash_hex = src_part->checksums.getTotalChecksumHex();
        String block_id_path;

        auto lock = dest_table_storage->allocateBlockNumber(partition_id, zookeeper, block_id_path);
        if (!lock)
        {
            LOG_INFO(log, "Part {} (hash {}) has been already attached", src_part->name, hash_hex);
            continue;
        }

        UInt64 index = lock->getNumber();
        MergeTreePartInfo dst_part_info(partition_id, index, index, src_part->info.level);
        auto dst_part = dest_table_storage->cloneAndLoadDataPartOnSameDisk(src_part, TMP_PREFIX, dst_part_info, dest_metadata_snapshot);

        src_parts.emplace_back(src_part);
        dst_parts.emplace_back(dst_part);
        ephemeral_locks.emplace_back(std::move(*lock));
        block_id_paths.emplace_back(block_id_path);
        part_checksums.emplace_back(hash_hex);
    }

    ReplicatedMergeTreeLogEntryData entry_delete;
    {
        entry_delete.type = LogEntry::DROP_RANGE;
        entry_delete.source_replica = replica_name;
        entry_delete.new_part_name = drop_range_fake_part_name;
        entry_delete.detach = false; //-V1048
        entry_delete.create_time = time(nullptr);
    }

    ReplicatedMergeTreeLogEntryData entry;
    {
        MergeTreePartInfo drop_range_dest = makeDummyDropRangeForMovePartitionOrAttachPartitionFrom(partition_id);

        entry.type = ReplicatedMergeTreeLogEntryData::REPLACE_RANGE;
        entry.source_replica = dest_table_storage->replica_name;
        entry.create_time = time(nullptr);
        entry.replace_range_entry = std::make_shared<ReplicatedMergeTreeLogEntryData::ReplaceRangeEntry>();

        auto & entry_replace = *entry.replace_range_entry;
        entry_replace.drop_range_part_name = getPartNamePossiblyFake(format_version, drop_range_dest);
        entry_replace.from_database = src_data_id.database_name;
        entry_replace.from_table = src_data_id.table_name;
        for (const auto & part : src_parts)
            entry_replace.src_part_names.emplace_back(part->name);
        for (const auto & part : dst_parts)
            entry_replace.new_part_names.emplace_back(part->name);
        for (const String & checksum : part_checksums)
            entry_replace.part_names_checksums.emplace_back(checksum);
        entry_replace.columns_version = -1;
    }

    clearBlocksInPartition(*zookeeper, drop_range.partition_id, drop_range.max_block, drop_range.max_block);

    DataPartsVector parts_to_remove;
    Coordination::Responses op_results;

    try
    {
        Coordination::Requests ops;
        for (size_t i = 0; i < dst_parts.size(); ++i)
        {
            dest_table_storage->getCommitPartOps(ops, dst_parts[i], block_id_paths[i]);
            ephemeral_locks[i].getUnlockOps(ops);

            if (ops.size() > zkutil::MULTI_BATCH_SIZE)
            {
                zookeeper->multi(ops);
                ops.clear();
            }
        }

        /// Check and update version to avoid race with DROP_RANGE
        ops.emplace_back(zkutil::makeSetRequest(dest_alter_partition_version_path, "", dest_alter_partition_version_stat.version));
        /// Just update version, because merges assignment relies on it
        ops.emplace_back(zkutil::makeSetRequest(fs::path(dest_table_storage->zookeeper_path) / "log", "", -1));
        ops.emplace_back(zkutil::makeCreateRequest(fs::path(dest_table_storage->zookeeper_path) / "log/log-",
                                                   entry.toString(), zkutil::CreateMode::PersistentSequential));

        {
            Transaction transaction(*dest_table_storage);

            auto src_data_parts_lock = lockParts();
            auto dest_data_parts_lock = dest_table_storage->lockParts();

            std::mutex mutex;
            DataPartsLock lock(mutex);

            for (MutableDataPartPtr & part : dst_parts)
                dest_table_storage->renameTempPartAndReplace(part, nullptr, &transaction, lock);

            Coordination::Error code = zookeeper->tryMulti(ops, op_results);
            if (code == Coordination::Error::ZBADVERSION)
                throw Exception(ErrorCodes::CANNOT_ASSIGN_ALTER, "Cannot assign ALTER PARTITION, because another ALTER PARTITION query was concurrently executed");
            else
                zkutil::KeeperMultiException::check(code, ops, op_results);

            parts_to_remove = removePartsInRangeFromWorkingSet(drop_range, true, lock);
            transaction.commit(&lock);
        }

        PartLog::addNewParts(getContext(), dst_parts, watch.elapsed());
    }
    catch (...)
    {
        PartLog::addNewParts(getContext(), dst_parts, watch.elapsed(), ExecutionStatus::fromCurrentException());
        throw;
    }

    String log_znode_path = dynamic_cast<const Coordination::CreateResponse &>(*op_results.back()).path_created;
    entry.znode_name = log_znode_path.substr(log_znode_path.find_last_of('/') + 1);

    for (auto & lock : ephemeral_locks)
        lock.assumeUnlocked();

    tryRemovePartsFromZooKeeperWithRetries(parts_to_remove);

    parts_to_remove.clear();
    cleanup_thread.wakeup();
    lock2.reset();

    dest_table_storage->waitForLogEntryToBeProcessedIfNecessary(entry, query_context);

    /// Create DROP_RANGE for the source table
    Coordination::Requests ops_src;
    ops_src.emplace_back(zkutil::makeCreateRequest(
        fs::path(zookeeper_path) / "log/log-", entry_delete.toString(), zkutil::CreateMode::PersistentSequential));
    /// Just update version, because merges assignment relies on it
    ops_src.emplace_back(zkutil::makeSetRequest(fs::path(zookeeper_path) / "log", "", -1));
    delimiting_block_lock->getUnlockOps(ops_src);

    op_results = zookeeper->multi(ops_src);

    log_znode_path = dynamic_cast<const Coordination::CreateResponse &>(*op_results.front()).path_created;
    entry_delete.znode_name = log_znode_path.substr(log_znode_path.find_last_of('/') + 1);

    lock1.reset();
    waitForLogEntryToBeProcessedIfNecessary(entry_delete, query_context);

    /// Cleaning possibly stored information about parts from /quorum/last_part node in ZooKeeper.
    cleanLastPartNode(partition_id);
}

void StorageReplicatedMergeTree::movePartitionToShard(
    const ASTPtr & partition, bool move_part, const String & to, ContextPtr /*query_context*/)
{
    /// This is a lightweight operation that only optimistically checks if it could succeed and queues tasks.

    if (!move_part)
        throw Exception("MOVE PARTITION TO SHARD is not supported, use MOVE PART instead", ErrorCodes::NOT_IMPLEMENTED);

    if (normalizeZooKeeperPath(zookeeper_path) == normalizeZooKeeperPath(to))
        throw Exception("Source and destination are the same", ErrorCodes::BAD_ARGUMENTS);

    auto zookeeper = getZooKeeper();

    String part_name = partition->as<ASTLiteral &>().value.safeGet<String>();
    auto part_info = MergeTreePartInfo::fromPartName(part_name, format_version);

    auto part = getPartIfExists(part_info, {MergeTreeDataPartState::Committed});
    if (!part)
        throw Exception(ErrorCodes::NO_SUCH_DATA_PART, "Part {} not found locally", part_name);

    if (part->uuid == UUIDHelpers::Nil)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Part {} does not have an uuid assigned and it can't be moved between shards", part_name);


    ReplicatedMergeTreeMergePredicate merge_pred = queue.getMergePredicate(zookeeper);

    /// The following block is pretty much copy & paste from StorageReplicatedMergeTree::dropPart to avoid conflicts while this is WIP.
    /// Extract it to a common method and re-use it before merging.
    {
        if (partIsLastQuorumPart(part->info))
        {
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Part {} is last inserted part with quorum in partition. Would not be able to drop", part_name);
        }

        /// canMergeSinglePart is overlapping with dropPart, let's try to use the same code.
        String out_reason;
        if (!merge_pred.canMergeSinglePart(part, &out_reason))
            throw Exception(ErrorCodes::PART_IS_TEMPORARILY_LOCKED, "Part is busy, reason: " + out_reason);
    }

    {
        /// Optimistic check that for compatible destination table structure.
        checkTableStructure(to, getInMemoryMetadataPtr());
    }

    PinnedPartUUIDs src_pins;
    PinnedPartUUIDs dst_pins;

    {
        String s = zookeeper->get(zookeeper_path + "/pinned_part_uuids", &src_pins.stat);
        src_pins.fromString(s);
    }

    {
        String s = zookeeper->get(to + "/pinned_part_uuids", &dst_pins.stat);
        dst_pins.fromString(s);
    }

    if (src_pins.part_uuids.contains(part->uuid) || dst_pins.part_uuids.contains(part->uuid))
        throw Exception(ErrorCodes::PART_IS_TEMPORARILY_LOCKED, "Part {} has it's uuid ({}) already pinned.", part_name, toString(part->uuid));

    src_pins.part_uuids.insert(part->uuid);
    dst_pins.part_uuids.insert(part->uuid);

    PartMovesBetweenShardsOrchestrator::Entry part_move_entry;
    part_move_entry.create_time = std::time(nullptr);
    part_move_entry.update_time = part_move_entry.create_time;
    part_move_entry.task_uuid = UUIDHelpers::generateV4();
    part_move_entry.part_name = part->name;
    part_move_entry.part_uuid = part->uuid;
    part_move_entry.to_shard = to;

    Coordination::Requests ops;
    ops.emplace_back(zkutil::makeCheckRequest(zookeeper_path + "/log", merge_pred.getVersion())); /// Make sure no new events were added to the log.
    ops.emplace_back(zkutil::makeSetRequest(zookeeper_path + "/pinned_part_uuids", src_pins.toString(), src_pins.stat.version));
    ops.emplace_back(zkutil::makeSetRequest(to + "/pinned_part_uuids", dst_pins.toString(), dst_pins.stat.version));
    ops.emplace_back(zkutil::makeCreateRequest(
        part_moves_between_shards_orchestrator.entries_znode_path + "/task-",
        part_move_entry.toString(),
        zkutil::CreateMode::PersistentSequential));

    Coordination::Responses responses;
    Coordination::Error rc = zookeeper->tryMulti(ops, responses);
    zkutil::KeeperMultiException::check(rc, ops, responses);

    String task_znode_path = dynamic_cast<const Coordination::CreateResponse &>(*responses.back()).path_created;
    LOG_DEBUG(log, "Created task for part movement between shards at " + task_znode_path);

    /// Force refresh local state. This will make the task immediately visible in `system.part_moves_between_shards` table.
    part_moves_between_shards_orchestrator.fetchStateFromZK();

    // TODO: Add support for `replication_alter_partitions_sync`.
}

void StorageReplicatedMergeTree::getCommitPartOps(
    Coordination::Requests & ops,
    MutableDataPartPtr & part,
    const String & block_id_path) const
{
    const String & part_name = part->name;
    const auto storage_settings_ptr = getSettings();

    if (!block_id_path.empty())
    {
        /// Make final duplicate check and commit block_id
        ops.emplace_back(
            zkutil::makeCreateRequest(
                block_id_path,
                part_name,  /// We will be able to know original part number for duplicate blocks, if we want.
                zkutil::CreateMode::Persistent));
    }

    /// Information about the part, in the replica
    if (storage_settings_ptr->use_minimalistic_part_header_in_zookeeper)
    {
        ops.emplace_back(zkutil::makeCreateRequest(
            fs::path(replica_path) / "parts" / part->name,
            ReplicatedMergeTreePartHeader::fromColumnsAndChecksums(part->getColumns(), part->checksums).toString(),
            zkutil::CreateMode::Persistent));
    }
    else
    {
        ops.emplace_back(zkutil::makeCreateRequest(
            fs::path(replica_path) / "parts" / part->name,
            "",
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(
            fs::path(replica_path) / "parts" / part->name / "columns",
            part->getColumns().toString(),
            zkutil::CreateMode::Persistent));
        ops.emplace_back(zkutil::makeCreateRequest(
            fs::path(replica_path) / "parts" / part->name / "checksums",
            getChecksumsForZooKeeper(part->checksums),
            zkutil::CreateMode::Persistent));
    }
}

ReplicatedMergeTreeAddress StorageReplicatedMergeTree::getReplicatedMergeTreeAddress() const
{
    auto host_port = getContext()->getInterserverIOAddress();
    auto table_id = getStorageID();

    ReplicatedMergeTreeAddress res;
    res.host = host_port.first;
    res.replication_port = host_port.second;
    res.queries_port = getContext()->getTCPPort();
    res.database = table_id.database_name;
    res.table = table_id.table_name;
    res.scheme = getContext()->getInterserverScheme();
    return res;
}

ActionLock StorageReplicatedMergeTree::getActionLock(StorageActionBlockType action_type)
{
    if (action_type == ActionLocks::PartsMerge)
        return merger_mutator.merges_blocker.cancel();

    if (action_type == ActionLocks::PartsTTLMerge)
        return merger_mutator.ttl_merges_blocker.cancel();

    if (action_type == ActionLocks::PartsFetch)
        return fetcher.blocker.cancel();

    if (action_type == ActionLocks::PartsSend)
    {
        auto data_parts_exchange_ptr = std::atomic_load(&data_parts_exchange_endpoint);
        return data_parts_exchange_ptr ? data_parts_exchange_ptr->blocker.cancel() : ActionLock();
    }

    if (action_type == ActionLocks::ReplicationQueue)
        return queue.actions_blocker.cancel();

    if (action_type == ActionLocks::PartsMove)
        return parts_mover.moves_blocker.cancel();

    return {};
}

void StorageReplicatedMergeTree::onActionLockRemove(StorageActionBlockType action_type)
{
    if (action_type == ActionLocks::PartsMerge || action_type == ActionLocks::PartsTTLMerge
        || action_type == ActionLocks::PartsFetch || action_type == ActionLocks::PartsSend
        || action_type == ActionLocks::ReplicationQueue)
        background_operations_assignee.trigger();
    else if (action_type == ActionLocks::PartsMove)
        background_moves_assignee.trigger();
}

bool StorageReplicatedMergeTree::waitForShrinkingQueueSize(size_t queue_size, UInt64 max_wait_milliseconds)
{
    Stopwatch watch;

    /// Let's fetch new log entries firstly
    queue.pullLogsToQueue(getZooKeeper(), {}, ReplicatedMergeTreeQueue::SYNC);

    /// This is significant, because the execution of this task could be delayed at BackgroundPool.
    /// And we force it to be executed.
    background_operations_assignee.trigger();

    Poco::Event target_size_event;
    auto callback = [&target_size_event, queue_size] (size_t new_queue_size)
    {
        if (new_queue_size <= queue_size)
            target_size_event.set();
    };
    const auto handler = queue.addSubscriber(std::move(callback));

    while (!target_size_event.tryWait(50))
    {
        if (max_wait_milliseconds && watch.elapsedMilliseconds() > max_wait_milliseconds)
            return false;

        if (partial_shutdown_called)
            throw Exception("Shutdown is called for table", ErrorCodes::ABORTED);
    }

    return true;
}

bool StorageReplicatedMergeTree::dropPartImpl(
    zkutil::ZooKeeperPtr & zookeeper, String part_name, LogEntry & entry, bool detach, bool throw_if_noop)
{
    LOG_TRACE(log, "Will try to insert a log entry to DROP_RANGE for part: " + part_name);

    auto part_info = MergeTreePartInfo::fromPartName(part_name, format_version);

    while (true)
    {
        ReplicatedMergeTreeMergePredicate merge_pred = queue.getMergePredicate(zookeeper);

        auto part = getPartIfExists(part_info, {MergeTreeDataPartState::Committed});

        if (!part)
        {
            if (throw_if_noop)
                throw Exception("Part " + part_name + " not found locally, won't try to drop it.", ErrorCodes::NO_SUCH_DATA_PART);
            return false;
        }

        if (merge_pred.hasDropRange(part->info))
        {
            if (throw_if_noop)
                throw Exception("Already has DROP RANGE for part " + part_name + " in queue.", ErrorCodes::PART_IS_TEMPORARILY_LOCKED);

            return false;
        }

        /// There isn't a lot we can do otherwise. Can't cancel merges because it is possible that a replica already
        /// finished the merge.
        if (partIsAssignedToBackgroundOperation(part))
        {
            if (throw_if_noop)
                throw Exception("Part " + part_name
                                + " is currently participating in a background operation (mutation/merge)"
                                + ", try again later", ErrorCodes::PART_IS_TEMPORARILY_LOCKED);
            return false;
        }

        if (partIsLastQuorumPart(part->info))
        {
            if (throw_if_noop)
                throw Exception("Part " + part_name + " is last inserted part with quorum in partition. Cannot drop",
                                ErrorCodes::NOT_IMPLEMENTED);
            return false;
        }

        if (partIsInsertingWithParallelQuorum(part->info))
        {
            if (throw_if_noop)
                throw Exception("Part " + part_name + " is inserting with parallel quorum. Cannot drop",
                                ErrorCodes::NOT_IMPLEMENTED);
            return false;
        }

        Coordination::Requests ops;
        getClearBlocksInPartitionOps(ops, *zookeeper, part_info.partition_id, part_info.min_block, part_info.max_block);
        size_t clear_block_ops_size = ops.size();

        /// If `part_name` is result of a recent merge and source parts are still available then
        /// DROP_RANGE with detach will move this part together with source parts to `detached/` dir.
        entry.type = LogEntry::DROP_RANGE;
        entry.source_replica = replica_name;
        /// We don't set fake drop level (999999999) for the single part DROP_RANGE.
        /// First of all we don't guarantee anything other than the part will not be
        /// active after DROP PART, but covering part (without data of dropped part) can exist.
        /// If we add part with 9999999 level than we can break invariant in virtual_parts of
        /// the queue.
        entry.new_part_name = getPartNamePossiblyFake(format_version, part->info);
        entry.detach = detach;
        entry.create_time = time(nullptr);

        ops.emplace_back(zkutil::makeCheckRequest(fs::path(zookeeper_path) / "log", merge_pred.getVersion())); /// Make sure no new events were added to the log.
        ops.emplace_back(zkutil::makeCreateRequest(fs::path(zookeeper_path) / "log/log-", entry.toString(), zkutil::CreateMode::PersistentSequential));
        /// Just update version, because merges assignment relies on it
        ops.emplace_back(zkutil::makeSetRequest(fs::path(zookeeper_path) / "log", "", -1));
        Coordination::Responses responses;
        Coordination::Error rc = zookeeper->tryMulti(ops, responses);

        if (rc == Coordination::Error::ZBADVERSION)
        {
            LOG_TRACE(log, "A new log entry appeared while trying to commit DROP RANGE. Retry.");
            continue;
        }
        else if (rc == Coordination::Error::ZNONODE)
        {
            LOG_TRACE(log, "Other replica already removing same part {} or part deduplication node was removed by background thread. Retry.", part_name);
            continue;
        }
        else
            zkutil::KeeperMultiException::check(rc, ops, responses);

        String log_znode_path = dynamic_cast<const Coordination::CreateResponse &>(*responses[clear_block_ops_size + 1]).path_created;
        entry.znode_name = log_znode_path.substr(log_znode_path.find_last_of('/') + 1);

        return true;
    }
}

bool StorageReplicatedMergeTree::dropAllPartsInPartition(
    zkutil::ZooKeeper & zookeeper, String & partition_id, LogEntry & entry, ContextPtr query_context, bool detach)
{
    String alter_partition_version_path = zookeeper_path + "/alter_partition_version";
    Coordination::Stat alter_partition_version_stat;
    zookeeper.get(alter_partition_version_path, &alter_partition_version_stat);

    MergeTreePartInfo drop_range_info;

    /// It would prevent other replicas from assigning merges which intersect locked block number.
    std::optional<EphemeralLockInZooKeeper> delimiting_block_lock;

    if (!getFakePartCoveringAllPartsInPartition(partition_id, drop_range_info, delimiting_block_lock))
    {
        LOG_INFO(log, "Will not drop partition {}, it is empty.", partition_id);
        return false;
    }

    clearBlocksInPartition(zookeeper, partition_id, drop_range_info.min_block, drop_range_info.max_block);

    String drop_range_fake_part_name = getPartNamePossiblyFake(format_version, drop_range_info);

    LOG_DEBUG(log, "Disabled merges covered by range {}", drop_range_fake_part_name);

    /// Finally, having achieved the necessary invariants, you can put an entry in the log.
    entry.type = LogEntry::DROP_RANGE;
    entry.source_replica = replica_name;
    entry.new_part_name = drop_range_fake_part_name;
    entry.detach = detach;
    entry.create_time = time(nullptr);

    Coordination::Requests ops;

    ops.emplace_back(zkutil::makeCreateRequest(fs::path(zookeeper_path) / "log/log-", entry.toString(),
        zkutil::CreateMode::PersistentSequential));

    /// Check and update version to avoid race with REPLACE_RANGE.
    /// Otherwise new parts covered by drop_range_info may appear after execution of current DROP_RANGE entry
    /// as a result of execution of concurrently created REPLACE_RANGE entry.
    ops.emplace_back(zkutil::makeSetRequest(alter_partition_version_path, "", alter_partition_version_stat.version));

    /// Just update version, because merges assignment relies on it
    ops.emplace_back(zkutil::makeSetRequest(fs::path(zookeeper_path) / "log", "", -1));
    delimiting_block_lock->getUnlockOps(ops);

    if (auto txn = query_context->getZooKeeperMetadataTransaction())
        txn->moveOpsTo(ops);

    Coordination::Responses responses;
    Coordination::Error code = zookeeper.tryMulti(ops, responses);

    if (code == Coordination::Error::ZOK)
        delimiting_block_lock->assumeUnlocked();
    else if (code == Coordination::Error::ZBADVERSION)
        throw Exception(ErrorCodes::CANNOT_ASSIGN_ALTER,
            "Cannot assign ALTER PARTITION because another ALTER PARTITION query was concurrently executed");
    else
        zkutil::KeeperMultiException::check(code, ops, responses);

    String log_znode_path = dynamic_cast<const Coordination::CreateResponse &>(*responses.front()).path_created;
    entry.znode_name = log_znode_path.substr(log_znode_path.find_last_of('/') + 1);

    getContext()->getMergeList().cancelInPartition(getStorageID(), partition_id, drop_range_info.max_block);

    return true;
}


CheckResults StorageReplicatedMergeTree::checkData(const ASTPtr & query, ContextPtr local_context)
{
    CheckResults results;
    DataPartsVector data_parts;
    if (const auto & check_query = query->as<ASTCheckQuery &>(); check_query.partition)
    {
        String partition_id = getPartitionIDFromQuery(check_query.partition, local_context);
        data_parts = getDataPartsVectorInPartition(MergeTreeDataPartState::Committed, partition_id);
    }
    else
        data_parts = getDataPartsVector();

    for (auto & part : data_parts)
    {
        try
        {
            results.push_back(part_check_thread.checkPart(part->name));
        }
        catch (const Exception & ex)
        {
            results.emplace_back(part->name, false, "Check of part finished with error: '" + ex.message() + "'");
        }
    }
    return results;
}


bool StorageReplicatedMergeTree::canUseAdaptiveGranularity() const
{
    const auto storage_settings_ptr = getSettings();
    return storage_settings_ptr->index_granularity_bytes != 0 &&
        (storage_settings_ptr->enable_mixed_granularity_parts ||
            (!has_non_adaptive_index_granularity_parts && !other_replicas_fixed_granularity));
}


MutationCommands StorageReplicatedMergeTree::getFirstAlterMutationCommandsForPart(const DataPartPtr & part) const
{
    return queue.getFirstAlterMutationCommandsForPart(part);
}


void StorageReplicatedMergeTree::startBackgroundMovesIfNeeded()
{
    if (areBackgroundMovesNeeded())
        background_moves_assignee.start();
}

std::unique_ptr<MergeTreeSettings> StorageReplicatedMergeTree::getDefaultSettings() const
{
    return std::make_unique<MergeTreeSettings>(getContext()->getReplicatedMergeTreeSettings());
}


void StorageReplicatedMergeTree::lockSharedData(const IMergeTreeDataPart & part) const
{
    if (!part.volume)
        return;
    DiskPtr disk = part.volume->getDisk();
    if (!disk || !disk->supportZeroCopyReplication())
        return;
    String zero_copy = fmt::format("zero_copy_{}", toString(disk->getType()));

    zkutil::ZooKeeperPtr zookeeper = tryGetZooKeeper();
    if (!zookeeper)
        return;

    String id = part.getUniqueId();
    boost::replace_all(id, "/", "_");

    String zookeeper_node = fs::path(zookeeper_path) / zero_copy / "shared" / part.name / id / replica_name;

    LOG_TRACE(log, "Set zookeeper lock {}", zookeeper_node);

    /// In rare case other replica can remove path between createAncestors and createIfNotExists
    /// So we make up to 5 attempts
    for (int attempts = 5; attempts > 0; --attempts)
    {
        try
        {
            zookeeper->createAncestors(zookeeper_node);
            zookeeper->createIfNotExists(zookeeper_node, "lock");
            break;
        }
        catch (const zkutil::KeeperException & e)
        {
            if (e.code == Coordination::Error::ZNONODE)
                continue;
            throw;
        }
    }
}


bool StorageReplicatedMergeTree::unlockSharedData(const IMergeTreeDataPart & part) const
{
    if (!part.volume)
        return true;
    DiskPtr disk = part.volume->getDisk();
    if (!disk || !disk->supportZeroCopyReplication())
        return true;
    String zero_copy = fmt::format("zero_copy_{}", toString(disk->getType()));

    zkutil::ZooKeeperPtr zookeeper = tryGetZooKeeper();
    if (!zookeeper)
        return true;

    String id = part.getUniqueId();
    boost::replace_all(id, "/", "_");

    String zookeeper_part_node = fs::path(zookeeper_path) / zero_copy / "shared" / part.name;
    String zookeeper_part_uniq_node = fs::path(zookeeper_part_node) / id;
    String zookeeper_node = fs::path(zookeeper_part_uniq_node) / replica_name;

    LOG_TRACE(log, "Remove zookeeper lock {}", zookeeper_node);

    zookeeper->tryRemove(zookeeper_node);

    Strings children;
    zookeeper->tryGetChildren(zookeeper_part_uniq_node, children);

    if (!children.empty())
    {
        LOG_TRACE(log, "Found zookeper locks for {}", zookeeper_part_uniq_node);
        return false;
    }

    zookeeper->tryRemove(zookeeper_part_uniq_node);

    /// Even when we have lock with same part name, but with different uniq, we can remove files on S3
    children.clear();
    zookeeper->tryGetChildren(zookeeper_part_node, children);
    if (children.empty())
        /// Cleanup after last uniq removing
        zookeeper->tryRemove(zookeeper_part_node);

    return true;
}


bool StorageReplicatedMergeTree::tryToFetchIfShared(
    const IMergeTreeDataPart & part,
    const DiskPtr & disk,
    const String & path)
{
    const auto settings = getSettings();
    auto disk_type = disk->getType();
    if (!(disk->supportZeroCopyReplication() && settings->allow_remote_fs_zero_copy_replication))
        return false;

    String replica = getSharedDataReplica(part, disk_type);

    /// We can't fetch part when none replicas have this part on a same type remote disk
    if (replica.empty())
        return false;

    return executeFetchShared(replica, part.name, disk, path);
}


String StorageReplicatedMergeTree::getSharedDataReplica(
    const IMergeTreeDataPart & part, DiskType disk_type) const
{
    String best_replica;

    zkutil::ZooKeeperPtr zookeeper = tryGetZooKeeper();
    if (!zookeeper)
        return best_replica;

    String zero_copy = fmt::format("zero_copy_{}", toString(disk_type));
    String zookeeper_part_node = fs::path(zookeeper_path) / zero_copy / "shared" / part.name;

    Strings ids;
    zookeeper->tryGetChildren(zookeeper_part_node, ids);

    Strings replicas;
    for (const auto & id : ids)
    {
        String zookeeper_part_uniq_node = fs::path(zookeeper_part_node) / id;
        Strings id_replicas;
        zookeeper->tryGetChildren(zookeeper_part_uniq_node, id_replicas);
        LOG_TRACE(log, "Found zookeper replicas for {}: {}", zookeeper_part_uniq_node, id_replicas.size());
        replicas.insert(replicas.end(), id_replicas.begin(), id_replicas.end());
    }

    LOG_TRACE(log, "Found zookeper replicas for part {}: {}", part.name, replicas.size());

    Strings active_replicas;

    /// TODO: Move best replica choose in common method (here is the same code as in StorageReplicatedMergeTree::fetchPartition)

    /// Leave only active replicas.
    active_replicas.reserve(replicas.size());

    for (const String & replica : replicas)
        if ((replica != replica_name) && (zookeeper->exists(fs::path(zookeeper_path) / "replicas" / replica / "is_active")))
            active_replicas.push_back(replica);

    LOG_TRACE(log, "Found zookeper active replicas for part {}: {}", part.name, active_replicas.size());

    if (active_replicas.empty())
        return best_replica;

    /** You must select the best (most relevant) replica.
    * This is a replica with the maximum `log_pointer`, then with the minimum `queue` size.
    * NOTE This is not exactly the best criteria. It does not make sense to download old partitions,
    *  and it would be nice to be able to choose the replica closest by network.
    * NOTE Of course, there are data races here. You can solve it by retrying.
    */
    Int64 max_log_pointer = -1;
    UInt64 min_queue_size = std::numeric_limits<UInt64>::max();

    for (const String & replica : active_replicas)
    {
        String current_replica_path = fs::path(zookeeper_path) / "replicas" / replica;

        String log_pointer_str = zookeeper->get(fs::path(current_replica_path) / "log_pointer");
        Int64 log_pointer = log_pointer_str.empty() ? 0 : parse<UInt64>(log_pointer_str);

        Coordination::Stat stat;
        zookeeper->get(fs::path(current_replica_path) / "queue", &stat);
        size_t queue_size = stat.numChildren;

        if (log_pointer > max_log_pointer
            || (log_pointer == max_log_pointer && queue_size < min_queue_size))
        {
            max_log_pointer = log_pointer;
            min_queue_size = queue_size;
            best_replica = replica;
        }
    }

    return best_replica;
}

String StorageReplicatedMergeTree::findReplicaHavingPart(
    const String & part_name, const String & zookeeper_path_, zkutil::ZooKeeper::Ptr zookeeper_)
{
    Strings replicas = zookeeper_->getChildren(fs::path(zookeeper_path_) / "replicas");

    /// Select replicas in uniformly random order.
    std::shuffle(replicas.begin(), replicas.end(), thread_local_rng);

    for (const String & replica : replicas)
    {
        if (zookeeper_->exists(fs::path(zookeeper_path_) / "replicas" / replica / "parts" / part_name)
            && zookeeper_->exists(fs::path(zookeeper_path_) / "replicas" / replica / "is_active"))
            return fs::path(zookeeper_path_) / "replicas" / replica;
    }

    return {};
}

bool StorageReplicatedMergeTree::checkIfDetachedPartExists(const String & part_name)
{
    fs::directory_iterator dir_end;
    for (const std::string & path : getDataPaths())
        for (fs::directory_iterator dir_it{fs::path(path) / "detached/"}; dir_it != dir_end; ++dir_it)
            if (dir_it->path().filename().string() == part_name)
                return true;
    return false;
}

bool StorageReplicatedMergeTree::checkIfDetachedPartitionExists(const String & partition_name)
{
    fs::directory_iterator dir_end;

    for (const std::string & path : getDataPaths())
    {
        for (fs::directory_iterator dir_it{fs::path(path) / "detached/"}; dir_it != dir_end; ++dir_it)
        {
            const String file_name = dir_it->path().filename().string();
            auto part_info = MergeTreePartInfo::tryParsePartName(file_name, format_version);

            if (part_info && part_info->partition_id == partition_name)
                return true;
        }
    }
    return false;
}


bool StorageReplicatedMergeTree::createEmptyPartInsteadOfLost(zkutil::ZooKeeperPtr zookeeper, const String & lost_part_name)
{
    LOG_INFO(log, "Going to replace lost part {} with empty part", lost_part_name);
    auto metadata_snapshot = getInMemoryMetadataPtr();
    auto settings = getSettings();

    constexpr static auto TMP_PREFIX = "tmp_empty_";

    auto new_part_info = MergeTreePartInfo::fromPartName(lost_part_name, format_version);
    auto block = metadata_snapshot->getSampleBlock();

    DB::IMergeTreeDataPart::TTLInfos move_ttl_infos;

    NamesAndTypesList columns = metadata_snapshot->getColumns().getAllPhysical().filter(block.getNames());
    ReservationPtr reservation = reserveSpacePreferringTTLRules(metadata_snapshot, 0, move_ttl_infos, time(nullptr), 0, true);
    VolumePtr volume = getStoragePolicy()->getVolume(0);

    auto minmax_idx = std::make_shared<IMergeTreeDataPart::MinMaxIndex>();
    minmax_idx->update(block, getMinMaxColumnsNames(metadata_snapshot->getPartitionKey()));

    auto new_data_part = createPart(
        lost_part_name,
        choosePartType(0, block.rows()),
        new_part_info,
        createVolumeFromReservation(reservation, volume),
        TMP_PREFIX + lost_part_name);

    if (settings->assign_part_uuids)
        new_data_part->uuid = UUIDHelpers::generateV4();

    new_data_part->setColumns(columns);
    new_data_part->rows_count = block.rows();

    {
        auto lock = lockParts();
        auto parts_in_partition = getDataPartsPartitionRange(new_part_info.partition_id);
        if (parts_in_partition.empty())
        {
            LOG_WARNING(log, "Empty part {} is not created instead of lost part because there are no parts in partition {} (it's empty), resolve this manually using DROP PARTITION.", lost_part_name, new_part_info.partition_id);
            return false;
        }

        new_data_part->partition = (*parts_in_partition.begin())->partition;
    }

    new_data_part->minmax_idx = std::move(minmax_idx);
    new_data_part->is_temp = true;


    SyncGuardPtr sync_guard;
    if (new_data_part->isStoredOnDisk())
    {
        /// The name could be non-unique in case of stale files from previous runs.
        String full_path = new_data_part->getFullRelativePath();

        if (new_data_part->volume->getDisk()->exists(full_path))
        {
            LOG_WARNING(log, "Removing old temporary directory {}", fullPath(new_data_part->volume->getDisk(), full_path));
            new_data_part->volume->getDisk()->removeRecursive(full_path);
        }

        const auto disk = new_data_part->volume->getDisk();
        disk->createDirectories(full_path);

        if (getSettings()->fsync_part_directory)
            sync_guard = disk->getDirectorySyncGuard(full_path);
    }

    /// This effectively chooses minimal compression method:
    ///  either default lz4 or compression method with zero thresholds on absolute and relative part size.
    auto compression_codec = getContext()->chooseCompressionCodec(0, 0);

    const auto & index_factory = MergeTreeIndexFactory::instance();
    MergedBlockOutputStream out(new_data_part, metadata_snapshot, columns, index_factory.getMany(metadata_snapshot->getSecondaryIndices()), compression_codec);
    bool sync_on_insert = settings->fsync_after_insert;

    out.write(block);
    /// TODO(ab): What projections should we add to the empty part? How can we make sure that it
    /// won't block future merges? Perhaps we should also check part emptiness when selecting parts
    /// to merge.
    out.writeSuffixAndFinalizePart(new_data_part, sync_on_insert);

    try
    {
        MergeTreeData::Transaction transaction(*this);
        auto replaced_parts = renameTempPartAndReplace(new_data_part, nullptr, &transaction);

        if (!replaced_parts.empty())
        {
            Strings part_names;
            for (const auto & part : replaced_parts)
                part_names.emplace_back(part->name);

            /// Why this exception is not a LOGICAL_ERROR? Because it's possible
            /// to have some source parts for the lost part if replica currently
            /// cloning from another replica, but source replica lost covering
            /// part and finished MERGE_PARTS before clone. It's an extremely
            /// rare case and it's unclear how to resolve it better. Eventually
            /// source replica will replace lost part with empty part and we
            /// will fetch this empty part instead of our source parts. This
            /// will make replicas consistent, but some data will be lost.
            throw Exception(ErrorCodes::INCORRECT_DATA, "Tried to create empty part {}, but it replaces existing parts {}.", lost_part_name, fmt::join(part_names, ", "));
        }

        while (true)
        {

            Coordination::Requests ops;
            Coordination::Stat replicas_stat;
            auto replicas_path = fs::path(zookeeper_path) / "replicas";
            Strings replicas = zookeeper->getChildren(replicas_path, &replicas_stat);

            /// In rare cases new replica can appear during check
            ops.emplace_back(zkutil::makeCheckRequest(replicas_path, replicas_stat.version));

            for (const String & replica : replicas)
            {
                String current_part_path = fs::path(zookeeper_path) / "replicas" / replica / "parts" / lost_part_name;

                /// We must be sure that this part doesn't exist on other replicas
                if (!zookeeper->exists(current_part_path))
                {
                    ops.emplace_back(zkutil::makeCreateRequest(current_part_path, "", zkutil::CreateMode::Persistent));
                    ops.emplace_back(zkutil::makeRemoveRequest(current_part_path, -1));
                }
                else
                {
                    throw Exception(ErrorCodes::DUPLICATE_DATA_PART, "Part {} already exists on replica {} on path {}", lost_part_name, replica, current_part_path);
                }
            }

            getCommitPartOps(ops, new_data_part);

            Coordination::Responses responses;
            if (auto code = zookeeper->tryMulti(ops, responses); code == Coordination::Error::ZOK)
            {
                transaction.commit();
                break;
            }
            else if (code == Coordination::Error::ZBADVERSION)
            {
                LOG_INFO(log, "Looks like new replica appearead while creating new empty part, will retry");
            }
            else
            {
                zkutil::KeeperMultiException::check(code, ops, responses);
            }
        }
    }
    catch (const Exception & ex)
    {
        LOG_WARNING(log, "Cannot commit empty part {} with error {}", lost_part_name, ex.displayText());
        return false;
    }

    LOG_INFO(log, "Created empty part {} instead of lost part", lost_part_name);

    return true;
}

}
