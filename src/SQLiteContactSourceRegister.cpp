/*
 * Copyright (C) 2008 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "SQLiteContactSource.h"
#include "SyncEvolutionUtil.h"

static EvolutionSyncSource *createSource(const EvolutionSyncSourceParams &params)
{
    pair <string, string> sourceType = EvolutionSyncSource::getSourceType(params.m_nodes);
    bool isMe = sourceType.first == "SQLite Address Book";

#ifndef ENABLE_SQLITE
    return isMe ? RegisterSyncSource::InactiveSource : NULL;
#else
    bool maybeMe = sourceType.first == "addressbook";
    
    if (isMe || maybeMe) {
        if (sourceType.second == "" || sourceType.second == "text/x-vcard") {
            return new SQLiteContactSource(params);
        } else {
            return NULL;
        }
    }
    return NULL;
#endif
}

static RegisterSyncSource registerMe("SQLite Address Book",
#ifdef ENABLE_SQLITE
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "SQLite Address Book = addressbook = contacts = sqlite-contacts\n"
                                     "   vCard 2.1 (default) = text/x-vcard\n",
                                     Values() +
                                     (Aliases("SQLite Address Book") + "sqlite-contacts" + "sqlite"));

#ifdef ENABLE_SQLITE
#ifdef ENABLE_UNIT_TESTS

class EvolutionSQLiteContactsTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(EvolutionSQLiteContactsTest);
    CPPUNIT_TEST(testInstantiate);
    CPPUNIT_TEST_SUITE_END();

protected:
    void testInstantiate() {
        boost::shared_ptr<EvolutionSyncSource> source;
        source.reset(EvolutionSyncSource::createTestingSource("contacts", "contacts", true));
        source.reset(EvolutionSyncSource::createTestingSource("contacts", "addressbook", true));
        source.reset(EvolutionSyncSource::createTestingSource("contacts", "sqlite-contacts", true));
        source.reset(EvolutionSyncSource::createTestingSource("contacts", "SQLite Address Book:text/x-vcard", true));
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(EvolutionSQLiteContactsTest);

#endif // ENABLE_UNIT_TESTS
#endif // ENABLE_SQLITE
