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

#include "EvolutionContactSource.h"
#include "SyncEvolutionUtil.h"

static EvolutionSyncSource *createSource(const EvolutionSyncSourceParams &params)
{
    pair <string, string> sourceType = EvolutionSyncSource::getSourceType(params.m_nodes);
    bool isMe = sourceType.first == "Evolution Address Book";

#ifndef ENABLE_EBOOK
    return isMe ? RegisterSyncSource::InactiveSource : NULL;
#else
    bool maybeMe = sourceType.first == "addressbook";
    
    if (isMe || maybeMe) {
        if (sourceType.second == "" || sourceType.second == "text/x-vcard") {
            return new EvolutionContactSource(params, EVC_FORMAT_VCARD_21);
        } else if (sourceType.second == "text/vcard") {
            return new EvolutionContactSource(params, EVC_FORMAT_VCARD_30);
        }
    }
    return NULL;
#endif
}

static RegisterSyncSource registerMe("Evolution Address Book",
#ifdef ENABLE_EBOOK
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "Evolution Address Book = Evolution Contacts = addressbook = contacts = evolution-contacts\n"
                                     "   vCard 2.1 (default) = text/x-vcard\n"
                                     "   vCard 3.0 = text/vcard\n"
                                     "   The later is the internal format of Evolution and preferred with\n"
                                     "   servers that support it. One such server is ScheduleWorld\n"
                                     "   together with the \"card3\" uri.\n",
                                     Values() +
                                     (Aliases("Evolution Address Book") + "Evolution Contacts" + "evolution-contacts"));

#ifdef ENABLE_EBOOK
#ifdef ENABLE_UNIT_TESTS

class EvolutionContactTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(EvolutionContactTest);
    CPPUNIT_TEST(testInstantiate);
    CPPUNIT_TEST(testImport);
    CPPUNIT_TEST_SUITE_END();

protected:
    void testInstantiate() {
        boost::shared_ptr<EvolutionSyncSource> source;
        source.reset(EvolutionSyncSource::createTestingSource("addressbook", "addressbook", true));
        source.reset(EvolutionSyncSource::createTestingSource("addressbook", "contacts", true));
        source.reset(EvolutionSyncSource::createTestingSource("addressbook", "evolution-contacts", true));
        source.reset(EvolutionSyncSource::createTestingSource("addressbook", "Evolution Contacts", true));
        source.reset(EvolutionSyncSource::createTestingSource("addressbook", "Evolution Address Book:text/x-vcard", true));
        source.reset(EvolutionSyncSource::createTestingSource("addressbook", "Evolution Address Book:text/vcard", true));
    }

    /**
     * Tests parsing of contacts as they might be send by certain servers.
     * This complements the actual testing with real servers and might cover
     * cases not occurring with servers that are actively tested against.
     */
    void testImport() {
        // this only tests that we can instantiate something under the type "addressbook";
        // it might not be an EvolutionContactSource
        boost::shared_ptr<EvolutionContactSource> source21(dynamic_cast<EvolutionContactSource *>(EvolutionSyncSource::createTestingSource("evolutioncontactsource21", "evolution-contacts:text/x-vcard", true)));
        boost::shared_ptr<EvolutionContactSource> source30(dynamic_cast<EvolutionContactSource *>(EvolutionSyncSource::createTestingSource("evolutioncontactsource30", "Evolution Address Book:text/vcard", true)));
        string parsed;

        // SF bug 1796086: sync with EGW: lost or messed up telephones
        parsed = "BEGIN:VCARD\r\nVERSION:3.0\r\nTEL;CELL:cell\r\nEND:VCARD\r\n";
        CPPUNIT_ASSERT_EQUAL(parsed,
                             preparse(*source21,
                                      "BEGIN:VCARD\nVERSION:2.1\nTEL;CELL:cell\nEND:VCARD\n",
                                      "text/x-vcard"));

        parsed = "BEGIN:VCARD\r\nVERSION:3.0\r\nTEL;TYPE=CAR:car\r\nEND:VCARD\r\n";
        CPPUNIT_ASSERT_EQUAL(parsed,
                             preparse(*source21,
                                      "BEGIN:VCARD\nVERSION:2.1\nTEL;TYPE=CAR:car\nEND:VCARD\n",
                                      "text/x-vcard"));

        parsed = "BEGIN:VCARD\r\nVERSION:3.0\r\nTEL;TYPE=HOME:home\r\nEND:VCARD\r\n";
        CPPUNIT_ASSERT_EQUAL(parsed,
                             preparse(*source21,
                                      "BEGIN:VCARD\nVERSION:2.1\nTEL:home\nEND:VCARD\n",
                                      "text/x-vcard"));

        // TYPE=PARCEL not supported by Evolution, used to represent Evolutions TYPE=OTHER
        parsed = "BEGIN:VCARD\r\nVERSION:3.0\r\nTEL;TYPE=OTHER:other\r\nEND:VCARD\r\n";
        CPPUNIT_ASSERT_EQUAL(parsed,
                             preparse(*source21,
                                      "BEGIN:VCARD\nVERSION:2.1\nTEL;TYPE=PARCEL:other\nEND:VCARD\n",
                                      "text/x-vcard"));

        parsed = "BEGIN:VCARD\r\nVERSION:3.0\r\nTEL;TYPE=HOME;TYPE=VOICE:cell\r\nEND:VCARD\r\n";
        CPPUNIT_ASSERT_EQUAL(parsed,
                             preparse(*source21,
                                      "BEGIN:VCARD\nVERSION:2.1\nTEL;TYPE=HOME,VOICE:cell\nEND:VCARD\n",
                                      "text/x-vcard"));
    }

private:
    string preparse(EvolutionContactSource &source,
                    const char *data,
                    const char *type) {
        SyncItem item;
        item.setData(data, strlen(data));
        item.setDataType(type);
        return source.preparseVCard(item);
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(EvolutionContactTest);

#endif // ENABLE_UNIT_TESTS
#endif // ENABLE_EBOOK
