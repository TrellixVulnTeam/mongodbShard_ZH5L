
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_write.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/client/connpool.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/chunk_manager_targeter.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

#include "mongo/bson/mutable/document.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

//heejin added
#include <string>
#define DYNAMIC 1
//#define STATIC 1
//#define ORIGINAL 1
namespace mongo {
namespace {

const uint64_t kTooManySplitPoints = 4;
int global_update=0;

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
    dassert(response->isValid(NULL));
}

/**
 * Returns the split point that will result in one of the chunk having exactly one document. Also
 * returns an empty document if the split point cannot be determined.
 *
 * doSplitAtLower - determines which side of the split will have exactly one document. True means
 * that the split point chosen will be closer to the lower bound.
 *
 * NOTE: this assumes that the shard key is not "special"- that is, the shardKeyPattern is simply an
 * ordered list of ascending/descending field names. For example {a : 1, b : -1} is not special, but
 * {a : "hashed"} is.
 */
BSONObj findExtremeKeyForShard(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const ShardId& shardId,
                               const ShardKeyPattern& shardKeyPattern,
                               bool doSplitAtLower) {
    Query q;

    if (doSplitAtLower) {
        q.sort(shardKeyPattern.toBSON());
    } else {
        // need to invert shard key pattern to sort backwards
        // TODO: make a helper in ShardKeyPattern?
        BSONObjBuilder r;

        BSONObjIterator i(shardKeyPattern.toBSON());
        while (i.more()) {
            BSONElement e = i.next();
            uassert(10163, "can only handle numbers here - which i think is correct", e.isNumber());
            r.append(e.fieldName(), -1 * e.number());
        }

        q.sort(r.obj());
    }

    // Find the extreme key
    const auto shardConnStr = [&]() {
        const auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
        return shard->getConnString();
    }();

    ScopedDbConnection conn(shardConnStr);

    BSONObj end;

    if (doSplitAtLower) {
        // Splitting close to the lower bound means that the split point will be the
        // upper bound. Chunk range upper bounds are exclusive so skip a document to
        // make the lower half of the split end up with a single document.
        std::unique_ptr<DBClientCursor> cursor = conn->query(nss.ns(),
                                                             q,
                                                             1, /* nToReturn */
                                                             1 /* nToSkip */);

        uassert(28736,
                str::stream() << "failed to initialize cursor during auto split due to "
                              << "connection problem with "
                              << conn->getServerAddress(),
                cursor.get() != nullptr);

        if (cursor->more()) {
            end = cursor->next().getOwned();
        }
    } else {
        end = conn->findOne(nss.ns(), q);
    }

    conn.done();

    if (end.isEmpty()) {
        return BSONObj();
    }

    return shardKeyPattern.extractShardKeyFromDoc(end);
}

/**
 * Splits the chunks touched based from the targeter stats if needed.
 */
void splitIfNeeded(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const TargeterStats& stats,
		   std::string string_key) {
    auto routingInfoStatus = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss);
    if (!routingInfoStatus.isOK()) {
        log() << "failed to get collection information for " << nss
              << " while checking for auto-split" << causedBy(routingInfoStatus.getStatus());
        return;
    }

    auto& routingInfo = routingInfoStatus.getValue();

    if (!routingInfo.cm()) {
        return;
    }

    for (auto it = stats.chunkSizeDelta.cbegin(); it != stats.chunkSizeDelta.cend(); ++it) {
        std::shared_ptr<Chunk> chunk;
        try {
            chunk = routingInfo.cm()->findIntersectingChunkWithSimpleCollation(it->first);
        } catch (const AssertionException& ex) {
            warning() << "could not find chunk while checking for auto-split: "
                      << causedBy(redact(ex));
            return;
        }

    //log() << "heejjin split IFNEED: " << double_key;
    // heejin added)
    // sum of chunk element
#ifndef ORIGINAL 
	chunk.get()->add_cnt();
   	chunk.get()->add_split_sum(string_key); 
#endif
   	//chunk.get()->update_split_average(string_key); 
	//log() << "heejjin update split average : " << chunk.get()->get_split_average() << " when cnt : " << chunk.get()->get_cnt();
        updateChunkWriteStatsAndSplitIfNeeded(
            opCtx, routingInfo.cm().get(), chunk.get(), it->second);
    }
}

}  // namespace

