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

#include "FileSyncSource.h"

static EvolutionSyncSource *createSource(const EvolutionSyncSourceParams &params)
{
    pair <string, string> sourceType = EvolutionSyncSource::getSourceType(params.m_nodes);
    // The string returned by getSourceType() is always the one
    // registered as main Aliases() below.
    bool isMe = sourceType.first == "Files in one directory";

#ifndef ENABLE_FILE
    // tell SyncEvolution if the user wanted to use a disabled sync source,
    // otherwise let it continue searching
    return isMe ? RegisterSyncSource::InactiveSource : NULL;
#else
    // Also recognize one of the standard types?
    // Not in the FileSyncSource!
    bool maybeMe = false /* sourceType.first == "addressbook" */;
    
    if (isMe || maybeMe) {
        // The FileSyncSource always needs the data format
        // parameter in sourceType.second.
        if (/* sourceType.second == "" || sourceType.second == "text/x-vcard" */
            sourceType.second.size()) {
            return new FileSyncSource(params, sourceType.second);
        } else {
            return NULL;
        }
    }
    return NULL;
#endif
}

static RegisterSyncSource registerMe("Files in one directory",
#ifdef ENABLE_FILE
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "Files in one directory = file\n"
                                     "   Stores items in one directory as one file per item.\n"
                                     "   The directory is selected via [file://]<path>; it\n"
                                     "   will only be created if the prefix is given, otherwise\n"
                                     "   it must exist already. Only items of the same type can\n"
                                     "   be synchronized and this type must be specified explicitly\n"
                                     "   with both mime type and version.\n"
                                     "   Examples:\n"
                                     "      file:text/plain:1.0\n"
                                     "      file:text/x-vcard:2.1\n"
                                     "      file:text/vcard:3.0\n"
                                     "      file:text/x-calendar:1.0\n"
                                     "      file:text/calendar:2.0\n",
                                     Values() +
                                     (Aliases("Files in one directory") + "file"));

#ifdef ENABLE_FILE
#ifdef ENABLE_UNIT_TESTS

class FileSyncSourceUnitTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(FileSyncSourceUnitTest);
    CPPUNIT_TEST(testInstantiate);
    CPPUNIT_TEST_SUITE_END();

protected:
    void testInstantiate() {
        boost::shared_ptr<EvolutionSyncSource> source;
        source.reset(EvolutionSyncSource::createTestingSource("file", "file:text/vcard:3.0", true));
        source.reset(EvolutionSyncSource::createTestingSource("file", "file:text/plain:1.0", true));
        source.reset(EvolutionSyncSource::createTestingSource("file", "Files in one directory:text/x-vcard:2.1", true));
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(FileSyncSourceUnitTest);

#endif // ENABLE_UNIT_TESTS

#ifdef ENABLE_INTEGRATION_TESTS

// The anonymous namespace ensures that we don't get
// name clashes: although the classes and objects are
// only defined in this file, the methods generated
// for local classes will clash with other methods
// of other classes with the same name if no namespace
// is used.
//
// The code above is not yet inside the anonymous
// name space because it would show up inside
// the CPPUnit unit test names. Use a unique class
// name there.
namespace {
#if 0
}
#endif

static class VCard21Test : public RegisterSyncSourceTest {
public:
    VCard21Test() : RegisterSyncSourceTest("file_vcard21", "vcard21") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        // set type as required by FileSyncSource
        // and leave everything else at its default
        config.type = "file:text/x-vcard:2.1";
    }
} VCard21Test;

static class VCard30Test : public RegisterSyncSourceTest {
public:
    VCard30Test() : RegisterSyncSourceTest("file_vcard30", "vcard30") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.type = "file:text/vcard:3.0";
    }
} VCard30Test;

static class ICal20Test : public RegisterSyncSourceTest {
public:
    ICal20Test() : RegisterSyncSourceTest("file_ical20", "ical20") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.type = "file:text/calendar:2.0";

        // A sync source which supports linked items (= recurring
        // event with detached exception) is expected to handle
        // inserting the parent or child twice by turning the
        // second operation into an update. The file backend is
        // to dumb for that and therefore fails these tests:
        //
        // Client::Source::file_ical20::testLinkedItemsInsertParentTwice
        // Client::Source::file_ical20::testLinkedItemsInsertChildTwice
        //
        // Disable linked item testing to avoid this.
        config.sourceKnowsItemSemantic = false;
    }
} ICal20Test;

static class ITodo20Test : public RegisterSyncSourceTest {
public:
    ITodo20Test() : RegisterSyncSourceTest("file_itodo20", "itodo20") {}

    virtual void updateConfig(ClientTestConfig &config) const
    {
        config.type = "file:text/calendar:2.0";
    }
} ITodo20Test;

}
#endif // ENABLE_INTEGRATION_TESTS

#endif // ENABLE_FILE
