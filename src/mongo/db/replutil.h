/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/database.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/db/repl.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/repl/rs.h"

namespace mongo {

    extern const char *replAllDead;

    /* note we always return true for the "local" namespace.

       we should not allow most operations when not the master
       also we report not master if we are "dead".

       See also CmdIsMaster.

       If 'client' is not specified, the current client is used.
    */
    inline bool _isMaster() {
        if( replSet ) {
            if( theReplSet )
                return theReplSet->isPrimary();
            return false;
        }
        return true;
    }
    inline bool isMaster(const char * dbname = 0) {
        if( _isMaster() )
            return true;
        if ( ! dbname ) {
            Database *database = cc().database();
            verify( database );
            dbname = database->name().c_str();
        }
        return strcmp( dbname , "local" ) == 0;
    }
    inline bool isMasterNs( const char *ns ) {
        if ( _isMaster() )
            return true;
        verify( ns );
        if ( ! str::startsWith( ns , "local" ) )
            return false;
        return ns[5] == 0 || ns[5] == '.';
    }

    inline void notMasterUnless(bool expr) {
        uassert( 10107 , "not master" , expr );
    }

    class ParsedQuery;
    
    void replVerifyReadsOk(const ParsedQuery* pq = 0);

} // namespace mongo
