/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_metadata_util.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace shardmetadatautil {

namespace {

const WriteConcernOptions kLocalWriteConcern(1,
                                             WriteConcernOptions::SyncMode::UNSET,
                                             Milliseconds(0));

}  // namespace

StatusWith<std::pair<BSONObj, OID>> getCollectionShardKeyAndEpoch(
    OperationContext* opCtx,
    ShardingCatalogClient* catalogClient,
    const NamespaceString& nss,
    bool isShardPrimary) {
    if (isShardPrimary) {
        // Get the config.collections entry for 'nss'.
        auto statusWithColl = catalogClient->getCollection(opCtx, nss.ns());
        if (!statusWithColl.isOK()) {
            if (statusWithColl.getStatus() == ErrorCodes::NamespaceNotFound) {
                auto status = dropChunksAndDeleteCollectionsEntry(
                    opCtx, NamespaceString(ChunkType::ConfigNS + "." + nss.ns()), nss);
                if (!status.isOK()) {
                    return status;
                }
            }

            return statusWithColl.getStatus();
        }
        auto collInfo = statusWithColl.getValue().value;

        // Update the shard's config.collections entry so that secondaries receive any changes.
        Status updateStatus = updateCollectionEntry(opCtx,
                                                    nss,
                                                    BSON(ShardCollectionType::uuid(nss.ns())),
                                                    ShardCollectionType(collInfo).toBSON());
        if (!updateStatus.isOK()) {
            return updateStatus;
        }

        return std::pair<BSONObj, OID>(collInfo.getKeyPattern().toBSON(), collInfo.getEpoch());
    } else {  // shard secondary
        auto statusWithCollectionEntry = readShardCollectionEntry(opCtx, nss);
        if (!statusWithCollectionEntry.isOK()) {
            return statusWithCollectionEntry.getStatus();
        }
        ShardCollectionType shardCollTypeEntry = statusWithCollectionEntry.getValue();
        if (!shardCollTypeEntry.isLastConsistentCollectionVersionSet()) {
            // The collection has been dropped since the refresh began.
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "Could not load metadata because collection " << nss.ns()
                                  << " was dropped"};
        }

        return std::pair<BSONObj, OID>(
            shardCollTypeEntry.getKeyPattern().toBSON(),
            shardCollTypeEntry.getLastConsistentCollectionVersionEpoch());
    }
}


StatusWith<ShardCollectionType> readShardCollectionEntry(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    Query fullQuery(BSON(ShardCollectionType::uuid() << nss.ns()));
    fullQuery.readPref(ReadPreference::SecondaryOnly, BSONArray());
    try {
        DBDirectClient client(opCtx);
        std::unique_ptr<DBClientCursor> cursor =
            client.query(ShardCollectionType::ConfigNS.c_str(), fullQuery, 1);
        if (!cursor) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Failed to establish a cursor for reading "
                                        << ShardCollectionType::ConfigNS
                                        << " from local storage");
        }

        if (!cursor->more()) {
            // The collection has been dropped.
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "collection " << nss.ns() << " not found");
        }

        BSONObj document = cursor->nextSafe();
        auto statusWithCollectionEntry = ShardCollectionType::fromBSON(document);
        if (!statusWithCollectionEntry.isOK()) {
            return statusWithCollectionEntry.getStatus();
        }

        // There should only be one entry per namespace!
        invariant(!cursor->more());

        return statusWithCollectionEntry.getValue();
    } catch (const DBException& ex) {
        return {ex.toStatus().code(),
                str::stream() << "Failed to read the '" << nss.ns()
                              << "' entry locally from config.collections"
                              << causedBy(ex.toStatus())};
    }
}