void ClusterWriter::write(OperationContext* opCtx,
                          const BatchedCommandRequest& request,
                          BatchWriteExecStats* stats,
                          BatchedCommandResponse* response) {
    const NamespaceString& nss = request.getNS();
//    log() << "jinnnn ClusterWriter::write "  << nss;
std::string string_key;
    LastError::Disabled disableLastError(&LastError::get(opCtx->getClient()));

    // Config writes and shard writes are done differently
    if (nss.db() == NamespaceString::kAdminDb) {
    	log() << "jin conjin config write";
        Grid::get(opCtx)->catalogClient()->writeConfigServerDirect(opCtx, request, response);
    } else { // jin) shard writes
        TargeterStats targeterStats;

        {
            ChunkManagerTargeter targeter(request.getTargetingNS(), &targeterStats);

            Status targetInitStatus = targeter.init(opCtx);
            if (!targetInitStatus.isOK()) {
                toBatchError({targetInitStatus.code(),
                              str::stream() << "unable to initialize targeter for"
                                            << (request.isInsertIndexRequest() ? " index" : "")
                                            << " write op for collection "
                                            << request.getTargetingNS().ns()
                                            << causedBy(targetInitStatus)},
                             response);
                return;
            }

            auto swEndpoints = targeter.targetCollection();
            if (!swEndpoints.isOK()) {
                toBatchError({swEndpoints.getStatus().code(),
                              str::stream() << "unable to target"
                                            << (request.isInsertIndexRequest() ? " index" : "")
                                            << " write op for collection "
                                            << request.getTargetingNS().ns()
                                            << causedBy(swEndpoints.getStatus())},
                             response);
                return;
            }

            const auto& endpoints = swEndpoints.getValue();
  //  	log() << "jin endpoints during shard request: " << request.toString();
//	log() << "jin endpoints during shard response: " << request.toBSON();
//	log() << "jin endpoints during shard response nField: " << request.toBSON().nFields();

	if(request.toBSON().hasElement("documents"))
	{
//		log() << "jin element in!!!!!";
//		log() << "jin endpoints during shard response getOwned: " << request.toBSON().getObjectField("documents").getOwned();

		mongo::mutablebson::Document doc(request.toBSON().getObjectField("documents").getOwned());
		mongo::mutablebson::Element zero =doc.root()["0"];
		mongo::mutablebson::Element key =zero[0];

		if(zero.toString() != "INVALID-MUTABLE-ELEMENT"){
			string_key = key.getValue().toString();
			//string_key = string_key.replaceAll("user","");
			string_key.replace(string_key.find("_"), 10, "");
			string_key.erase(string_key.end()-1);
			//log() << "string key inserted: " << string_key;
			//double_key =atoi(string_key.c_str());
			//std::istringstream iss(string_key);
			//iss >> double_key; 
//			log() << "double key inserted: " << double_key;
//stoi(string_key);
		}
		else
			log() << "jin endpoints INVALID" ;
	
	}		

            // Handle sharded config server writes differently.
            if (std::any_of(endpoints.begin(), endpoints.end(), [](const auto& it) {
                    return it.shardName == ShardRegistry::kConfigServerShardId;
                })) {
                // There should be no namespaces that partially target config servers.
                invariant(endpoints.size() == 1);

                // For config servers, we do direct writes.
                Grid::get(opCtx)->catalogClient()->writeConfigServerDirect(
                    opCtx, request, response);
                return;
            }

            BatchWriteExec::executeBatch(opCtx, targeter, request, response, stats);
        }

        splitIfNeeded(opCtx, request.getNS(), targeterStats, string_key);
    }
}
//heejin_found split
void updateChunkWriteStatsAndSplitIfNeeded(OperationContext* opCtx,
                                           ChunkManager* manager,
                                           Chunk* chunk,
                                           long dataWritten) {

	global_update++;
    // Disable lastError tracking so that any errors, which occur during auto-split do not get
    // bubbled up on the client connection doing a write
    LastError::Disabled disableLastError(&LastError::get(opCtx->getClient()));
    //log() << "jin!!! updateChunkWriteStatsAndSplitIfNeeded: " << global_update;

    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

    const bool minIsInf =
        (0 == manager->getShardKeyPattern().getKeyPattern().globalMin().woCompare(chunk->getMin()));
    const bool maxIsInf =
        (0 == manager->getShardKeyPattern().getKeyPattern().globalMax().woCompare(chunk->getMax()));

    const uint64_t chunkBytesWritten = chunk->addBytesWritten(dataWritten);
  //  log() << "jin!!! addBytesWritten(dataWritten) " << dataWritten;
    //log() << "jin!!! addBytesWritten(chunkBytesWritten) " << chunkBytesWritten;
    const uint64_t desiredChunkSize = balancerConfig->getMaxChunkSizeBytes();
/*	if(!chunk->shouldSplit(desiredChunkSize, minIsInf, maxIsInf))
	{
		log() << "heejjin error 1: " << desiredChunkSize;
		if(minIsInf)
			log() << "heejjin error 1: minIsInf";;
		if(maxIsInf)
			log() << "heejjin error 1 : maxIsInf";

		
	}
	if(!balancerConfig->getShouldAutoSplit())
	{
		log() << "heejjin error 2";	
	}*/
    if (!chunk->shouldSplit(desiredChunkSize, minIsInf, maxIsInf) ||
        !balancerConfig->getShouldAutoSplit()) {
	//log() << "heejin_ return: " << global_update;
        return;
    }

    const NamespaceString nss(manager->getns());

    if (!manager->_autoSplitThrottle._splitTickets.tryAcquire()) {
        LOG(1) << "won't auto split because not enough tickets: " << nss;
        return;
    }

    TicketHolderReleaser releaser(&(manager->_autoSplitThrottle._splitTickets));

    const ChunkRange chunkRange(chunk->getMin(), chunk->getMax());
//log() << "ChunkRange : " << chunkRange;
    try {
        // Ensure we have the most up-to-date balancer configuration
        uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));

        if (!balancerConfig->getShouldAutoSplit()) {
            return;
        }

        LOG(1) << "about to initiate autosplit: " << redact(chunk->toString())
               << " dataWritten: " << chunkBytesWritten
               << " desiredChunkSize: " << desiredChunkSize;

        const uint64_t chunkSizeToUse = [&]() {
            const uint64_t estNumSplitPoints = chunkBytesWritten / desiredChunkSize * 2;

            if (estNumSplitPoints >= kTooManySplitPoints) {
                // The current desired chunk size will split the chunk into lots of small chunk and
                // at the worst case this can result into thousands of chunks. So check and see if a
                // bigger value can be used.
                return std::min(chunkBytesWritten, balancerConfig->getMaxChunkSizeBytes());
            } else {
                return desiredChunkSize;
            }
        }();
