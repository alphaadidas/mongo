// delete.h

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

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class NamespaceDetails;
    class NamespaceDetailsTransient;

    void deleteOneObject(NamespaceDetails *details, NamespaceDetailsTransient *nsdt,
                         const BSONObj &pk, const BSONObj &obj, uint64_t flags = 0);

    // System-y version of deleteObjects that allows you to delete from the system collections, used to be god = true.
    long long _deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop);

    // If justOne is true, deletedId is set to the id of the deleted object.
    long long deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop = false);

}