Status updateCollectionEntry(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BSONObj& query,
                             const BSONObj& update) {
    const BSONElement idField = query.getField("_id");
    invariant(!idField.eoo());

    // Want to modify the document, not replace it.
    BSONObjBuilder updateBuilder;
    updateBuilder.append("$set", update);

    std::unique_ptr<BatchedUpdateDocument> updateDoc(new BatchedUpdateDocument());
    updateDoc->setQuery(query);
    updateDoc->setUpdateExpr(updateBuilder.obj());
    updateDoc->setUpsert(true);

    std::unique_ptr<BatchedUpdateRequest> updateRequest(new BatchedUpdateRequest());
    updateRequest->addToUpdates(updateDoc.release());

    BatchedCommandRequest request(updateRequest.release());
    request.setNS(NamespaceString(CollectionType::ConfigNS));
    request.setWriteConcern(kLocalWriteConcern.toBSON());
    BSONObj cmdObj = request.toBSON();

    try {
        DBDirectClient client(opCtx);

        rpc::UniqueReply commandResponse = client.runCommandWithMetadata(
            "config", cmdObj.firstElementFieldName(), rpc::makeEmptyMetadata(), cmdObj);
        BSONObj responseReply = commandResponse->getCommandReply().getOwned();

        Status commandStatus = getStatusFromCommandResult(commandResponse->getCommandReply());
        if (!commandStatus.isOK()) {
            return commandStatus;
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return {ex.toStatus().code(),
                str::stream() << "Failed to locally update the '" << nss.ns()
                              << "' entry in config.collections"
                              << causedBy(ex.toStatus())};
    }
}

Status writeNewChunks(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const std::vector<ChunkType>& chunks,
                      const OID& currEpoch) {
    NamespaceString chunkMetadataNss(ChunkType::ConfigNS + "." + nss.ns());

    try {
        DBDirectClient client(opCtx);

        /**
         * Here are examples of the operations that can happen on the config server to update
         * the config.chunks collection. 'chunks' only includes the chunks that result from the
         * operations, which can be read from the config server, not any that were removed, so
         * we must delete any chunks that overlap with the new 'chunks'.
         *
         * CollectionVersion = 10.3
         *
         * moveChunk
         * {_id: 3, max: 5, version: 10.1} --> {_id: 3, max: 5, version: 11.0}
         *
         * splitChunk
         * {_id: 3, max: 9, version 10.3} --> {_id: 3, max: 5, version 10.4}
         *                                    {_id: 5, max: 8, version 10.5}
         *                                    {_id: 8, max: 9, version 10.6}
         *
         * mergeChunk
         * {_id: 10, max: 14, version 4.3} --> {_id: 10, max: 22, version 10.4}
         * {_id: 14, max: 19, version 7.1}
         * {_id: 19, max: 22, version 2.0}
         *
         */
        for (auto& chunk : chunks) {
            // Check for a different epoch.
            if (!chunk.getVersion().hasEqualEpoch(currEpoch)) {
                // This means the collection was dropped and recreated. Drop the chunk metadata
                // and return.
                Status status = dropChunksAndDeleteCollectionsEntry(opCtx, chunkMetadataNss, nss);
                if (!status.isOK()) {
                    return status;
                }

                return Status{ErrorCodes::RemoteChangeDetected,
                              str::stream() << "Invalid chunks found when reloading '"
                                            << nss.toString()
                                            << "'. Previous collection epoch was '"
                                            << currEpoch.toString()
                                            << "', but unexpectedly found a new epoch '"
                                            << chunk.getVersion().epoch().toString()
                                            << "'. Collection was dropped and recreated."};
            }

            // Delete any overlapping chunk ranges. Overlapping chunks will have a min value
            // ("_id") between (chunk.min, chunk.max].
            //
            // query: { "_id" : {"$gte": chunk.min, "$lt": chunk.max}}
            auto deleteDocs(stdx::make_unique<BatchedDeleteDocument>());
            deleteDocs->setQuery(BSON(ChunkType::minShardID << BSON(
                                          "$gte" << chunk.getMin() << "$lt" << chunk.getMax())));
            deleteDocs->setLimit(0);

            auto deleteRequest(stdx::make_unique<BatchedDeleteRequest>());
            deleteRequest->addToDeletes(deleteDocs.release());

            BatchedCommandRequest batchedDeleteRequest(deleteRequest.release());
            batchedDeleteRequest.setNS(chunkMetadataNss);
            const BSONObj deleteCmdObj = batchedDeleteRequest.toBSON();

            rpc::UniqueReply deleteCommandResponse =
                client.runCommandWithMetadata(chunkMetadataNss.db().toString(),
                                              deleteCmdObj.firstElementFieldName(),
                                              rpc::makeEmptyMetadata(),
                                              deleteCmdObj);
            auto deleteStatus =
                getStatusFromCommandResult(deleteCommandResponse->getCommandReply());

            if (!deleteStatus.isOK()) {
                return deleteStatus;
            }

            // Now the document can be expected to cleanly insert without overlap.
            auto insert(stdx::make_unique<BatchedInsertRequest>());
            insert->addToDocuments(chunk.toShardBSON());

            BatchedCommandRequest insertRequest(insert.release());
            insertRequest.setNS(chunkMetadataNss);
            const BSONObj insertCmdObj = insertRequest.toBSON();

            rpc::UniqueReply commandResponse =
                client.runCommandWithMetadata(chunkMetadataNss.db().toString(),
                                              insertCmdObj.firstElementFieldName(),
                                              rpc::makeEmptyMetadata(),
                                              insertCmdObj);
            auto insertStatus = getStatusFromCommandResult(commandResponse->getCommandReply());

            if (!insertStatus.isOK()) {
                return insertStatus;
            }
        }

        // Must update the config.collections 'lastConsistentCollectionVersion' field so that
        // secondaries can load the latest config.chunks.ns writes.
        if (!chunks.empty()) {
            BSONObjBuilder builder;
            chunks.back().getVersion().appendWithFieldForCommands(
                &builder, ShardCollectionType::lastConsistentCollectionVersion());
            BSONObj update = builder.obj();

            auto collUpdateStatus = updateCollectionEntry(
                opCtx, nss, BSON(ShardCollectionType::uuid(nss.ns())), update);
            if (!collUpdateStatus.isOK()) {
                return collUpdateStatus;
            }
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

Status dropChunksAndDeleteCollectionsEntry(OperationContext* opCtx,
                                           const NamespaceString& chunkMetadataNss,
                                           const NamespaceString& collectionsEntryNss) {
    try {
        DBDirectClient client(opCtx);

        // Drop the config.chunks.ns collection specified by 'chunkMetadataNss'.
        rpc::UniqueReply commandResponse =
            client.runCommandWithMetadata(chunkMetadataNss.db().toString(),
                                          "drop",
                                          rpc::makeEmptyMetadata(),
                                          BSON("drop" << chunkMetadataNss.coll()));
        Status status = getStatusFromCommandResult(commandResponse->getCommandReply());

        if (!status.isOK() && status.code() != ErrorCodes::NamespaceNotFound) {
            return status;
        }

        // Delete the config.collections entry matching 'collectionsEntryNss'.
        auto deleteDocs(stdx::make_unique<BatchedDeleteDocument>());
        deleteDocs->setQuery(BSON(ShardCollectionType::uuid << collectionsEntryNss.ns()));
        deleteDocs->setLimit(0);

        auto deleteRequest(stdx::make_unique<BatchedDeleteRequest>());
        deleteRequest->addToDeletes(deleteDocs.release());

        BatchedCommandRequest batchedDeleteRequest(deleteRequest.release());
        batchedDeleteRequest.setNS(NamespaceString(ShardCollectionType::ConfigNS));
        const BSONObj deleteCmdObj = batchedDeleteRequest.toBSON();

        rpc::UniqueReply deleteCommandResponse = client.runCommandWithMetadata(
            "config", deleteCmdObj.firstElementFieldName(), rpc::makeEmptyMetadata(), deleteCmdObj);
        auto deleteStatus = getStatusFromCommandResult(deleteCommandResponse->getCommandReply());

        if (!deleteStatus.isOK()) {
            return deleteStatus;
        }

        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

}  // namespace shardmetadatautil
}  // namespace mongo