//heejin) splitpoints call selectChunkSplitPoints
	//uint64_t split_average = chunk->get_split_average();
	uint64_t split_average = chunk->get_split_sum()/chunk->get_cnt();
//	log() << "heejjin update split_average: " << split_average;
//	log() << "jin!! yamae global split " << global_split;
//	log() << "jin!! yanae key is " << global_update;

        auto splitPoints =
            uassertStatusOK(shardutil::selectChunkSplitPoints(opCtx,
                                                              chunk->getShardId(),
                                                              nss,
                                                              manager->getShardKeyPattern(),
                                                              chunkRange,
                                                              chunkSizeToUse,
                                                              boost::none));
/*
        auto splitPoints =
            uassertStatusOK(shardutil::selectChunkSplitPoints(opCtx,
                                                              chunk->getShardId(),
                                                              nss,
                                                              manager->getShardKeyPattern(),
                                                              chunkRange,
                                                              chunkSizeToUse,
                                                              boost::none,
								split_average));
*/
	//BSONObjBuilder current_key;
	//current_key.append("splitKeys", global_update+49800);
	//splitPoints.push_back(current_key.obj());
        if (splitPoints.size() <= 1) {
	//	log() << "splitpoints.size() <= 1 " << global_update;
            // No split points means there isn't enough data to split on; 1 split point means we
            // have
            // between half the chunk size to full chunk size so there is no need to split yet
            chunk->clearBytesWritten();
            return;
        }
