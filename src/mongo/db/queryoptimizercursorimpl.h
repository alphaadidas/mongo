// @file queryoptimizercursorimpl.h - A cursor interleaving multiple candidate cursors.

/**
 *    Copyright (C) 2011 10gen Inc.
 *    Copyright (C) 2013 Tokutek Inc.
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

#pragma once

#include "mongo/db/queryutil.h"
#include "mongo/db/queryoptimizercursor.h"

namespace mongo {
    
    /** Helper class for caching and counting matches during execution of a QueryPlan. */
    class CachedMatchCounter {
    public:
        /**
         * @param aggregateNscanned - shared count of nscanned for this and othe plans.
         * @param cumulativeCount - starting point for accumulated count over a series of plans.
         */
        CachedMatchCounter( long long &aggregateNscanned, int cumulativeCount ) : _aggregateNscanned( aggregateNscanned ), _nscanned(), _cumulativeCount( cumulativeCount ), _count(), _checkDups(), _match( Unknown ), _counted() {}
        
        /** Set whether dup checking is enabled when counting. */
        void setCheckDups( bool checkDups ) { _checkDups = checkDups; }
        
        /**
         * Usual sequence of events:
         * 1) resetMatch() - reset stored match value to Unkonwn.
         * 2) setMatch() - set match value to a definite true/false value.
         * 3) knowMatch() - check if setMatch() has been called.
         * 4) countMatch() - increment count if match is true.
         */
        
        void resetMatch() {
            _match = Unknown;
            _counted = false;
        }
        /** @return true if the match was not previously recorded. */
        bool setMatch( bool match ) {
            MatchState oldMatch = _match;
            _match = match ? True : False;
            return _match == True && oldMatch != True;
        }
        bool knowMatch() const { return _match != Unknown; }
        void countMatch( const BSONObj &pk ) {
            if ( !_counted && _match == True && !getsetdup( pk ) ) {
                ++_cumulativeCount;
                ++_count;
                _counted = true;
            }
        }
        bool wouldCountMatch( const BSONObj &pk ) const {
            return !_counted && _match == True && !getdup( pk );
        }

        bool enoughCumulativeMatchesToChooseAPlan() const {
            // This is equivalent to the default condition for switching from
            // a query to a getMore, which was the historical default match count for
            // choosing a plan.
            return _cumulativeCount >= 101;
        }
        bool enoughMatchesToRecordPlan() const {
            // Recording after 50 matches is a historical default (101 default limit / 2).
            return _count > 50;
        }

        int cumulativeCount() const { return _cumulativeCount; }
        int count() const { return _count; }
        
        /** Update local and aggregate nscanned counts. */
        void updateNscanned( long long nscanned ) {
            _aggregateNscanned += ( nscanned - _nscanned );
            _nscanned = nscanned;
        }
        long long nscanned() const { return _nscanned; }
        long long &aggregateNscanned() const { return _aggregateNscanned; }
    private:
        bool getsetdup( const BSONObj &pk ) {
            if ( !_checkDups ) {
                return false;
            }
            // We need a copy of the PK because we don't own the caller's BSON
            pair<set<BSONObj>::iterator, bool> p = _dups.insert( pk.copy() );
            return !p.second;
        }
        bool getdup( const BSONObj &pk ) const {
            if ( !_checkDups ) {
                return false;
            }
            return _dups.find( pk ) != _dups.end();
        }
        long long &_aggregateNscanned;
        long long _nscanned;
        int _cumulativeCount;
        int _count;
        bool _checkDups;
        enum MatchState { Unknown, False, True };
        MatchState _match;
        bool _counted;
        set<BSONObj> _dups;
    };
    
    /** Dup tracking class, optimizing one common case with small set and few initial reads. */
    class SmallDupSet {
    public:
        SmallDupSet() : _accesses() {
            _vec.reserve( 250 );
        }
        /** @return true if @param 'pk' already added to the set, false if adding to the set in this call. */
        bool getsetdup( const BSONObj &pk ) {
            access();
            return vec() ? getsetdupVec( pk.copy() ) : getsetdupSet( pk.copy() );
        }
        /** @return true when @param pk in the set. */
        bool getdup( const BSONObj &pk ) {
            access();
            return vec() ? getdupVec( pk ) : getdupSet( pk );
        }            
    private:
        void access() {
            ++_accesses;
            mayUpgrade();
        }
        void mayUpgrade() {
            if ( vec() && _accesses > 500 ) {
                _set.insert( _vec.begin(), _vec.end() );
            }
        }
        bool vec() const {
            return _set.size() == 0;
        }
        bool getsetdupVec( const BSONObj &pk ) {
            if ( getdupVec( pk ) ) {
                return true;
            }
            _vec.push_back( pk );
            return false;
        }
        bool getdupVec( const BSONObj &pk ) const {
            for( vector<BSONObj>::const_iterator i = _vec.begin(); i != _vec.end(); ++i ) {
                if ( *i == pk ) {
                    return true;
                }
            }
            return false;
        }
        bool getsetdupSet( const BSONObj &pk ) {
            pair<set<BSONObj>::iterator, bool> p = _set.insert(pk);
            return !p.second;
        }
        bool getdupSet( const BSONObj &pk ) {
            return _set.count( pk ) > 0;
        }
        vector<BSONObj> _vec;
        set<BSONObj> _set;
        long long _accesses;
    };
    
    class QueryPlanSummary;
    class MultiPlanScanner;
    
    /**
     * Helper class for generating a simple Cursor or QueryOptimizerCursor from a set of query
     * parameters.  This class was refactored from a single function call and is not expected to
     * outlive its constructor arguments.
     */
    class CursorGenerator {
    public:
        CursorGenerator( const StringData& ns,
                        const BSONObj &query,
                        const BSONObj &order,
                        const QueryPlanSelectionPolicy &planPolicy,
                        bool *simpleEqualityMatch,
                        const shared_ptr<const ParsedQuery> &parsedQuery,
                        bool requireOrder,
                        QueryPlanSummary *singlePlanSummary );
        
        shared_ptr<Cursor> generate();
        
    private:
        bool snapshot() const { return _parsedQuery && _parsedQuery->isSnapshot(); }
        bool explain() const { return _parsedQuery && _parsedQuery->isExplain(); }
        BSONObj min() const { return _parsedQuery ? _parsedQuery->getMin() : BSONObj(); }
        BSONObj max() const { return _parsedQuery ? _parsedQuery->getMax() : BSONObj(); }
        bool hasFields() const { return _parsedQuery && _parsedQuery->getFieldPtr(); }
        
        bool isOrderRequired() const { return _requireOrder; }
        bool mayShortcutQueryOptimizer() const {
            return min().isEmpty() && max().isEmpty() && !hasFields() && _argumentsHint.isEmpty();
        }
        BSONObj hint() const {
            return _argumentsHint.isEmpty() ? _planPolicy.planHint( _ns ) : _argumentsHint;
        }
        
        void setArgumentsHint();
        shared_ptr<Cursor> shortcutCursor() const;
        void setMultiPlanScanner();
        shared_ptr<Cursor> singlePlanCursor();
        
        const StringData _ns;
        BSONObj _query;
        BSONObj _order;
        const QueryPlanSelectionPolicy &_planPolicy;
        bool *_simpleEqualityMatch;
        shared_ptr<const ParsedQuery> _parsedQuery;
        bool _requireOrder;
        QueryPlanSummary *_singlePlanSummary;
        
        BSONObj _argumentsHint;
        auto_ptr<MultiPlanScanner> _mps;
    };
    
} // namespace mongo
