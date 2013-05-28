/**
 *    Copyright (C) 2012 10gen Inc.
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
 */

#include "mongo/pch.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs_sync.h"

namespace mongo {
    void incRBID();
    BackgroundSync* BackgroundSync::s_instance = 0;
    boost::mutex BackgroundSync::s_mutex;


    BackgroundSync::BackgroundSync() : _opSyncShouldRun(false),
                                            _opSyncRunning(false),
                                            _currentSyncTarget(NULL),
                                            _opSyncShouldExit(false),
                                            _opSyncInProgress(false),
                                            _applierShouldExit(false),
                                            _applierInProgress(false)
    {
    }

    BackgroundSync::QueueCounter::QueueCounter() : waitTime(0) {
    }

    BackgroundSync* BackgroundSync::get() {
        boost::unique_lock<boost::mutex> lock(s_mutex);
        if (s_instance == NULL && !inShutdown()) {
            s_instance = new BackgroundSync();
        }
        return s_instance;
    }

    BSONObj BackgroundSync::getCounters() {
        BSONObjBuilder counters;
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            counters.appendIntOrLL("waitTimeMs", _queueCounter.waitTime);
            uint32_t size = _deque.size();
            counters.append("numElems", size);
        }
        return counters.obj();
    }

    void BackgroundSync::shutdown() {
        // first get producer thread to exit
        log() << "trying to shutdown bgsync" << rsLog;
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            _opSyncShouldExit = true;
            _opSyncShouldRun = false;
            _opSyncCanRunCondVar.notify_all();
        }
        // this does not need to be efficient
        // just sleep for periods of one second
        // until we see that we are no longer running 
        // the opSync thread
        log() << "waiting for opSync thread to end" << rsLog;
        while (_opSyncInProgress) {
            sleepsecs(1);
            log() << "still waiting for opSync thread to end... " << rsLog;
        }

        // at this point, the opSync thread should be done
        _queueCond.notify_all();
        
        // now get applier thread to exit
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            _applierShouldExit = true;
            _queueCond.notify_all();
        }
        // same reasoning as with _opSyncInProgress above
        log() << "waiting for applier thread to end" << rsLog;
        while (_applierInProgress) {
            sleepsecs(1);
            log() << "still waiting for applier thread to end..." << rsLog;
        }
        log() << "shutdown of bgsync complete" << rsLog;
    }

    void BackgroundSync::applierThread() {
        _applierInProgress = true;
        Client::initThread("applier");
        replLocalAuth();
        applyOpsFromOplog();
        cc().shutdown();
        _applierInProgress = false;
    }

    void BackgroundSync::applyOpsFromOplog() {
        GTID lastLiveGTID;
        GTID lastUnappliedGTID;
        while (1) {
            try {
                BSONObj curr;
                {
                    boost::unique_lock<boost::mutex> lck(_mutex);
                    // wait until we know an item has been produced
                    while (_deque.size() == 0 && !_applierShouldExit) {
                        _queueDone.notify_all();
                        _queueCond.wait(_mutex);
                    }
                    if (_deque.size() == 0 && _applierShouldExit) {
                        return; 
                    }
                    curr = _deque.front();
                }
                GTID currEntry = getGTIDFromOplogEntry(curr);
                theReplSet->gtidManager->noteApplyingGTID(currEntry);
                applyTransactionFromOplog(curr);

                {
                    boost::unique_lock<boost::mutex> lck(_mutex);
                    // I don't recall if noteGTIDApplied needs to be called within _mutex
                    theReplSet->gtidManager->noteGTIDApplied(currEntry);
                    dassert(_deque.size() > 0);
                    _deque.pop_front();
                    
                    // this is a flow control mechanism, with bad numbers
                    // hard coded for now just to get something going.
                    // If the opSync thread notices that we have over 20000
                    // transactions in the queue, it waits until we get below
                    // 10000. This is where we signal that we have gotten there
                    // Once we have spilling of transactions working, this
                    // logic will need to be redone
                    if (_deque.size() == 10000) {
                        _queueCond.notify_all();
                    }
                }
            }
            catch (DBException& e) {
                sethbmsg(str::stream() << "db exception in producer on applier thread: " << e.toString());
                sleepsecs(2);
            }
            catch (std::exception& e2) {
                sethbmsg(str::stream() << "exception in producer on applier thread: " << e2.what());
                sleepsecs(2);
            }
        }
    }
    
    void BackgroundSync::producerThread() {
        _opSyncInProgress = true;
        Client::initThread("rsBackgroundSync");
        replLocalAuth();
        uint32_t timeToSleep = 0;

        while (!_opSyncShouldExit) {
            try {
                if (timeToSleep) {
                    {
                        boost::unique_lock<boost::mutex> lck(_mutex);
                        _opSyncRunning = false;
                        // notify other threads that we are not running
                        _opSyncRunningCondVar.notify_all();
                    }
                    for (uint32_t i = 0; i < timeToSleep; i++) {
                        sleepsecs(1);
                        // get out if we need to
                        if (_opSyncShouldExit) { break; }
                    }
                    timeToSleep = 0;
                }
                // get out if we need to
                if (_opSyncShouldExit) { break; }

                {
                    boost::unique_lock<boost::mutex> lck(_mutex);
                    _opSyncRunning = false;

                    while (!_opSyncShouldRun && !_opSyncShouldExit) {
                        // notify other threads that we are not running
                        _opSyncRunningCondVar.notify_all();
                        // wait for permission that we can run
                        _opSyncCanRunCondVar.wait(lck);
                    }

                    // notify other threads that we are running
                    _opSyncRunningCondVar.notify_all();
                    _opSyncRunning = true;
                }
                // get out if we need to
                if (_opSyncShouldExit) { break; }

                MemberState state = theReplSet->state();
                if (state.fatal() || state.startup()) {
                    timeToSleep = 5;
                    continue;
                }
                // this does the work of reading a remote oplog
                // and writing it to our oplog
                timeToSleep = produce();
            }
            catch (DBException& e) {
                sethbmsg(str::stream() << "db exception in producer: " << e.toString());
                timeToSleep = 10;
            }
            catch (std::exception& e2) {
                sethbmsg(str::stream() << "exception in producer: " << e2.what());
                timeToSleep = 10;
            }
        }

        cc().shutdown();
        _opSyncRunning = false; // this is racy, but who cares, we are shutting down
        _opSyncInProgress = false;
    }

    void BackgroundSync::handleSlaveDelay(uint64_t opTimestamp) {
        dassert(_opSyncRunning);
        uint64_t slaveDelayMillis = theReplSet->myConfig().slaveDelay * 1000;
        uint64_t currTime = curTimeMillis64();
        uint64_t timeOpShouldBeApplied = opTimestamp + slaveDelayMillis;
        while (currTime < timeOpShouldBeApplied) {
            uint64_t sleepTime = (timeOpShouldBeApplied - currTime);
            // let's sleep for at most one second
            sleepmillis((sleepTime < 1000) ? sleepTime : 1000);        
            // check if we should bail out, as we don't want to 
            // sleep the whole time possibly long delay time
            // if we see we should be stopping
            {
                boost::unique_lock<boost::mutex> lck(_mutex);
                if (!_opSyncShouldRun) {
                    break;
                }
            }
            // reset currTime
            currTime = curTimeMillis64();
        }
    }

    // returns number of seconds to sleep, if any
    uint32_t BackgroundSync::produce() {

        // normally msgCheckNewState gets called periodically, but in a single node repl set
        // there are no heartbeat threads, so we do it here to be sure.  this is relevant if the
        // singleton member has done a stepDown() and needs to come back up.
        if (theReplSet->config().members.size() == 1 &&
            theReplSet->myConfig().potentiallyHot()) {
            Manager* mgr = theReplSet->mgr;
            // When would mgr be null?  During replsettest'ing, in which case we should
            // fall through and actually apply ops as if we were a real secondary.
            if (mgr) {
                mgr->send(boost::bind(&Manager::msgCheckNewState, theReplSet->mgr));
                // There should never be ops to sync in a 1-member set, anyway
                return 1;
            }
        }

        OplogReader r(true /* doHandshake */);

        // find a target to sync from the last op time written
        getOplogReader(r);

        // no server found
        GTID lastGTIDFetched = theReplSet->gtidManager->getLiveState();
        {
            boost::unique_lock<boost::mutex> lock(_mutex);

            if (_currentSyncTarget == NULL) {
                // if there is no one to sync from
                return 1; //sleep one second
            }
            r.tailingQueryGTE(rsoplog, lastGTIDFetched);
        }

        // if target cut connections between connecting and querying (for
        // example, because it stepped down) we might not have a cursor
        if (!r.haveCursor()) {
            return 0;
        }

        try {
            // this method may actually run rollback, yes, the name is bad
            if (isRollbackRequired(r)) {
                // sleep 2 seconds and try again. (The 2 is arbitrary).
                // If we are not fatal, then we will keep trying to sync
                // from another machine
                return 2;
            }
        }
        catch (RollbackOplogException& re){
            // we attempted a rollback and failed, we must go fatal.            
            theReplSet->fatal();
        }

        while (!_opSyncShouldExit) {
            while (!_opSyncShouldExit) {
                {
                    // check if we should bail out
                    boost::unique_lock<boost::mutex> lck(_mutex);
                    if (!_opSyncShouldRun) {
                        return 0;
                    }
                }
                if (!r.moreInCurrentBatch()) {
                    // check to see if we have a request to sync
                    // from a specific target. If so, get out so that
                    // we can restart the act of syncing and
                    // do so from the correct target
                    if (theReplSet->gotForceSync()) {
                        return 0;
                    }

                    verify(!theReplSet->isPrimary());

                    {
                        boost::unique_lock<boost::mutex> lock(_mutex);
                        if (!_currentSyncTarget || !_currentSyncTarget->hbinfo().hbstate.readable()) {
                            return 0;
                        }
                    }

                    r.more();
                }

                if (!r.more()) {
                    break;
                }

                // This is the operation we have received from the target
                // that we must put in our oplog with an applied field of false
                BSONObj o = r.nextSafe().getOwned();
                log() << "replicating " << o.toString(false, true) << " from " << _currentSyncTarget->fullName() << endl;
                uint64_t ts = o["ts"]._numberLong();

                // now that we have the element in o, let's check
                // if there a delay is required (via slaveDelay) before
                // writing it to the oplog
                if (theReplSet->myConfig().slaveDelay > 0) {
                    handleSlaveDelay(ts);
                    {
                        boost::unique_lock<boost::mutex> lck(_mutex);
                        if (!_opSyncShouldRun) {
                            break;
                        }
                    }
                }
                
                Timer timer;
                {
                    Client::ReadContext ctx(rsoplog);
                    Client::Transaction transaction(DB_SERIALIZABLE);
                    replicateTransactionToOplog(o);                    
                    // we are operating as a secondary. We don't have to fsync
                    transaction.commit(DB_TXN_NOSYNC);
                }
                {
                    GTID currEntry = getGTIDFromOplogEntry(o);
                    uint64_t lastHash = o["h"].numberLong();
                    boost::unique_lock<boost::mutex> lock(_mutex);
                    // update counters
                    theReplSet->gtidManager->noteGTIDAdded(currEntry, ts, lastHash);
                    _queueCounter.waitTime += timer.millis();
                    // notify applier thread that data exists
                    if (_deque.size() == 0) {
                        _queueCond.notify_all();
                    }
                    _deque.push_back(o);
                    // this is a flow control mechanism, with bad numbers
                    // hard coded for now just to get something going.
                    // If the opSync thread notices that we have over 20000
                    // transactions in the queue, it waits until we get below
                    // 10000. This is where we wait if we get too high
                    // Once we have spilling of transactions working, this
                    // logic will need to be redone
                    if (_deque.size() > 20000) {
                        _queueCond.wait(lock);
                    }
                }
            } // end while

            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                if (!_currentSyncTarget || !_currentSyncTarget->hbinfo().hbstate.readable()) {
                    return 0;
                }
            }


            r.tailCheck();
            if( !r.haveCursor() ) {
                LOG(1) << "replSet end opSync pass" << rsLog;
                return 0;
            }

            // looping back is ok because this is a tailable cursor
        }
        return 0;
    }

    bool BackgroundSync::isStale(OplogReader& r, BSONObj& remoteOldestOp) {
        remoteOldestOp = r.findOne(rsoplog, Query());
        GTID remoteOldestGTID = getGTIDFromBSON("_id", remoteOldestOp);
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            GTID currLiveState = theReplSet->gtidManager->getLiveState();
            if (GTID::cmp(currLiveState, remoteOldestGTID) < 0) {
                return true;
            }
        }
        return false;
    }

    void BackgroundSync::getOplogReader(OplogReader& r) {
        Member *target = NULL, *stale = NULL;
        BSONObj oldest;

        verify(r.conn() == NULL);
        while ((target = theReplSet->getMemberToSyncTo()) != NULL) {
            string current = target->fullName();

            if (!r.connect(current)) {
                LOG(2) << "replSet can't connect to " << current << " to read operations" << rsLog;
                r.resetConnection();
                theReplSet->veto(current);
                continue;
            }

            if (isStale(r, oldest)) {
                r.resetConnection();
                theReplSet->veto(current, 600);
                stale = target;
                continue;
            }

            // if we made it here, the target is up and not stale
            {
                boost::unique_lock<boost::mutex> lock(_mutex);
                _currentSyncTarget = target;
            }

            return;
        }

        // the only viable sync target was stale
        if (stale) {
            GTID remoteOldestGTID = getGTIDFromBSON("_id", oldest);
            theReplSet->goStale(stale, remoteOldestGTID);
            // vanilla Mongo used to do a sleep of 120 seconds here
            // We removed it. It seems excessive, and if this machine is doing
            // nothing anyway, sleeping won't help. It might as well
            // return with a null sync target, and produce() will handle
            // that fact and sleep one second
        }

        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            _currentSyncTarget = NULL;
        }
    }

    void BackgroundSync::runRollback(OplogReader& r, uint64_t oplogTS) {
        // starting from ourLast, we need to read the remote oplog
        // backwards until we find an entry in the remote oplog
        // that has the same GTID, timestamp, and hash as
        // what we have in our oplog. If we don't find one that is within
        // some reasonable timeframe, then we go fatal
        GTID ourLast = theReplSet->gtidManager->getLiveState();
        GTID idToRollbackTo;
        uint64_t rollbackPointTS;
        uint64_t rollbackPointHash;
        incRBID();
        try {
            shared_ptr<DBClientCursor> rollbackCursor = r.getRollbackCursor(ourLast);
            while (rollbackCursor->more()) {
                BSONObj remoteObj = rollbackCursor->next();
                GTID remoteGTID = getGTIDFromBSON("_id", remoteObj);
                uint64_t remoteTS = remoteObj["ts"]._numberLong();
                if (oplogTS == 0) {
                    // in case we are rolling back due to not finding an element,
                    // see isRollbackRequired
                    oplogTS = remoteTS;
                }
                uint64_t remoteLastHash = remoteObj["h"].numberLong();
                if (remoteTS + 1800*1000 < oplogTS) {
                    throw RollbackOplogException("replSet rollback too long a time period for a rollback (at least 30 minutes).");
                    break;
                }
                //now try to find an entry in our oplog with that GTID
                BSONObjBuilder localQuery;
                BSONObj localObj;
                addGTIDToBSON("_id", remoteGTID, localQuery);
                bool foundLocally = false;
                {
                    Client::ReadContext ctx(rsoplog);
                    Client::Transaction transaction(DB_SERIALIZABLE);
                    foundLocally = Helpers::findOne( rsoplog, localQuery.done(), localObj);
                }
                if (foundLocally) {
                    GTID localGTID = getGTIDFromBSON("_id", localObj);
                    uint64_t localTS = localObj["ts"]._numberLong();
                    uint64_t localLastHash = localObj["h"].numberLong();
                    if (localLastHash == remoteLastHash &&
                        localTS == remoteTS &&
                        GTID::cmp(localGTID, remoteGTID) == 0
                        )
                    {
                        log() << "found id to rollback to " << idToRollbackTo << rsLog;
                        idToRollbackTo = localGTID;
                        rollbackPointTS = localTS;
                        rollbackPointHash = localLastHash;
                        break;
                    }
                }
            }
            // At this point, either we have found the point to try to rollback to,
            // or we have determined that we cannot rollback
            if (idToRollbackTo.isInitial()) {
                // we cannot rollback
                throw RollbackOplogException("could not find ID to rollback to");
            }
        }
        catch (DBException& e) {
            throw RollbackOplogException("DBException while trying to find ID to rollback to: " + e.toString());
        }
        catch (std::exception& e2) {
            throw RollbackOplogException(str::stream() << "Exception while trying to find ID to rollback to: " << e2.what());
        }

        // proceed with the rollback to point idToRollbackTo
        // probably ought to grab a global write lock while doing this
        // I don't think we want oplog cursors reading from this machine
        // while we are rolling back. Or at least do something to protect against this

        // first, let's get all the operations that are being applied out of the way,
        // we don't want to rollback an item in the oplog while simultaneously,
        // the applier thread is applying it to the oplog
        {
            boost::unique_lock<boost::mutex> lock(_mutex);
            while (_deque.size() > 0) {
                log() << "waiting for applier to finish work before doing rollback " << rsLog;
                _queueDone.wait(lock);
            }
            verifySettled();
        }

        // now let's tell the system we are going to rollback, to do so,
        // abort live multi statement transactions, invalidate cursors, and
        // change the state to RS_ROLLBACK
        {
            rwlock(multiStmtTransactionLock, true);
            // so we know writes are not simultaneously occurring
            Lock::GlobalWrite lk;
            ClientCursor::invalidateAllCursors();
            Client::abortLiveTransactions();
            theReplSet->goToRollbackState();
        }

        try {
            // now that we are settled, we have to take care of the GTIDManager
            // and the repl info thread.
            // We need to reset the state of the GTIDManager to the point
            // we intend to rollback to, and we need to make sure that the repl info thread
            // has captured this information.
            theReplSet->gtidManager->resetAfterInitialSync(
                idToRollbackTo,
                rollbackPointTS,
                rollbackPointHash
                );
            // now force an update of the repl info thread
            theReplSet->forceUpdateReplInfo();

            // at this point, everything should be settled, the applier should
            // have nothing left (and remain that way, because this is the only
            // thread that can put work on the applier). Now we can rollback
            // the data.
            while (true) {
                BSONObj o;
                {
                    Lock::DBRead lk(rsoplog);
                    Client::Transaction txn(DB_SERIALIZABLE);
                    // if there is nothing in the oplog, break
                    if( !Helpers::getLast(rsoplog, o) ) {
                        break;
                    }
                }
                GTID lastGTID = getGTIDFromBSON("_id", o);
                // if we have rolled back enough, break from while loop
                if (GTID::cmp(lastGTID, idToRollbackTo) <= 0) {
                    dassert(GTID::cmp(lastGTID, idToRollbackTo) == 0);
                    break;
                }
                rollbackTransactionFromOplog(o);
            }
            theReplSet->leaveRollbackState();
        }
        catch (DBException& e) {
            throw RollbackOplogException("DBException while trying to run rollback: " + e.toString());
        }
        catch (std::exception& e2) {
            throw RollbackOplogException(str::stream() << "Exception while trying to run rollback: " << e2.what());
        }
        
    }

    bool BackgroundSync::isRollbackRequired(OplogReader& r) {
        string hn = r.conn()->getServerAddress();
        if (!r.more()) {
            // In vanilla Mongo, this happened for one of the
            // following reasons:
            //  - we were ahead of what we are syncing from (don't
            //    think that is possible anymore)
            //  - remote oplog is empty for some weird reason
            // in either case, if it (strangely) happens, we'll just return
            // and our caller will simply try again after a short sleep.
            log() << "replSet error empty query result from " << hn << " oplog, attempting rollback" << rsLog;
            runRollback(r, ts);
            return true;
        }

        BSONObj o = r.nextSafe();
        uint64_t ts = o["ts"]._numberLong();
        uint64_t lastHash = o["h"].numberLong();
        GTID gtid = getGTIDFromBSON("_id", o);

        if( !theReplSet->gtidManager->rollbackNeeded(gtid, ts, lastHash)) {
            log() << "Rollback NOT needed! Our GTID" << gtid << endl;
            return false;
        }

        log() << "Rollback needed! Our GTID" <<
            theReplSet->gtidManager->getLiveState().toString() <<
            " remote GTID: " << gtid.toString() << ". Attempting rollback." << rsLog;

        runRollback(r, ts);
        return true;
    }

    Member* BackgroundSync::getSyncTarget() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        return _currentSyncTarget;
    }

    // does some sanity checks before finishing starting and stopping the opsync 
    // thread that we are in a decent state
    //
    // called with _mutex held
    void BackgroundSync::verifySettled() {
        verify(_deque.size() == 0);
        // do a sanity check on the GTID Manager
        GTID lastLiveGTID;
        GTID lastUnappliedGTID;
        theReplSet->gtidManager->getLiveGTIDs(
            &lastLiveGTID, 
            &lastUnappliedGTID
            );
        verify(GTID::cmp(lastUnappliedGTID, lastLiveGTID) == 0);

        GTID minLiveGTID;
        GTID minUnappliedGTID;
        theReplSet->gtidManager->getMins(
            &minLiveGTID, 
            &minUnappliedGTID
            );
        verify(GTID::cmp(minUnappliedGTID, minLiveGTID) == 0);
        log() << "GTIDs: " << 
            lastLiveGTID.toString() << " " << 
            lastUnappliedGTID.toString() << " " << 
            minLiveGTID.toString() << " " <<
            minUnappliedGTID.toString() << rsLog;
    }

    void BackgroundSync::stopOpSyncThread() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        _opSyncShouldRun = false;
        while (_opSyncRunning) {
            _opSyncRunningCondVar.wait(lock);
        }
        // sanity checks
        verify(!_opSyncShouldRun);

        // wait for all things to be applied
        while (_deque.size() > 0) {
            _queueDone.wait(lock);
        }

        verifySettled();
    }

    void BackgroundSync::startOpSyncThread() {
        boost::unique_lock<boost::mutex> lock(_mutex);
        verifySettled();

        _opSyncShouldRun = true;
        _opSyncCanRunCondVar.notify_all();
        while (!_opSyncRunning) {
            _opSyncRunningCondVar.wait(lock);
        }
        // sanity check that no one has changed this variable
        verify(_opSyncShouldRun);
    }

} // namespace mongo