#ifndef ORIGINAL
	else {
	
	//	log() << "splitpoints.size() > 1 so split average insert start";
		uint64_t target = split_average;
		std::ostringstream o;
		o << split_average;
		std::string str;
		str += ("user"+o.str());
		BSONObjBuilder current_key;
		current_key.append("_id", str);
		//BSONObjIterator it(splitPoints);
		std::vector<BSONObj>::iterator it = splitPoints.begin();
		int n=0;
		std::string string_key;
		std::string prefix_key ;
		while(it != splitPoints.end()) {
		//for(int i=0; i<splitPoints.size(); i++) { 
			uint64_t k=0;
			BSONElement e = it->getField("_id");
			//int k = e.getValue().numberInt();
			string_key = e.String();
		//	log() << "heejin debugging " << string_key;
			string_key.replace(string_key.find("user"), 4, "");
			string_key.erase(string_key.end()-1);
			
			prefix_key = string_key.substr(0,10);
			std::istringstream iss(prefix_key);
			iss >> k; 
		//	log() << "k value : " << k;
			if(k <= split_average) {
				target = k;
				n++;
			}
			it++;

		}
		BSONObj key;
		uint64_t int_chunk_max, int_chunk_min;
		//calculate chunk range (prefix)
		if(maxIsInf) {
			//log() << "maxIsInf int_chunk_max";
			key = findExtremeKeyForShard(
			    opCtx, nss, chunk->getShardId(), manager->getShardKeyPattern(), false);
			std::string string_key = key.toString();
			string_key.replace(string_key.find("{"), 12, "");
			string_key.erase(string_key.end()-1);
			std::string prefix_key = string_key.substr(0,10);
			std::istringstream iss(prefix_key);
			iss >> int_chunk_max;
		}
		else{
			BSONObj max = chunk->getMax();	
			std::string string_key = max.toString();
			string_key.replace(string_key.find("{"), 12, "");
			string_key.erase(string_key.end()-1);
			std::string prefix_key = string_key.substr(0,10);
			std::istringstream iss(prefix_key);
			iss >> int_chunk_max;
		}

		if(minIsInf) {
			//log() << "minIsInf int_chunk_min";
			key = findExtremeKeyForShard(
			    opCtx, nss, chunk->getShardId(), manager->getShardKeyPattern(), true);
			string_key = key.toString();
			//log() << "minIsInf chunk getmax " << string_key;
			string_key.replace(string_key.find("{"), 12, "");
			string_key.erase(string_key.end()-1);
			prefix_key = string_key.substr(0,10);
			std::istringstream isss(prefix_key);
			isss >> int_chunk_min;

		}
		else{
			BSONObj min = chunk->getMin();	
			std::string string_key = min.toString();
			//log() << "chunk getmin " << string_key;
			string_key.replace(string_key.find("{"), 12, "");
			string_key.erase(string_key.end()-1);
			std::string prefix_key = string_key.substr(0,10);
			std::istringstream isss(prefix_key);
			isss >> int_chunk_min;
			//log() << "chunk prefix intgetmin " << int_chunk_min;
		}
		//log() << "heejin debugging)  max : " << int_chunk_max << " / min : " << int_chunk_min;
		uint64_t chunk_range = (uint64_t)((double)int_chunk_max - (double)int_chunk_min);
		double shift_params = 0.1;
		//uint64_t shift = 100000000;
		uint64_t shift = chunk_range * shift_params;
		
		//log() << "chunk range : " << chunk_range <<", shift : " << shift << "splitPoints size : " << splitPoints.size() <<", average : " << split_average <<", n : " << n;	
#endif

		
#ifdef STATIC	
		//static tuning
		for(uint8_t i=0; i<splitPoints.size(); i++) {
			uint64_t k=0;
			std::string new_split_key = "user";
			BSONElement e = splitPoints[i].getField("_id");	
			std::string string_key = e.String();
			string_key.replace(string_key.find("user"), 4, "");
			string_key.erase(string_key.end()-1);
			new_split_key += string_key;
			std::string prefix_key = string_key.substr(0,10);
			std::istringstream iss(prefix_key);
			iss >> k;
			if(i>=n)
				k -= shift;
			else if((i<n)&(k!=split_average))// n>i, meaning splitPoints[i] is bigger than split_average
				k += shift;
			std::string k_string;
			std::ostringstream o;
			o << k;
			k_string += o.str();

			new_split_key.replace(new_split_key.begin()+4, new_split_key.begin()+15, k_string);
			//new_split_key.replace(new_split_key.begin()+4, new_split_key.begin()+15, prefix_key.begin(), prefix_key.begin()+11);
			BSONObjBuilder new_split_BSON;
			new_split_BSON.append("_id", new_split_key);
			//log() << "before splitPoints[i] : " << splitPoints[i];
			splitPoints[i] = new_split_BSON.obj().getOwned();
			//log() << "after splitPoints[i] : " << splitPoints[i];
		}
#elif DYNAMIC	
		//dynamic tuning
		int right=n;
		uint64_t right_shift = shift;
		//log() << "dynamic tuning start";
		for(uint8_t i=right; i<splitPoints.size(); i++) {
			uint64_t k=0;
			std::string new_split_key = "user";
			BSONElement e = splitPoints[i].getField("_id");	
			std::string string_key = e.String();
			string_key.replace(string_key.find("user"), 4, "");
			string_key.erase(string_key.end()-1);
			new_split_key += string_key;
		//	log() << "right new_split_key : " << new_split_key;
			std::string prefix_key = string_key.substr(0,10);
		//	log() << "right prefix_key after parsing : " << prefix_key;
			std::istringstream iss(prefix_key);
			iss >> k;
			k -= right_shift;
			right_shift= right_shift/2;
			std::string k_string;
			std::ostringstream o;
			o << k;
			k_string += o.str();

			//log() << "right new_split_key before replace : " << new_split_key;
			new_split_key.replace(new_split_key.begin()+4, new_split_key.begin()+14, k_string);
			//log() << "right new_split_key after replace : " << new_split_key;
			//new_split_key.replace(new_split_key.begin()+4, new_split_key.begin()+15, prefix_key.begin(), prefix_key.begin()+11);
			BSONObjBuilder new_split_BSON;
			new_split_BSON.append("_id", new_split_key);
			//log() << "right shift : " << right_shift;
			//log() << "right for, before splitPoints[" << (int)i << "] : " << splitPoints[i];
			splitPoints[i] = new_split_BSON.obj().getOwned();
			//log() << "right for, after splitPoints[" << (int)i << "] : " << splitPoints[i];
		}
		int left=n-1;
		uint64_t left_shift = shift;
		if(left>=0) {
			for(int i=left; i>=0; i--) {
				uint64_t k=0;
				std::string new_split_key = "user";
				BSONElement e = splitPoints[i].getField("_id");	
				std::string string_key = e.String();
				string_key.replace(string_key.find("user"), 4, "");
				string_key.erase(string_key.end()-1);
				new_split_key += string_key;
			//	log() << "left new_split_key : " << new_split_key;
				std::string prefix_key = string_key.substr(0,10);
			//	log() << "left prefix_key after parsing : " << prefix_key;
				std::istringstream iss(prefix_key);
				iss >> k;
				if(k!=split_average) {// if k == split_average, no need to shift
					k += left_shift;
					left_shift=left_shift/2;
				}
				std::string k_string;
				std::ostringstream o;
				o << k;
				k_string += o.str();

				//log() << "right new_split_key before replace : " << new_split_key;
				new_split_key.replace(new_split_key.begin()+4, new_split_key.begin()+14, k_string);
				//log() << "right new_split_key after replace : " << new_split_key;
				//new_split_key.replace(new_split_key.begin()+4, new_split_key.begin()+15, prefix_key.begin(), prefix_key.begin()+11);
				BSONObjBuilder new_split_BSON;
				new_split_BSON.append("_id", new_split_key);
			//	log() << "left shift : " << left_shift;
				//log() << "left for, before splitPoints[" << (int)i << "] : " << splitPoints[i];
				splitPoints[i] = new_split_BSON.obj().getOwned();
				//log() << "left for, after splitPoints[" << (int)i << "] : " << splitPoints[i];
			}
		}



#elif ORIGINAL
	
#else
	log() << "usage : #define DYNAMIC or #define STATIC or #define ORIGINAL";
#endif
	}

//	log() << "heejin*** found-front : " << splitPoints.front();
//	log() << "heejin*** found-back : " << splitPoints.back();
        if (minIsInf || maxIsInf) {
            // We don't want to reset _dataWritten since we want to check the other side right away
        } else {
            // We're splitting, so should wait a bit
//	log() << "heejin** found-front : " << splitPoints.front();
//	log() << "heejin** found-back : " << splitPoints.back();
            chunk->clearBytesWritten();
//	log() << "heejin* found-front : " << splitPoints.front();
//	log() << "heejin* found-back : " << splitPoints.back();
        }

	//log() << "heejin_ found-front : " << splitPoints.front();
	//log() << "heejin_ found-back : " << splitPoints.back();
        // We assume that if the chunk being split is the first (or last) one on the collection,
        // this chunk is likely to see more insertions. Instead of splitting mid-chunk, we use the
        // very first (or last) key as a split point.
        //
        // This heuristic is skipped for "special" shard key patterns that are not likely to produce
        // monotonically increasing or decreasing values (e.g. hashed shard keys).
        
//	auto tmp_splitPoints 


	if (KeyPattern::isOrderedKeyPattern(manager->getShardKeyPattern().toBSON())) {
//	log() << "heejin ) key pattern if statement in";
            if (minIsInf) {
                BSONObj key = findExtremeKeyForShard(
                    opCtx, nss, chunk->getShardId(), manager->getShardKeyPattern(), true);
                if (!key.isEmpty()) {
                    splitPoints.front() = key.getOwned();
		//heejin debug
		//log() << "heejin) minIsInf" << splitPoints.front() ;
                }
            } else if (maxIsInf) {
                BSONObj key = findExtremeKeyForShard(
                    opCtx, nss, chunk->getShardId(), manager->getShardKeyPattern(), false);
                if (!key.isEmpty()) {
			//toBSON().getObjectField("documents").getOwned();
		//	splitPoints.push_back(current_key.obj());

                    splitPoints.back() = key.getOwned();
		//heejin debug
		//log() << "heejin) maxIsInf" << splitPoints.back() ;
                }
            }
        }
	    // Make sure splitKeys is in ascending order
/*	    std::sort(
		splitPoints.begin(), splitPoints.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

*/

//	log() << "heejin__ found-front : " << splitPoints.front();
//	log() << "heejin__ found-back : " << splitPoints.back();
//heejin) this part call splitChunkAtMultiplePoints
        const auto suggestedMigrateChunk =
            uassertStatusOK(shardutil::splitChunkAtMultiplePoints(opCtx,
                                                                  chunk->getShardId(),
                                                                  nss,
                                                                  manager->getShardKeyPattern(),
                                                                  manager->getVersion(),
                                                                  chunkRange,
                                                                  splitPoints));

	//global_split++;
	//log() << "jin!! real global split " << global_split;
        // Balance the resulting chunks if the option is enabled and if the shard suggested a chunk
        // to balance
        const bool shouldBalance = [&]() {
            if (!balancerConfig->shouldBalanceForAutoSplit())
                return false;

            auto collStatus =
                Grid::get(opCtx)->catalogClient()->getCollection(opCtx, manager->getns());
            if (!collStatus.isOK()) {
                log() << "Auto-split for " << nss << " failed to load collection metadata"
                      << causedBy(redact(collStatus.getStatus()));
                return false;
            }

            return collStatus.getValue().value.getAllowBalance();
        }();

//	log() << "heejin found-front : " << splitPoints.front();
//	log() << "heejin found-back : " << splitPoints.back();
        log() << "autosplitted " << nss << " chunk: " << redact(chunk->toString()) << " into "
              << (splitPoints.size() + 1) << " parts (desiredChunkSize " << desiredChunkSize << ")"
              << (suggestedMigrateChunk ? "" : (std::string) " (migrate suggested" +
                          (shouldBalance ? ")" : ", but no migrations allowed)"));

        // Reload the chunk manager after the split
        auto routingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));

        if (!shouldBalance || !suggestedMigrateChunk) {
            return;
        }

        // Top chunk optimization - try to move the top chunk out of this shard to prevent the hot
        // spot from staying on a single shard. This is based on the assumption that succeeding
        // inserts will fall on the top chunk.

        // We need to use the latest chunk manager (after the split) in order to have the most
        // up-to-date view of the chunk we are about to move
        auto suggestedChunk = routingInfo.cm()->findIntersectingChunkWithSimpleCollation(
            suggestedMigrateChunk->getMin());

        ChunkType chunkToMove;
        chunkToMove.setNS(nss.ns());
        chunkToMove.setShard(suggestedChunk->getShardId());
        chunkToMove.setMin(suggestedChunk->getMin());
        chunkToMove.setMax(suggestedChunk->getMax());
        chunkToMove.setVersion(suggestedChunk->getLastmod());

        uassertStatusOK(configsvr_client::rebalanceChunk(opCtx, chunkToMove));

        // Ensure the collection gets reloaded because of the move
        Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss);
    } catch (const DBException& ex) {
        chunk->clearBytesWritten();

        if (ErrorCodes::isStaleShardingError(ex.code())) {
            log() << "Unable to auto-split chunk " << redact(chunkRange.toString()) << causedBy(ex)
                  << ", going to invalidate routing table entry for " << nss;
            Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss);
        }
    }
}

}  // namespace mongo
