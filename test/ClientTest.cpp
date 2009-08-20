/*
 * Copyright (C) 2008 Funambol, Inc.
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

/** @cond API */
/** @addtogroup ClientTest */
/** @{ */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef ENABLE_INTEGRATION_TESTS

#include "ClientTest.h"
#include "test.h"
#include <EvolutionSyncSource.h>
#include <TransportAgent.h>
#include <Logging.h>
#include <SyncEvolutionUtil.h>
#include <EvolutionSyncClient.h>
#include <VolatileConfigNode.h>

#include <synthesis/dataconversion.h>

#include <memory>
#include <vector>
#include <utility>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <algorithm>

#include <boost/bind.hpp>

bool SyncOptions::defaultWBXML()
{
    const char *t = getenv("CLIENT_TEST_XML");
    if (t && (!strcmp(t, "1") || !strcasecmp(t, "t"))) {
        // use XML
        return false;
    } else {
        return true;
    }
}

/** utility function to iterate over different kinds of items in a sync source */
static std::list<std::string> listAnyItems(
    SyncSource *source,
    SyncItem * (SyncSource::*first)(),
    SyncItem * (SyncSource::*next)() )
{
    SyncItem *item = NULL;
    std::list<std::string> res;

    CPPUNIT_ASSERT(source);
    CPPUNIT_ASSERT(!source->hasFailed());
    SOURCE_ASSERT_NO_FAILURE(source, item = (source->*first)());
    while ( item ) {
        string key = item->getKey();
        SOURCE_ASSERT(source, !key.empty());
        res.push_back(key);
        delete item;
        SOURCE_ASSERT_NO_FAILURE(source, item = (source->*next)());
    }

    return res;
}

static std::list<std::string> listNewItems( SyncSource *source )
{
    std::list<std::string> res = listAnyItems(
        source,
        &SyncSource::getFirstNewItem,
        &SyncSource::getNextNewItem );
    return res;
}

static std::list<std::string> listUpdatedItems( SyncSource *source )
{
    std::list<std::string> res = listAnyItems(
        source,
        &SyncSource::getFirstUpdatedItem,
        &SyncSource::getNextUpdatedItem );
    return res;
}

static std::list<std::string> listDeletedItems( SyncSource *source )
{
    std::list<std::string> res = listAnyItems(
        source,
        &SyncSource::getFirstDeletedItem,
        &SyncSource::getNextDeletedItem );
    return res;
}

static std::list<std::string> listItems( SyncSource *source )
{
    std::list<std::string> res = listAnyItems(
        source,
        &SyncSource::getFirstItem,
        &SyncSource::getNextItem );
    return res;
}

std::list<std::string> listItemsOfType(SyncSource *source, itemType type)
{
    std::list<std::string> res;

    switch(type) {
     case NEW_ITEMS:
        res = listNewItems(source);
        break;
     case UPDATED_ITEMS:
        res = listUpdatedItems(source);
        break;
     case DELETED_ITEMS:
        res = listDeletedItems(source);
        break;
     case TOTAL_ITEMS:
        res = listItems(source);
        break;
     default:
        CPPUNIT_ASSERT(false);
        break;
    }
    return res;
}

static int countNewItems( SyncSource *source ) { return listNewItems(source).size(); }
static int countUpdatedItems( SyncSource *source ) { return listUpdatedItems(source).size(); }
static int countDeletedItems( SyncSource *source ) { return listDeletedItems(source).size(); }
static int countItems( SyncSource *source ) { return listItems(source).size(); }
int countItemsOfType(SyncSource *source, itemType type) { return listItemsOfType(source, type).size(); }


/** insert new item, return LUID */
static std::string importItem(SyncSource *source, std::string &data)
{
    CPPUNIT_ASSERT(source);
    if (data.size()) {
        SyncItem item;
        item.setData( data.c_str(), (long)data.size() );
        item.setDataType( ("raw") );
        SyncMLStatus status = STATUS_OK;
        SOURCE_ASSERT_NO_FAILURE(source, status = source->addItem(item));
        CPPUNIT_ASSERT(status == STATUS_OK);
        CPPUNIT_ASSERT(!item.getKey().empty());
        return item.getKey();
    } else {
        return "";
    }
}

/** adds the supported tests to the instance itself */
void LocalTests::addTests() {
    if (config.createSourceA) {
        ADD_TEST(LocalTests, testOpen);
        ADD_TEST(LocalTests, testIterateTwice);
        if (config.insertItem) {
            ADD_TEST(LocalTests, testSimpleInsert);
            ADD_TEST(LocalTests, testLocalDeleteAll);
            ADD_TEST(LocalTests, testComplexInsert);

            if (config.updateItem) {
                ADD_TEST(LocalTests, testLocalUpdate);

                if (config.createSourceB) {
                    ADD_TEST(LocalTests, testChanges);
                }
            }

            if (config.import &&
                config.dump &&
                config.compare &&
                config.testcases) {
                ADD_TEST(LocalTests, testImport);
                ADD_TEST(LocalTests, testImportDelete);
            }

            if (config.templateItem &&
                config.uniqueProperties) {
                ADD_TEST(LocalTests, testManyChanges);
            }

            if (config.parentItem &&
                config.childItem) {
                ADD_TEST(LocalTests, testLinkedItemsParent);
                ADD_TEST(LocalTests, testLinkedItemsChild);
                ADD_TEST(LocalTests, testLinkedItemsParentChild);
                ADD_TEST(LocalTests, testLinkedItemsChildParent);
                ADD_TEST(LocalTests, testLinkedItemsChildChangesParent);
                ADD_TEST(LocalTests, testLinkedItemsRemoveParentFirst);
                ADD_TEST(LocalTests, testLinkedItemsRemoveNormal);
                if (config.sourceKnowsItemSemantic) {
                    ADD_TEST(LocalTests, testLinkedItemsInsertParentTwice);
                    ADD_TEST(LocalTests, testLinkedItemsInsertChildTwice);
                }
                ADD_TEST(LocalTests, testLinkedItemsParentUpdate);
                ADD_TEST(LocalTests, testLinkedItemsUpdateChild);
                ADD_TEST(LocalTests, testLinkedItemsInsertBothUpdateChild);
                ADD_TEST(LocalTests, testLinkedItemsInsertBothUpdateParent);
            }
        }
    }
}

std::string LocalTests::insert(CreateSource createSource, const char *data, const char *dataType, bool relaxed) {
    std::string uid;

    // create source
    std::auto_ptr<SyncSource> source(createSource());
    CPPUNIT_ASSERT(source.get() != 0);

    // count number of already existing items
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);
    int numItems = 0;
    CPPUNIT_ASSERT_NO_THROW(numItems = countItems(source.get()));
    SyncItem item;
    item.setData(data, (long)strlen(data));
    item.setDataType(dataType);
    SyncMLStatus status = STATUS_OK;
    SOURCE_ASSERT_NO_FAILURE(source.get(), status = source->addItem(item));
    CPPUNIT_ASSERT(!item.getKey().empty());
    uid = item.getKey();
    SOURCE_ASSERT(source.get(), source->endSync() == 0);

    // delete source again
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    if (!relaxed) {
        // two possible results:
        // - a new item was added
        // - the item was matched against an existing one
        CPPUNIT_ASSERT_NO_THROW(source.reset(createSource()));
        CPPUNIT_ASSERT(source.get() != 0);
        SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);
        CPPUNIT_ASSERT(status == STATUS_OK || status == STATUS_DATA_MERGED);
        CPPUNIT_ASSERT_EQUAL(numItems + (status == STATUS_DATA_MERGED ? 0 : 1),
                             countItems(source.get()));
        CPPUNIT_ASSERT(countNewItems(source.get()) == 0);
        CPPUNIT_ASSERT(countUpdatedItems(source.get()) == 0);
        CPPUNIT_ASSERT(countDeletedItems(source.get()) == 0);
        SOURCE_ASSERT(source.get(), source->endSync() == 0 );
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }

#if 0
    /* source.createItem() is a SyncEvolution extension which cannot be used here */
    SyncItem *sameItem;
    SOURCE_ASSERT_NO_FAILURE(
        source,
        sameItem = source.createItem(item.getKey(), item.getState()));
    CPPUNIT_ASSERT(sameItem != 0);
    CPPUNIT_ASSERT_EQUAL(sameItem->getKey(), item.getKey());
    delete sameItem;
#endif

    return uid;
}

/** deletes specific item locally via sync source */
static std::string updateItem(CreateSource createSource, const std::string &uid, const char *data) {
    std::string newuid;

    CPPUNIT_ASSERT(createSource.createSource);

    // create source
    std::auto_ptr<SyncSource> source(createSource());
    CPPUNIT_ASSERT(source.get() != 0);
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);

    // insert item
    SyncItem item;
    item.setKey(uid.c_str());
    item.setData(data, (long)strlen(data) + 1);
    item.setDataType("raw");
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->updateItem(item));
    SOURCE_ASSERT(source.get(), !item.getKey().empty());
    newuid = item.getKey();

    SOURCE_ASSERT(source.get(), source->endSync() == 0);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    return newuid;
}

/** updates specific item locally via sync source */
static void removeItem(CreateSource createSource, const std::string &luid)
{
    CPPUNIT_ASSERT(createSource.createSource);

    // create source
    std::auto_ptr<SyncSource> source(createSource());
    CPPUNIT_ASSERT(source.get() != 0);
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);

    // remove item
    SyncItem item;
    item.setKey(luid);
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->deleteItem(item));

    SOURCE_ASSERT(source.get(), source->endSync() == 0);
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}

void LocalTests::update(CreateSource createSource, const char *data, const char *dataType, bool check) {
    CPPUNIT_ASSERT(createSource.createSource);
    CPPUNIT_ASSERT(data);

    // create source
    std::auto_ptr<SyncSource> source(createSource());
    CPPUNIT_ASSERT(source.get() != 0);
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);

    // get existing item, then update it
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0 );
    std::auto_ptr<SyncItem> item;
    SOURCE_ASSERT_NO_FAILURE(source.get(), item.reset(source->getFirstItem()) );
    CPPUNIT_ASSERT(item.get());
    item->setData(data, (long)strlen(data) + 1);
    item->setDataType(dataType);
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->updateItem(*item));
    SOURCE_ASSERT(source.get(), source->endSync() == 0);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    if (!check) {
        return;
    }

    // check that the right changes are reported when reopening the source
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0 );
    CPPUNIT_ASSERT_EQUAL(1, countItems(source.get()));
    CPPUNIT_ASSERT_EQUAL(0, countNewItems(source.get()));
    CPPUNIT_ASSERT_EQUAL(0, countUpdatedItems(source.get()));
    CPPUNIT_ASSERT_EQUAL(0, countDeletedItems(source.get()));
    std::auto_ptr<SyncItem> modifiedItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), modifiedItem.reset(source->getFirstItem()) );
    CPPUNIT_ASSERT(modifiedItem.get());
    CPPUNIT_ASSERT_EQUAL(item->getKey(), modifiedItem->getKey());
}

void LocalTests::update(CreateSource createSource, const char *data, const char *dataType, const std::string &luid) {
    CPPUNIT_ASSERT(createSource.createSource);
    CPPUNIT_ASSERT(data);

    // create source
    std::auto_ptr<SyncSource> source(createSource());
    CPPUNIT_ASSERT(source.get() != 0);
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);

    // get existing item, then update it
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0 );
    SyncItem item;
    item.setData(data, (long)strlen(data) + 1);
    item.setDataType(dataType);
    item.setKey(luid.c_str());
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->updateItem(item));
    SOURCE_ASSERT(source.get(), source->endSync() == 0);
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}

/** deletes all items locally via sync source */
void LocalTests::deleteAll(CreateSource createSource) {
    CPPUNIT_ASSERT(createSource.createSource);

    // create source
    std::auto_ptr<SyncSource> source(createSource());
    CPPUNIT_ASSERT(source.get() != 0);
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);

    // delete all items
    SOURCE_ASSERT_NO_FAILURE(source.get(), source->removeAllItems());
    SOURCE_ASSERT(source.get(), source->endSync() == 0);
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // check that all items are gone
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSource()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_MESSAGE(
        "should be empty now",
        source.get(),
        countItems(source.get()) == 0);
    CPPUNIT_ASSERT_EQUAL( 0, countNewItems(source.get()) );
    CPPUNIT_ASSERT_EQUAL( 0, countUpdatedItems(source.get()) );
    CPPUNIT_ASSERT_EQUAL( 0, countDeletedItems(source.get()) );
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}

/** deletes specific item locally via sync source */
static void deleteItem(CreateSource createSource, const std::string &uid) {
    CPPUNIT_ASSERT(createSource.createSource);

    // create source
    std::auto_ptr<SyncSource> source(createSource());
    CPPUNIT_ASSERT(source.get() != 0);
    SOURCE_ASSERT(source.get(), source->beginSync(SYNC_NONE) == 0);

    // delete item
    SyncItem item;
    item.setKey(uid.c_str());
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->deleteItem(item));

    SOURCE_ASSERT(source.get(), source->endSync() == 0);
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}

/**
 * takes two databases, exports them,
 * then compares them using synccompare
 *
 * @param refFile      existing file with source reference items, NULL uses a dump of sync source A instead
 * @param copy         a sync source which contains the copied items, begin/endSync will be called
 * @param raiseAssert  raise assertion if comparison yields differences (defaults to true)
 */
bool LocalTests::compareDatabases(const char *refFile, SyncSource &copy, bool raiseAssert) {
    CPPUNIT_ASSERT(config.dump);

    std::string sourceFile, copyFile;

    if (refFile) {
        sourceFile = refFile;
    } else {
        sourceFile = getCurrentTest() + ".A.test.dat";
        simplifyFilename(sourceFile);
        std::auto_ptr<SyncSource> source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
        SOURCE_ASSERT_EQUAL(source.get(), 0, config.dump(client, *source.get(), sourceFile.c_str()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }

    copyFile = getCurrentTest() + ".B.test.dat";
    simplifyFilename(copyFile);
    SOURCE_ASSERT_EQUAL(&copy, STATUS_OK, copy.beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(&copy, 0, config.dump(client, copy, copyFile.c_str()));
    SOURCE_ASSERT_EQUAL(&copy, STATUS_OK, copy.endSync());

    bool equal = config.compare(client, sourceFile.c_str(), copyFile.c_str());
    CPPUNIT_ASSERT(!raiseAssert || equal);

    return equal;
}

std::string LocalTests::createItem(int item, const std::string &revision, int size)
{
    std::string data = config.templateItem;
    std::stringstream prefix;

    prefix << std::setfill('0') << std::setw(3) << item << " ";

    const char *prop = config.uniqueProperties;
    const char *nextProp;
    while (*prop) {
        std::string curProp;
        nextProp = strchr(prop, ':');
        if (!nextProp) {
            curProp = prop;
        } else {
            curProp = std::string(prop, 0, nextProp - prop);
        }

        std::string property;
        // property is expected to not start directly at the
        // beginning
        property = "\n";
        property += curProp;
        property += ":";
        size_t off = data.find(property);
        if (off != data.npos) {
            data.insert(off + property.size(), prefix.str());
        }

        if (!nextProp) {
            break;
        }
        prop = nextProp + 1;
    }
    data.replace(data.find("<<REVISION>>"), strlen("<<REVISION>>"), revision);
    if (size > 0 && (int)data.size() < size) {
        int additionalBytes = size - (int)data.size();
        int added = 0;
        /* vCard 2.1 and vCal 1.0 need quoted-printable line breaks */
        bool quoted = data.find("VERSION:1.0") != data.npos ||
            data.find("VERSION:2.1") != data.npos;
        size_t toreplace = 1;

        CPPUNIT_ASSERT(config.sizeProperty);

        /* stuff the item so that it reaches at least that size */
        size_t off = data.find(config.sizeProperty);
        CPPUNIT_ASSERT(off != data.npos);
        std::stringstream stuffing;
        if (quoted) {
            stuffing << ";ENCODING=QUOTED-PRINTABLE:";
        } else {
            stuffing << ":";
        }

        // insert after the first line, it often acts as the summary
        if (data.find("BEGIN:VJOURNAL") != data.npos) {
            size_t start = data.find(":", off);
            CPPUNIT_ASSERT( start != data.npos );
            size_t eol = data.find("\\n", off);
            CPPUNIT_ASSERT( eol != data.npos );
            stuffing << data.substr(start + 1, eol - start + 1);
            toreplace += eol - start + 1;
        }

        while(added < additionalBytes) {
            int linelen = 0;

            while(added + 4 < additionalBytes &&
                  linelen < 60) {
                stuffing << 'x';
                added++;
                linelen++;
            }
            // insert line breaks to allow folding
            if (quoted) {
                stuffing << "x=0D=0Ax";
                added += 8;
            } else {
                stuffing << "x\\nx";
                added += 4;
            }
        }
        off = data.find(":", off);
        data.replace(off, toreplace, stuffing.str());
    }

    return data;
}


/**
 * insert artificial items, number of them determined by config.numItems
 * unless passed explicitly
 *
 * @param createSource    a factory for the sync source that is to be used
 * @param startIndex      IDs are generated starting with this value
 * @param numItems        number of items to be inserted if non-null, otherwise config.numItems is used
 * @param size            minimum size for new items
 * @return LUIDs of all inserted items
 */
std::list<std::string> LocalTests::insertManyItems(CreateSource createSource, int startIndex, int numItems, int size) {
    std::list<std::string> luids;

    CPPUNIT_ASSERT(config.templateItem);
    CPPUNIT_ASSERT(config.uniqueProperties);

    std::auto_ptr<SyncSource> source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    CPPUNIT_ASSERT(startIndex > 1 || !countItems(source.get()));

    int firstIndex = startIndex;
    if (firstIndex < 0) {
        firstIndex = 1;
    }
    int lastIndex = firstIndex + (numItems >= 1 ? numItems : config.numItems) - 1;
    for (int item = firstIndex; item <= lastIndex; item++) {
        std::string data = createItem(item, "", size);
        luids.push_back(importItem(source.get(), data));
    }

    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());
    return luids;
}

// creating sync source
void LocalTests::testOpen() {
    // check requirements
    CPPUNIT_ASSERT(config.createSourceA);

    std::auto_ptr<SyncSource> source(createSourceA());
    CPPUNIT_ASSERT(source.get() != 0);
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}

// restart scanning of items
void LocalTests::testIterateTwice() {
    // check requirements
    CPPUNIT_ASSERT(config.createSourceA);

    // open source
    std::auto_ptr<SyncSource> source(createSourceA());
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_MESSAGE(
        "iterating twice should produce identical results",
        source.get(),
        countItems(source.get()) == countItems(source.get()));
}

// insert one contact without clearing the source first
void LocalTests::testSimpleInsert() {
    // check requirements
    CPPUNIT_ASSERT(config.insertItem);
    CPPUNIT_ASSERT(config.createSourceA);

    insert(createSourceA, config.insertItem, config.itemType);
}

// delete all items
void LocalTests::testLocalDeleteAll() {
    // check requirements
    CPPUNIT_ASSERT(config.insertItem);
    CPPUNIT_ASSERT(config.createSourceA);

    // make sure there is something to delete, then delete again
    insert(createSourceA, config.insertItem, config.itemType);
    deleteAll(createSourceA);
}

// clean database, then insert
void LocalTests::testComplexInsert() {
    testLocalDeleteAll();
    testSimpleInsert();
    testIterateTwice();
}

// clean database, insert item, update it
void LocalTests::testLocalUpdate() {
    // check additional requirements
    CPPUNIT_ASSERT(config.updateItem);

    testLocalDeleteAll();
    testSimpleInsert();
    update(createSourceA, config.updateItem, config.itemType);
}

// complex sequence of changes
void LocalTests::testChanges() {
    // check additional requirements
    CPPUNIT_ASSERT(config.createSourceB);

    testLocalDeleteAll();
    testSimpleInsert();

    // clean changes in sync source B by creating and closing it
    std::auto_ptr<SyncSource> source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // no new changes now
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    std::auto_ptr<SyncItem> item;
    SOURCE_ASSERT_NO_FAILURE(source.get(), item.reset(source->getFirstItem()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // delete item again via sync source A
    deleteAll(createSourceA);
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
    std::auto_ptr<SyncItem> deletedItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), deletedItem.reset(source->getFirstDeletedItem()));
    CPPUNIT_ASSERT(!item->getKey().empty());
    CPPUNIT_ASSERT_EQUAL(item->getKey(), deletedItem->getKey());
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // insert another item via sync source A
    testSimpleInsert();
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    SOURCE_ASSERT_NO_FAILURE(source.get(), item.reset(source->getFirstItem()));
    std::auto_ptr<SyncItem> newItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), newItem.reset(source->getFirstNewItem()));
    CPPUNIT_ASSERT(!item->getKey().empty());
    CPPUNIT_ASSERT_EQUAL(item->getKey(), newItem->getKey());
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // update item via sync source A
    update(createSourceA, config.updateItem, config.itemType);
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    std::auto_ptr<SyncItem> updatedItem;
    SOURCE_ASSERT_NO_FAILURE(source.get(), updatedItem.reset(source->getFirstUpdatedItem()));
    CPPUNIT_ASSERT(!item->getKey().empty());
    CPPUNIT_ASSERT_EQUAL(item->getKey(), updatedItem->getKey());
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // start anew, then create and update an item -> should only be listed as new
    // or updated, but not both
    deleteAll(createSourceA);
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    testSimpleInsert();
    update(createSourceA, config.updateItem, config.itemType);
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()) + countUpdatedItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));

    // start anew, then create, delete and recreate an item -> should only be listed as new or updated,
    // even if (as for calendar with UID) the same LUID gets reused
    deleteAll(createSourceA);
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    testSimpleInsert();
    deleteAll(createSourceA);
    testSimpleInsert();
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
    SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()) + countUpdatedItems(source.get()));
    if (countDeletedItems(source.get()) == 1) {
        // It's not nice, but acceptable to send the LUID of a deleted item to a
        // server which has never seen that LUID. The LUID must not be the same as
        // the one we list as new or updated, though.
        SyncItem *deleted = source->getFirstDeletedItem();
        CPPUNIT_ASSERT(deleted);
        SyncItem *new_or_updated = source->getFirstNewItem();
        if (!new_or_updated) {
            new_or_updated = source->getFirstUpdatedItem();
        }
        CPPUNIT_ASSERT(new_or_updated);
        CPPUNIT_ASSERT(deleted->getKey() != new_or_updated->getKey());
    } else {
        SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
    }
}

// clean database, import file, then export again and compare
void LocalTests::testImport() {
    // check additional requirements
    CPPUNIT_ASSERT(config.import);
    CPPUNIT_ASSERT(config.dump);
    CPPUNIT_ASSERT(config.compare);
    CPPUNIT_ASSERT(config.testcases);

    testLocalDeleteAll();

    // import via sync source A
    std::auto_ptr<SyncSource> source;
    SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(createSourceA()));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
    std::string testcases;
    SOURCE_ASSERT_EQUAL(source.get(), 0, config.import(client, *source.get(), config.testcases, testcases));
    SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
    CPPUNIT_ASSERT_NO_THROW(source.reset());

    // export again and compare against original file
    std::auto_ptr<SyncSource> copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceA()));
    compareDatabases(testcases.c_str(), *copy.get());
    CPPUNIT_ASSERT_NO_THROW(source.reset());
}

// same as testImport() with immediate delete
void LocalTests::testImportDelete() {
    testImport();

    // delete again, because it was observed that this did not
    // work right with calendars in SyncEvolution
    testLocalDeleteAll();
}

// test change tracking with large number of items
void LocalTests::testManyChanges() {
    // check additional requirements
    CPPUNIT_ASSERT(config.templateItem);
    CPPUNIT_ASSERT(config.uniqueProperties);

    deleteAll(createSourceA);

    // check that everything is empty, also resets change counter of sync source B
    std::auto_ptr<SyncSource> copy;
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // now insert plenty of items
    int numItems = insertManyItems(createSourceA).size();

    // check that exactly this number of items is listed as new
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // delete all items
    deleteAll(createSourceA);

    // verify again
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), numItems, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

template<class T, class V> int countEqual(const T &container,
                                          const V &value) {
    return count(container.begin(),
                 container.end(),
                 value);
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParent() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // now insert main item
    parent = insert(createSourceA, config.parentItem, config.itemType);

    // check that exactly the parent is listed as new
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // delete all items
    deleteAll(createSourceA);

    // verify again
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChild() {
#if LINKED_ITEMS_RELAXED_SEMANTIC
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // same as above for child item
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteAll(createSourceA);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
#endif
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParentChild() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert parent first, then child
    parent = insert(createSourceA, config.parentItem, config.itemType);
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteAll(createSourceA);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChildParent() {
#if LINKED_ITEMS_RELAXED_SEMANTIC
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert child first, then parent
    child = insert(createSourceA, config.childItem, config.itemType);
    parent = insert(createSourceA, config.parentItem, config.itemType, true);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteAll(createSourceA);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
#endif
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsChildChangesParent() {
#if LINKED_ITEMS_RELAXED_SEMANTIC
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert child first, check changes, then insert the parent
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    parent = insert(createSourceA, config.parentItem, config.itemType, true);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listNewItems(copy.get()), parent));
    // relaxed semantic: the child item might be considered updated now if
    // it had to be modified when inserting the parent
    SOURCE_ASSERT(copy.get(), 1 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteAll(createSourceA);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
#endif
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsRemoveParentFirst() {
#if LINKED_ITEMS_RELAXED_SEMANTIC
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert both items, remove parent, then child
    parent = insert(createSourceA, config.parentItem, config.itemType);
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, parent);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    // deleting the parent may or may not modify the child
    SOURCE_ASSERT(copy.get(), 1 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, child);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
#endif
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsRemoveNormal() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // insert both items, remove child, then parent
    parent = insert(createSourceA, config.parentItem, config.itemType);
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, child);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, parent);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertParentTwice() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent twice (should be turned into update)
    parent = insert(createSourceA, config.parentItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    parent = insert(createSourceA, config.parentItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, parent);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertChildTwice() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

#if LINKED_ITEMS_RELAXED_SEMANTIC
    // add child twice (should be turned into update)
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, child);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
#endif
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsParentUpdate() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent, then update it
    parent = insert(createSourceA, config.parentItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    parent = updateItem(createSourceA, parent, config.parentItem);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, parent);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsUpdateChild() {
#if LINKED_ITEMS_RELAXED_SEMANTIC
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add child, then update it
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    child = updateItem(createSourceA, child, config.childItem);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, child);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
#endif
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertBothUpdateChild() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent and child, then update child
    parent = insert(createSourceA, config.parentItem, config.itemType);
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    child = updateItem(createSourceA, child, config.childItem);

    // child has to be listed as modified, parent may be
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 1 <= countUpdatedItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 2 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, parent);
    deleteItem(createSourceA, child);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}

// test inserting, removing and updating of parent + child item in
// various order plus change tracking
void LocalTests::testLinkedItemsInsertBothUpdateParent() {
    // check additional requirements
    CPPUNIT_ASSERT(config.parentItem);
    CPPUNIT_ASSERT(config.childItem);

    deleteAll(createSourceA);
    std::string parent, child;
    std::auto_ptr<SyncSource> copy;

    // check that everything is empty, also resets change counter of sync source B
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    // add parent and child, then update parent
    parent = insert(createSourceA, config.parentItem, config.itemType);
    child = insert(createSourceA, config.childItem, config.itemType);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    parent = updateItem(createSourceA, parent, config.parentItem);

    // parent has to be listed as modified, child may be
    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 1 <= countUpdatedItems(copy.get()));
    SOURCE_ASSERT(copy.get(), 2 >= countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listUpdatedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());

    deleteItem(createSourceA, parent);
    deleteItem(createSourceA, child);

    SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(createSourceB()));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countNewItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 0, countUpdatedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 2, countDeletedItems(copy.get()));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), parent));
    SOURCE_ASSERT_EQUAL(copy.get(), 1, countEqual(listDeletedItems(copy.get()), child));
    SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
    CPPUNIT_ASSERT_NO_THROW(copy.reset());
}


SyncTests::SyncTests(const std::string &name, ClientTest &cl, std::vector<int> sourceIndices, bool isClientA) :
    CppUnit::TestSuite(name),
    client(cl) {
    sourceArray = new int[sourceIndices.size() + 1];
    for (std::vector<int>::iterator it = sourceIndices.begin();
         it != sourceIndices.end();
         ++it) {
        ClientTest::Config config;
        client.getSourceConfig(*it, config);

        if (config.sourceName) {
            sourceArray[sources.size()] = *it;
            sources.push_back(std::pair<int,LocalTests *>(*it, cl.createLocalTests(config.sourceName, *it, config)));
        }
    }
    sourceArray[sources.size()] = -1;

    // check whether we have a second client
    ClientTest *clientB = cl.getClientB();
    if (clientB) {
        accessClientB = clientB->createSyncTests(name, sourceIndices, false);
    } else {
        accessClientB = 0;
    }
}

SyncTests::~SyncTests() {
    for (source_it it = sources.begin();
         it != sources.end();
         ++it) {
        delete it->second;
    }
    delete [] sourceArray;
    if (accessClientB) {
        delete accessClientB;
    }
}

/** adds the supported tests to the instance itself */
void SyncTests::addTests() {
    if (sources.size()) {
        const ClientTest::Config &config(sources[0].second->config);

        // run this test first, even if it is more complex:
        // if it works, all the following tests will run with
        // the server in a deterministic state
        if (config.createSourceA) {
            if (config.insertItem) {
                ADD_TEST(SyncTests, testDeleteAllRefresh);
            }
        }

        ADD_TEST(SyncTests, testTwoWaySync);
        ADD_TEST(SyncTests, testSlowSync);
        ADD_TEST(SyncTests, testRefreshFromServerSync);
        ADD_TEST(SyncTests, testRefreshFromClientSync);

        if (config.compare &&
            config.testcases) {
            ADD_TEST(SyncTests, testConversion);
        }

        if (config.createSourceA) {
            if (config.insertItem) {
                ADD_TEST(SyncTests, testRefreshFromServerSemantic);
                ADD_TEST(SyncTests, testRefreshFromClientSemantic);
                ADD_TEST(SyncTests, testRefreshStatus);

                if (accessClientB &&
                    config.dump &&
                    config.compare) {
                    ADD_TEST(SyncTests, testCopy);
                    ADD_TEST(SyncTests, testDelete);
                    ADD_TEST(SyncTests, testAddUpdate);
                    ADD_TEST(SyncTests, testManyItems);
                    ADD_TEST(SyncTests, testSlowSyncSemantic);
                    ADD_TEST(SyncTests, testComplexRefreshFromServerSemantic);

                    if (config.updateItem) {
                        ADD_TEST(SyncTests, testUpdate);
                    }
                    if (config.complexUpdateItem) {
                        ADD_TEST(SyncTests, testComplexUpdate);
                    }
                    if (config.mergeItem1 && config.mergeItem2) {
                        ADD_TEST(SyncTests, testMerge);
                    }
                    if (config.import) {
                        ADD_TEST(SyncTests, testTwinning);
                        ADD_TEST(SyncTests, testItems);
                        ADD_TEST(SyncTests, testItemsXML);
                    }
                    if (config.templateItem) {
                        ADD_TEST(SyncTests, testMaxMsg);
                        ADD_TEST(SyncTests, testLargeObject);
                        ADD_TEST(SyncTests, testOneWayFromServer);
                        ADD_TEST(SyncTests, testOneWayFromClient);
                    }
                }
            }
        }

        if (config.retrySync &&
            config.insertItem &&
            config.updateItem &&
            accessClientB &&
            config.dump &&
            config.compare) {
            CppUnit::TestSuite *retryTests = new CppUnit::TestSuite(getName() + "::Retry");
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientAdd);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientRemove);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeClientUpdate);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerAdd);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerRemove);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeServerUpdate);
            ADD_TEST_TO_SUITE(retryTests, SyncTests, testInterruptResumeFull);
            addTest(FilterTest(retryTests));
        }

        if (config.retrySync &&
            config.insertItem &&
            config.updateItem &&
            accessClientB &&
            config.dump &&
            config.compare) {
            CppUnit::TestSuite *suspendTests = new CppUnit::TestSuite(getName() + "::Suspend");
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientAdd);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientRemove);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendClientUpdate);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerAdd);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerRemove);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendServerUpdate);
            ADD_TEST_TO_SUITE(suspendTests, SyncTests, testUserSuspendFull);
            addTest(FilterTest(suspendTests));
        }

    }
}

bool SyncTests::compareDatabases(const char *refFileBase, bool raiseAssert) {
    source_it it1;
    source_it it2;
    bool equal = true;

    CPPUNIT_ASSERT(accessClientB);
    for (it1 = sources.begin(), it2 = accessClientB->sources.begin();
         it1 != sources.end() && it2 != accessClientB->sources.end();
         ++it1, ++it2) {
        std::auto_ptr<SyncSource> copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it2->second->createSourceB()));
        if (refFileBase) {
            std::string refFile = refFileBase;
            refFile += it1->second->config.sourceName;
            refFile += ".dat";
            simplifyFilename(refFile);
            if (!it1->second->compareDatabases(refFile.c_str(), *copy.get(), raiseAssert)) {
                equal = false;
            }
        } else {
            if (!it1->second->compareDatabases(NULL, *copy.get(), raiseAssert)) {
                equal = false;
            }
        }
        CPPUNIT_ASSERT_NO_THROW(copy.reset());
    }
    CPPUNIT_ASSERT(it1 == sources.end());
    CPPUNIT_ASSERT(it2 == accessClientB->sources.end());

    CPPUNIT_ASSERT(!raiseAssert || equal);
    return equal;
}

/** deletes all items locally and on server */
void SyncTests::deleteAll(DeleteAllMode mode) {
    source_it it;
    SyncPrefix prefix("deleteall", *this);

    switch(mode) {
     case DELETE_ALL_SYNC:
        // a refresh from server would slightly reduce the amount of data exchanged, but not all servers support it
        for (it = sources.begin(); it != sources.end(); ++it) {
            it->second->deleteAll(it->second->createSourceA);
        }
        doSync("init", SyncOptions(SYNC_SLOW));
        // now that client and server are in sync, delete locally and sync again
        for (it = sources.begin(); it != sources.end(); ++it) {
            it->second->deleteAll(it->second->createSourceA);
        }
        doSync("twoway",
               SyncOptions(SYNC_TWO_WAY,
                           CheckSyncReport(0,0,0, 0,0,-1, true, SYNC_TWO_WAY)));
        break;
     case DELETE_ALL_REFRESH:
        // delete locally and then tell the server to "copy" the empty databases
        for (it = sources.begin(); it != sources.end(); ++it) {
            it->second->deleteAll(it->second->createSourceA);
        }
        doSync("refreshserver",
               SyncOptions(SYNC_REFRESH_FROM_CLIENT,
                           CheckSyncReport(0,0,0, 0,0,-1, true, SYNC_REFRESH_FROM_CLIENT)));
        break;
    }
}

/** get both clients in sync with empty server, then copy one item from client A to B */
void SyncTests::doCopy() {
    SyncPrefix("copy", *this);

    // check requirements
    CPPUNIT_ASSERT(accessClientB);

    deleteAll();
    accessClientB->deleteAll();

    // insert into first database, copy to server
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // copy into second database
    accessClientB->doSync("recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    compareDatabases();
}

/**
 * replicate server database locally: same as SYNC_REFRESH_FROM_SERVER,
 * but done with explicit local delete and then a SYNC_SLOW because some
 * servers do no support SYNC_REFRESH_FROM_SERVER
 */
void SyncTests::refreshClient(SyncOptions options) {
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    doSync("refresh",
           options
           .setSyncMode(SYNC_SLOW)
           .setCheckReport(CheckSyncReport(-1,0,0, 0,0,0, true, SYNC_SLOW)));
}


// delete all items, locally and on server using refresh-from-client sync
void SyncTests::testDeleteAllRefresh() {
    source_it it;

    // copy something to server first; doesn't matter whether it has the
    // item already or not, as long as it exists there afterwards
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("insert", SyncOptions(SYNC_SLOW));

    // now ensure we can delete it
    deleteAll(DELETE_ALL_SYNC);

    // nothing stored locally?
    for (it = sources.begin(); it != sources.end(); ++it) {
        std::auto_ptr<SyncSource> source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }

    // make sure server really deleted everything
    doSync("check",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_SLOW)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        std::auto_ptr<SyncSource> source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }
}

// test that a refresh sync from an empty server leads to an empty datatbase
// and no changes are sent to server during next two-way sync
void SyncTests::testRefreshFromServerSemantic() {
    source_it it;

    // clean client and server
    deleteAll();

    // insert item, then refresh from empty server
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("refresh",
           SyncOptions(SYNC_REFRESH_FROM_SERVER,
                       CheckSyncReport(0,0,-1, 0,0,0, true, SYNC_REFRESH_FROM_SERVER)));

    // check
    for (it = sources.begin(); it != sources.end(); ++it) {
        std::auto_ptr<SyncSource> source;
        SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
        SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
        SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
        CPPUNIT_ASSERT_NO_THROW(source.reset());
    }
    doSync("two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
}

// test that a refresh sync from an empty client leads to an empty datatbase
// and no changes are sent to server during next two-way sync
void SyncTests::testRefreshFromClientSemantic() {
    source_it it;

    // clean client and server
    deleteAll();

    // insert item, send to server
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // delete locally
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    // refresh from client
    doSync("refresh",
           SyncOptions(SYNC_REFRESH_FROM_CLIENT,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_REFRESH_FROM_CLIENT)));

    // check
    doSync("check",
           SyncOptions(SYNC_REFRESH_FROM_SERVER,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_REFRESH_FROM_SERVER)));
}

// tests the following sequence of events:
// - insert item
// - delete all items
// - insert one other item
// - refresh from client
// => no items should now be listed as new, updated or deleted for this client during another sync
void SyncTests::testRefreshStatus() {
    source_it it;

    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testSimpleInsert();
    }
    doSync("refresh-from-client",
           SyncOptions(SYNC_REFRESH_FROM_CLIENT,
                       CheckSyncReport(0,0,0, -1,-1,-1, /* strictly speaking 1,0,0, but not sure exactly what the server will be told */
                                       true, SYNC_REFRESH_FROM_CLIENT)));
    doSync("two-way",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
}

// test that a two-way sync copies updates from database to the other client,
// using simple data commonly supported by servers
void SyncTests::testUpdate() {
    CPPUNIT_ASSERT(sources.begin() != sources.end());
    CPPUNIT_ASSERT(sources.begin()->second->config.updateItem);

    // setup client A, B and server so that they all contain the same item
    doCopy();

    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceA, it->second->config.updateItem, it->second->config.itemType);
    }

    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync("update",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,1,0, 0,0,0, true, SYNC_TWO_WAY)));

    compareDatabases();
}

// test that a two-way sync copies updates from database to the other client,
// using data that some, but not all servers support, like adding a second
// phone number to a contact
void SyncTests::testComplexUpdate() {
    // setup client A, B and server so that they all contain the same item
    doCopy();

    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceA,
                           /* this test might get executed with some sources which have
                              a complex update item while others don't: use the normal update item
                              for them or even just the same item */
                           it->second->config.complexUpdateItem ? it->second->config.complexUpdateItem :
                           it->second->config.updateItem ? it->second->config.updateItem :
                           it->second->config.insertItem,
                           it->second->config.itemType
                           );
    }

    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    accessClientB->doSync("update",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,1,0, 0,0,0, true, SYNC_TWO_WAY)));

    compareDatabases();
}


// test that a two-way sync deletes the copy of an item in the other database
void SyncTests::testDelete() {
    // setup client A, B and server so that they all contain the same item
    doCopy();

    // delete it on A
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);
    }

    // transfer change from A to server to B
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    accessClientB->doSync("delete",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));

    // check client B: shouldn't have any items now
    for (it = sources.begin(); it != sources.end(); ++it) {
        std::auto_ptr<SyncSource> copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
        SOURCE_ASSERT_EQUAL(copy.get(), 0, countItems(copy.get()));
        SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
        CPPUNIT_ASSERT_NO_THROW(copy.reset());
    }
}

// test what the server does when it finds that different
// fields of the same item have been modified
void SyncTests::testMerge() {
    // setup client A, B and server so that they all contain the same item
    doCopy();

    // update in client A
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceA, it->second->config.mergeItem1, it->second->config.itemType);
    }

    // update in client B
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->update(it->second->createSourceA, it->second->config.mergeItem2, it->second->config.itemType);
    }

    // send change to server from client A (no conflict)
    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));
    // Now the changes from client B (conflict!).
    // There are several possible outcomes:
    // - client item completely replaces server item
    // - server item completely replaces client item (update on client)
    // - server merges and updates client
    accessClientB->doSync("conflict",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_TWO_WAY)));

    // figure out how the conflict during ".conflict" was handled
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        std::auto_ptr<SyncSource> copy;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), copy.reset(it->second->createSourceA()));
        SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->beginSync(SYNC_NONE));
        int numItems = 0;
        SOURCE_ASSERT_NO_FAILURE(copy.get(), numItems = countItems(copy.get()));
        SOURCE_ASSERT_EQUAL(copy.get(), STATUS_OK, copy->endSync());
        CPPUNIT_ASSERT(numItems >= 1);
        CPPUNIT_ASSERT(numItems <= 2);
        std::cout << " \"" << it->second->config.sourceName << ": " << (numItems == 1 ? "conflicting items were merged" : "both of the conflicting items were preserved") << "\" ";
        std::cout.flush();
        CPPUNIT_ASSERT_NO_THROW(copy.reset());        
    }

    // now pull the same changes into client A
    doSync("refresh",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(-1,-1,-1, 0,0,0, true, SYNC_TWO_WAY)));

    // client A and B should have identical data now
    compareDatabases();

    // Furthermore, it should be identical with the server.
    // Be extra careful and pull that data anew and compare once more.
    doSync("check",
           SyncOptions(SYNC_REFRESH_FROM_SERVER,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_REFRESH_FROM_SERVER)));
    compareDatabases();
}

// test what the server does when it has to execute a slow sync
// with identical data on client and server:
// expected behaviour is that nothing changes
void SyncTests::testTwinning() {
    // clean server and client A
    deleteAll();

    // import test data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testImport();
    }

    // send to server
    doSync("send", SyncOptions(SYNC_TWO_WAY));

    // ensure that client has the same data, thus ignoring data conversion
    // issues (those are covered by testItems())
    refreshClient();

    // copy to client B to have another copy
    accessClientB->refreshClient();

    // slow sync should not change anything
    doSync("twinning", SyncOptions(SYNC_TWO_WAY));

    // check
    compareDatabases();
}

// tests one-way sync from server:
// - get both clients and server in sync with no items anywhere
// - add one item on first client, copy to server
// - add a different item on second client, one-way-from-server
// - two-way sync with first client
// => one item on first client, two on second
// - delete on first client, sync that to second client
//   via two-way sync + one-way-from-server
// => one item left on second client (the one inserted locally)
void SyncTests::testOneWayFromServer() {
    // no items anywhere
    deleteAll();
    accessClientB->refreshClient();

    // check that everything is empty, also resets change tracking
    // in second sources of each client
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add one item on first client, copy to server, and check change tracking via second source
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 200, 1);
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add a different item on second client, one-way-from-server
    // => one item added locally, none sent to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 2, 1);

        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync("recv",
                          SyncOptions(SYNC_ONE_WAY_FROM_SERVER,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_ONE_WAY_FROM_SERVER)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 2, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // two-way sync with first client for verification
    // => no changes
    doSync("check",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // delete items on clientA, sync to server
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);

        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // sync the same change to second client
    // => one item left (the one inserted locally)
    accessClientB->doSync("delete",
                          SyncOptions(SYNC_ONE_WAY_FROM_SERVER,
                                      CheckSyncReport(0,0,1, 0,0,0, true, SYNC_ONE_WAY_FROM_SERVER)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
}

// tests one-way sync from client:
// - get both clients and server in sync with no items anywhere
// - add one item on first client, copy to server
// - add a different item on second client, one-way-from-client
// - two-way sync with first client
// => two items on first client, one on second
// - delete on second client, sync that to first client
//   via one-way-from-client, two-way
// => one item left on first client (the one inserted locally)
void SyncTests::testOneWayFromClient() {
    // no items anywhere
    deleteAll();
    accessClientB->deleteAll();

    // check that everything is empty, also resets change tracking
    // in second sources of each client
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add one item on first client, copy to server, and check change tracking via second source
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 1, 1);
    }
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // add a different item on second client, one-way-from-client
    // => no item added locally, one sent to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 2, 1);

        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync("send",
                          SyncOptions(SYNC_ONE_WAY_FROM_CLIENT,
                                      CheckSyncReport(0,0,0, 1,0,0, true, SYNC_ONE_WAY_FROM_CLIENT)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // two-way sync with client A for verification
    // => receive one item
    doSync("check",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 2, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // delete items on client B, sync to server
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        it->second->deleteAll(it->second->createSourceA);

        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
    accessClientB->doSync("delete",
                          SyncOptions(SYNC_ONE_WAY_FROM_CLIENT,
                                      CheckSyncReport(0,0,0, 0,0,1, true, SYNC_ONE_WAY_FROM_CLIENT)));
    for (it = accessClientB->sources.begin(); it != accessClientB->sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }

    // sync the same change to client A
    // => one item left (the one inserted locally)
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));
    for (it = sources.begin(); it != sources.end(); ++it) {
        if (it->second->config.createSourceB) {
            std::auto_ptr<SyncSource> source;
            SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceB()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countNewItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 1, countDeletedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), 0, countUpdatedItems(source.get()));
            SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
            CPPUNIT_ASSERT_NO_THROW(source.reset());
        }
    }
}

// get engine ready, then use it to convert our test items
// to and from the internal field list
void SyncTests::testConversion() {
    bool success = false;
    SyncOptions::Callback_t callback = boost::bind(&SyncTests::doConversionCallback, this, &success, _1, _2);

    doSync(SyncOptions(SYNC_TWO_WAY, CheckSyncReport(-1,-1,-1, -1,-1,-1, false))
           .setStartCallback(callback));
    CPPUNIT_ASSERT(success);
}

bool SyncTests::doConversionCallback(bool *success,
                                     EvolutionSyncClient &syncClient,
                                     SyncOptions &options) {
    *success = false;

    for (source_it it = sources.begin(); it != sources.end(); ++it) {
        const ClientTest::Config *config = &it->second->config;
        EvolutionSyncSource *source = syncClient.findSource(config->sourceName);
        CPPUNIT_ASSERT(source);

        std::string type = source->getNativeDatatypeName();
        if (type.empty()) {
            continue;
        }

        std::list<std::string> items;
        std::string testcases;
        ClientTest::getItems(config->testcases, items, testcases);
        std::string converted = getCurrentTest();
        converted += ".converted.";
        converted += config->sourceName;
        converted += ".dat";
        simplifyFilename(converted);
        std::ofstream out(converted.c_str());
        BOOST_FOREACH(const string &item, items) {
            string convertedItem = item;
            if(!sysync::DataConversion(syncClient.getSession().get(),
                                       type.c_str(),
                                       type.c_str(),
                                       convertedItem)) {
                SE_LOG_ERROR(NULL, NULL, "failed parsing as %s:\n%s",
                             type.c_str(),
                             item.c_str());
            } else {
                out << convertedItem << "\n";
            }
        }
        out.close();
        CPPUNIT_ASSERT(config->compare(client, testcases.c_str(), converted.c_str()));
    }

    // abort sync after completing the test successfully (no exception so far!)
    *success = true;
    return true;
}

// creates several items, transmits them back and forth and
// then compares which of them have been preserved
void SyncTests::testItems() {
    // clean server and first test database
    deleteAll();

    // import data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testImport();
    }

    // transfer from client A to server to client B
    doSync("send", SyncOptions(SYNC_TWO_WAY).setWBXML(true));
    accessClientB->refreshClient(SyncOptions().setWBXML(true));

    compareDatabases();
}

// creates several items, transmits them back and forth and
// then compares which of them have been preserved
void SyncTests::testItemsXML() {
    // clean server and first test database
    deleteAll();

    // import data
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->testImport();
    }

    // transfer from client A to server to client B using the non-default XML format
    doSync("send", SyncOptions(SYNC_TWO_WAY).setWBXML(false));
    accessClientB->refreshClient(SyncOptions().setWBXML(false));

    compareDatabases();
}

// tests the following sequence of events:
// - both clients in sync with server
// - client 1 adds item
// - client 1 updates the same item
// - client 2 gets item: the client should be asked to add the item
//
// However it has been observed that sometimes the item was sent as "update"
// for a non-existant local item. This is a server bug, the client does not
// have to handle that. See
// http://forge.objectweb.org/tracker/?func=detail&atid=100096&aid=305018&group_id=96
void SyncTests::testAddUpdate() {
    // clean server and both test databases
    deleteAll();
    accessClientB->refreshClient();

    // add item
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insert(it->second->createSourceA, it->second->config.insertItem, it->second->config.itemType);
    }
    doSync("add",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 1,0,0, true, SYNC_TWO_WAY)));

    // update it
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->update(it->second->createSourceB, it->second->config.updateItem, it->second->config.itemType);
    }
    doSync("update",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,1,0, true, SYNC_TWO_WAY)));

    // now download the updated item into the second client
    accessClientB->doSync("recv",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(1,0,0, 0,0,0, true, SYNC_TWO_WAY)));

    // compare the two databases
    compareDatabases();
}

//
// stress tests: execute some of the normal operations,
// but with large number of artificially generated items
//

// two-way sync with clean client/server,
// followed by slow sync and comparison
// via second client
void SyncTests::testManyItems() {
    // clean server and client A
    deleteAll();

    // import artificial data: make them large to generate some
    // real traffic and test buffer handling
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        it->second->insertManyItems(it->second->createSourceA, 0, it->second->config.numItems, 2000);
    }

    // send data to server
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, -1,0,0, true, SYNC_TWO_WAY),
                       64 * 1024, 64 * 1024, true));

    // ensure that client has the same data, ignoring data conversion
    // issues (those are covered by testItems())
    refreshClient();

    // also copy to second client
    accessClientB->refreshClient();

    // slow sync now should not change anything
    doSync("twinning",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(-1,-1,-1, -1,-1,-1, true, SYNC_SLOW),
                       64 * 1024, 64 * 1024, true));

    // compare
    compareDatabases();
}

/**
 * - get client A, server, client B in sync with one item
 * - force slow sync in A: must not duplicate items, but may update it locally
 * - refresh client B (in case that the item was updated)
 * - delete item in B and server via two-way sync
 * - refresh-from-server in B to check that item is gone
 * - two-way in A: must delete the item
 */
void SyncTests::testSlowSyncSemantic()
{
    // set up one item everywhere
    doCopy();

    // slow in A
    doSync("slow",
           SyncOptions(SYNC_SLOW,
                       CheckSyncReport(0,-1,0, -1,-1,0, true, SYNC_SLOW)));

    // refresh B, delete item
    accessClientB->doSync("refresh",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,-1,0, 0,0,0, true, SYNC_TWO_WAY)));
    BOOST_FOREACH(source_array_t::value_type &source_pair, accessClientB->sources)  {
        source_pair.second->deleteAll(source_pair.second->createSourceA);
    }
    accessClientB->doSync("delete",
                          SyncOptions(SYNC_TWO_WAY,
                                      CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    accessClientB->doSync("check",
                          SyncOptions(SYNC_REFRESH_FROM_SERVER,
                                      CheckSyncReport(0,0,0, 0,0,0, true, SYNC_REFRESH_FROM_SERVER)));

    // now the item should also be deleted on A
    doSync("delete",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,1, 0,0,0, true, SYNC_TWO_WAY)));
}

/**
 * check that refresh-from-server works correctly:
 * - create the same item on A, server, B via testCopy()
 * - refresh B (one item deleted, one created)
 * - delete item on A and server
 * - refresh B (one item deleted)
 */
void SyncTests::testComplexRefreshFromServerSemantic()
{
    testCopy();

    // check refresh with one item on server
    accessClientB->doSync("refresh-one",
                          SyncOptions(SYNC_REFRESH_FROM_SERVER,
                                      CheckSyncReport(1,0,1, 0,0,0, true, SYNC_REFRESH_FROM_SERVER)));

    // delete that item via A, check again
    BOOST_FOREACH(source_array_t::value_type &source_pair, sources)  {
        source_pair.second->deleteAll(source_pair.second->createSourceA);
    }
    doSync("delete-item",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, 0,0,1, true, SYNC_TWO_WAY)));
    accessClientB->doSync("refresh-none",
                          SyncOptions(SYNC_REFRESH_FROM_SERVER,
                                      CheckSyncReport(0,0,1, 0,0,0, true, SYNC_REFRESH_FROM_SERVER)));
}

/**
 * implements testMaxMsg(), testLargeObject(), testLargeObjectEncoded()
 * using a sequence of items with varying sizes
 */
void SyncTests::doVarSizes(bool withMaxMsgSize,
                           bool withLargeObject) {
    int maxMsgSize = 8 * 1024;
    const char* maxItemSize = getenv("CLIENT_TEST_MAX_ITEMSIZE");
    int tmpSize = maxItemSize ? atoi(maxItemSize) : 0;
    if(tmpSize > 0) 
        maxMsgSize = tmpSize;

    // clean server and client A
    deleteAll();

    // insert items, doubling their size, then restart with small size
    source_it it;
    for (it = sources.begin(); it != sources.end(); ++it) {
        int item = 1;
        for (int i = 0; i < 2; i++ ) {
            int size = 1;
            while (size < 2 * maxMsgSize) {
                it->second->insertManyItems(it->second->createSourceA, item, 1, (int)strlen(it->second->config.templateItem) + 10 + size);
                size *= 2;
                item++;
            }
        }
    }

    // transfer to server
    doSync("send",
           SyncOptions(SYNC_TWO_WAY,
                       CheckSyncReport(0,0,0, -1,0,0, true, SYNC_TWO_WAY), // number of items sent to server depends on source
                       withMaxMsgSize ? maxMsgSize : 0,
                       withMaxMsgSize ? maxMsgSize * 100 : 0,
                       withLargeObject));

    // copy to second client
    accessClientB->doSync("recv",
                          SyncOptions(SYNC_REFRESH_FROM_SERVER,
                                      CheckSyncReport(-1,0,-1, 0,0,0, true, SYNC_REFRESH_FROM_SERVER), // number of items received from server depends on source
                                      withLargeObject ? maxMsgSize : withMaxMsgSize ? maxMsgSize * 100 /* large enough so that server can sent the largest item */ : 0,
                                      withMaxMsgSize ? maxMsgSize * 100 : 0,
                                      withLargeObject));

    // compare
    compareDatabases();
}

class TransportFaultInjector : public TransportWrapper{
public:
    TransportFaultInjector()
         :TransportWrapper() {
    }

    ~TransportFaultInjector() {
    }

    virtual void send(const char *data, size_t len)
    {
        if (m_interruptAtMessage == m_messageCount) {
            SE_LOG_DEBUG(NULL, NULL, "TransportFaultInjector: interrupt before sending message #%d", m_messageCount);
        }
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
            m_messageCount > m_interruptAtMessage) {
            throw string("TransportFaultInjector: interrupt before send");
        }
    
        m_wrappedAgent->send(data, len);

        m_status = m_wrappedAgent->wait();
        
        if (m_interruptAtMessage == m_messageCount) {
            SE_LOG_DEBUG(NULL, NULL, "TransportFaultInjector: interrupt after receiving reply #%d", m_messageCount);
        }
        m_messageCount++;
        if (m_interruptAtMessage >= 0 &&
            m_messageCount > m_interruptAtMessage) {
            m_status = FAILED;
        }
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * Emulates a user suspend just after receving response 
 * from server.
 */
class UserSuspendInjector : public TransportWrapper{
public:
    UserSuspendInjector()
         :TransportWrapper() {
    }

    ~UserSuspendInjector() {
    }

    virtual void send(const char *data, size_t len)
    {
        m_wrappedAgent->send(data, len);
        m_status = m_wrappedAgent->wait();
    }

    virtual void getReply(const char *&data, size_t &len, std::string &contentType) {
        if (m_status == FAILED) {
            data = "";
            len = 0;
        } else {
            if (m_interruptAtMessage == m_messageCount) {
                 SE_LOG_DEBUG(NULL, NULL, "UserSuspendInjector: user suspend after getting reply #%d", m_messageCount);
            }
            m_messageCount++;
            if (m_interruptAtMessage >= 0 &&
                    m_messageCount > m_interruptAtMessage) {
                m_options->m_isSuspended = true;
            }
            m_wrappedAgent->getReply(data, len, contentType);
        }
    }
};

/**
 * This function covers different error scenarios that can occur
 * during real synchronization. To pass, clients must either force a
 * slow synchronization after a failed synchronization or implement
 * the error handling described in the design guide (track server's
 * status for added/updated/deleted items and resend unacknowledged
 * changes).
 *
 * The items used during these tests are synthetic. They are
 * constructed so that normally a server should be able to handle
 * twinning during a slow sync correctly.
 *
 * Errors are injected into a synchronization by wrapping the normal
 * HTTP transport agent. The wrapper enumerates messages sent between
 * client and server (i.e., one message exchange increments the
 * counter by two), starting from zero. It "cuts" the connection before
 * sending out the next message to the server respectively after the 
 * server has replied, but before returning the reply to the client.
 * The first case simulates a lost message from the client to the server
 * and the second case a lost message from the server to the client.
 *
 * The expected result is the same as in an uninterrupted sync, which
 * is done once at the beginning.
 *
 * Each test goes through the following steps:
 * - client A and B reset local data store
 * - client A creates 3 new items, remembers LUIDs
 * - refresh-from-client A sync
 * - refresh-from-client B sync
 * - client B creates 3 different items, remembers LUIDs
 * - client B syncs
 * - client A syncs => A, B, server are in sync
 * - client A modifies his items (depends on test) and
 *   sends changes to server => server has changes for B
 * - client B modifies his items (depends on test)
 * - client B syncs, transport wrapper simulates lost message n
 * - client B syncs again, resuming synchronization if possible or
 *   slow sync otherwise (responsibility of the client!)
 * - client A syncs (not tested yet: A should be sent exactly the changes made by B)
 * - test that A and B contain same items
 * - test that A contains the same items as the uninterrupted reference run
 * - repeat the steps above ranging starting with lost message 0 until no
 *   message got lost
 *
 * Set the CLIENT_TEST_INTERRUPT_AT env variable to a message number
 * >= 0 to execute one uninterrupted run and then interrupt at that
 * message.
 */
void SyncTests::doInterruptResume(int changes, 
                  boost::shared_ptr<TransportWrapper> wrapper)
{
    int interruptAtMessage = -1;
    const char *t = getenv("CLIENT_TEST_INTERRUPT_AT");
    int requestedInterruptAt = t ? atoi(t) : -1;
    const char *s = getenv("CLIENT_TEST_INTERRUPT_SLEEP");
    int sleep_t = s ? atoi(s) : 0;
    size_t i;
    std::string refFileBase = getCurrentTest() + ".ref.";
    bool equal = true;

    while (true) {
        char buffer[80];
        sprintf(buffer, "%d", interruptAtMessage);
        const char *prefix = interruptAtMessage == -1 ? "complete" : buffer;
        SyncPrefix prefixA(prefix, *this);
        SyncPrefix prefixB(prefix, *accessClientB);

        std::vector< std::list<std::string> > clientAluids;
        std::vector< std::list<std::string> > clientBluids;

        // create new items in client A and sync to server
        clientAluids.resize(sources.size());
        for (i = 0; i < sources.size(); i++) {
            sources[i].second->deleteAll(sources[i].second->createSourceA);
            clientAluids[i] =
                sources[i].second->insertManyItems(sources[i].second->createSourceA,
                                                   1, 3, 0);
        }
        doSync("fromA", SyncOptions(SYNC_REFRESH_FROM_CLIENT));

        // init client B and add its items to server and client A
        accessClientB->doSync("initB", SyncOptions(SYNC_REFRESH_FROM_SERVER));
        clientBluids.resize(sources.size());
        for (i = 0; i < sources.size(); i++) {
            clientBluids[i] =
                accessClientB->sources[i].second->insertManyItems(accessClientB->sources[i].second->createSourceA,
                                                                  11, 3, 0);
        }
        accessClientB->doSync("fromB", SyncOptions(SYNC_TWO_WAY));
        doSync("updateA", SyncOptions(SYNC_TWO_WAY));

        // => client A, B and server in sync with a total of six items

        // make changes as requested on client A and sync to server
        for (i = 0; i < sources.size(); i++) {
            if (changes & SERVER_ADD) {
                sources[i].second->insertManyItems(sources[i].second->createSourceA,
                                                   4, 1, 0);
            }
            if (changes & SERVER_REMOVE) {
                // remove second item
                removeItem(sources[i].second->createSourceA,
                           *(++clientAluids[i].begin()));
            }
            if (changes & SERVER_UPDATE) {
                // update third item
                updateItem(sources[i].second->createSourceA,
                           *(++ ++clientAluids[i].begin()),
                           sources[i].second->createItem(3, "updated", 0).c_str());
                                              
            }
        }
        if (changes & (SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE)) {
            doSync("changesFromA", SyncOptions(SYNC_TWO_WAY));
        }

        // make changes as requested on client B
        for (i = 0; i < sources.size(); i++) {
            if (changes & CLIENT_ADD) {
                accessClientB->sources[i].second->insertManyItems(accessClientB->sources[i].second->createSourceA,
                                                                  14, 1, 0);
            }
            if (changes & CLIENT_REMOVE) {
                // remove second item
                removeItem(accessClientB->sources[i].second->createSourceA,
                           *(++clientBluids[i].begin()));
            }
            if (changes & CLIENT_UPDATE) {
                // update third item
                updateItem(accessClientB->sources[i].second->createSourceA,
                           *(++ ++clientBluids[i].begin()),
                           accessClientB->sources[i].second->createItem(13, "updated", 0).c_str());
            }
        }

        // Now do an interrupted sync between B and server.
        // The explicit delete of the TransportAgent is suppressed
        // by overloading the delete operator.
        int wasInterrupted;
        {
            wrapper->reset();
            wrapper->setInterruptAtMessage(interruptAtMessage);
            accessClientB->doSync("changesFromB",
                                  SyncOptions(SYNC_TWO_WAY,
                                              CheckSyncReport(-1, -1, -1, -1,
                                                  -1, -1, false)).setTransportAgent(wrapper));
            wasInterrupted = interruptAtMessage != -1 &&
                wrapper->getMessageCount() <= interruptAtMessage;
        }

        if (interruptAtMessage != -1) {
            if (wasInterrupted) {
                // uninterrupted sync, done
                break;
            }

            // continue, wait until server timeout
            if(sleep_t) 
                sleep (sleep_t);
            accessClientB->doSync("retryB", SyncOptions(SYNC_TWO_WAY));
        }

        // copy changes to client A
        doSync("toA", SyncOptions(SYNC_TWO_WAY));

        // compare client A and B
        if (interruptAtMessage != -1 &&
            !compareDatabases(refFileBase.c_str(), false)) {
            equal = false;
            std::cout << "====> comparison of client B against reference file(s) failed after interrupting at message #" <<
                interruptAtMessage << std::endl;
            std::cout.flush();
        }
        if (!compareDatabases(NULL, false)) {
            equal = false;
            std::cout << "====> comparison of client A and B failed after interrupting at message #" <<
                interruptAtMessage << std::endl;
            std::cout.flush();
        }

        // save reference files from uninterrupted run?
        if (interruptAtMessage == -1) {
            for (source_it it = sources.begin();
                 it != sources.end();
                 ++it) {
                std::string refFile = refFileBase;
                refFile += it->second->config.sourceName;
                refFile += ".dat";
                simplifyFilename(refFile);
                std::auto_ptr<SyncSource> source;
                SOURCE_ASSERT_NO_FAILURE(source.get(), source.reset(it->second->createSourceA()));
                SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->beginSync(SYNC_NONE));
                SOURCE_ASSERT_EQUAL(source.get(), 0, it->second->config.dump(client, *source.get(), refFile.c_str()));
                SOURCE_ASSERT_EQUAL(source.get(), STATUS_OK, source->endSync());
                CPPUNIT_ASSERT_NO_THROW(source.reset());
            }
        }

        // pick next iterration
        if (requestedInterruptAt != -1) {
            // only do one interrupted run of the test
            if (requestedInterruptAt == interruptAtMessage) {
                break;
            } else {
                interruptAtMessage = requestedInterruptAt;
            }
        } else {
            // interrupt one message later than before
            interruptAtMessage++;
        }
    }

    CPPUNIT_ASSERT(equal);
}

void SyncTests::testInterruptResumeClientAdd()
{
    doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeClientRemove()
{
    doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeClientUpdate()
{
    doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerAdd()
{
    doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerRemove()
{
    doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeServerUpdate()
{
    doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testInterruptResumeFull()
{
    doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                      SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new TransportFaultInjector()));
}

void SyncTests::testUserSuspendClientAdd()
{
    doInterruptResume(CLIENT_ADD, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendClientRemove()
{
    doInterruptResume(CLIENT_REMOVE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendClientUpdate()
{
    doInterruptResume(CLIENT_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerAdd()
{
    doInterruptResume(SERVER_ADD, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerRemove()
{
    doInterruptResume(SERVER_REMOVE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendServerUpdate()
{
    doInterruptResume(SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}

void SyncTests::testUserSuspendFull()
{
    doInterruptResume(CLIENT_ADD|CLIENT_REMOVE|CLIENT_UPDATE|
                      SERVER_ADD|SERVER_REMOVE|SERVER_UPDATE, boost::shared_ptr<TransportWrapper> (new UserSuspendInjector()));
}


void SyncTests::doSync(const SyncOptions &options)
{
    int res = 0;
    static int syncCounter = 0;
    static std::string lastTest;
    std::stringstream logstream;

    // reset counter when switching tests
    if (lastTest != getCurrentTest()) {
        syncCounter = 0;
        lastTest = getCurrentTest();
    }

    std::string prefix;
    prefix.reserve(80);
    for (std::list<std::string>::iterator it = logPrefixes.begin();
         it != logPrefixes.end();
         ++it) {
        prefix += ".";
        prefix += *it;
    }
    if (!prefix.empty()) {
        printf(" %s", prefix.c_str() + 1);
        fflush(stdout);
    }

    logstream /* << std::setw(4) << std::setfill('0') << syncCounter << "_" */ << getCurrentTest()
                 << prefix
                 << ".client." << (accessClientB ? "A" : "B");
    std::string logname = logstream.str();
    simplifyFilename(logname);
    syncCounter++;

    SE_LOG_DEBUG(NULL, NULL, "%d. starting %s with sync mode %s",
                 syncCounter, logname.c_str(), PrettyPrintSyncMode(options.m_syncMode).c_str());

    try {
        res = client.doSync(sourceArray,
                            logname,
                            options);

        client.postSync(res, logname);
    } catch (CppUnit::Exception &ex) {
        res = 1;
        client.postSync(res, logname);

        // report the original exception without altering the source line
        throw;
    } catch (...) {
        res = 1;
        client.postSync(res, logname);

        // this logs the original exception using CPPUnit mechanisms,
        // with current line as source
        CPPUNIT_ASSERT_NO_THROW(throw);
    }
}


/** generates tests on demand based on what the client supports */
class ClientTestFactory : public CppUnit::TestFactory {
public:
    ClientTestFactory(ClientTest &c) :
        client(c) {}

    virtual CppUnit::Test *makeTest() {
        int source;
        CppUnit::TestSuite *alltests = new CppUnit::TestSuite("Client");
        CppUnit::TestSuite *tests;

        // create local source tests
        tests = new CppUnit::TestSuite(alltests->getName() + "::Source");
        for (source=0; source < client.getNumSources(); source++) {
            ClientTest::Config config;
            client.getSourceConfig(source, config);
            if (config.sourceName) {
                LocalTests *sourcetests =
                    client.createLocalTests(tests->getName() + "::" + config.sourceName, source, config);
                sourcetests->addTests();
                tests->addTest(FilterTest(sourcetests));
            }
        }
        alltests->addTest(FilterTest(tests));
        tests = 0;

        // create sync tests with just one source
        tests = new CppUnit::TestSuite(alltests->getName() + "::Sync");
        for (source=0; source < client.getNumSources(); source++) {
            ClientTest::Config config;
            client.getSourceConfig(source, config);
            if (config.sourceName) {
                std::vector<int> sources;
                sources.push_back(source);
                SyncTests *synctests =
                    client.createSyncTests(tests->getName() + "::" + config.sourceName, sources);
                synctests->addTests();
                tests->addTest(FilterTest(synctests));
            }
        }

        // create sync tests with all sources enabled, unless we only have one:
        // that would be identical to the test above
        std::vector<int> sources;
        std::string name, name_reversed;
        for (source=0; source < client.getNumSources(); source++) {
            ClientTest::Config config;
            client.getSourceConfig(source, config);
            if (config.sourceName) {
                sources.push_back(source);
                if (name.size() > 0) {
                    name += "_";
                    name_reversed = std::string("_") + name_reversed;
                }
                name += config.sourceName;
                name_reversed = config.sourceName + name_reversed;
            }
        }
        if (sources.size() > 1) {
            SyncTests *synctests =
                client.createSyncTests(tests->getName() + "::" + name, sources);
            synctests->addTests();
            tests->addTest(FilterTest(synctests));
            synctests = 0;

            // now also in reversed order - who knows, it might make a difference
            std::reverse(sources.begin(), sources.end());
            synctests =
                client.createSyncTests(tests->getName() + "::" + name_reversed, sources);
            synctests->addTests();
            tests->addTest(FilterTest(synctests));
            synctests = 0;
        }

        alltests->addTest(FilterTest(tests));
        tests = 0;

        return alltests;
    }

private:
    ClientTest &client;
};

void ClientTest::registerTests()
{
    factory = (void *)new ClientTestFactory(*this);
    CppUnit::TestFactoryRegistry::getRegistry().registerFactory((CppUnit::TestFactory *)factory);
}

ClientTest::ClientTest(int serverSleepSec, const std::string &serverLog) :
    serverSleepSeconds(serverSleepSec),
    serverLogFileName(serverLog),
    factory(NULL)
{
}

ClientTest::~ClientTest()
{
    if(factory) {
        CppUnit::TestFactoryRegistry::getRegistry().unregisterFactory((CppUnit::TestFactory *)factory);
        delete (CppUnit::TestFactory *)factory;
        factory = 0;
    }
}

LocalTests *ClientTest::createLocalTests(const std::string &name, int sourceParam, ClientTest::Config &co)
{
    return new LocalTests(name, *this, sourceParam, co);
}

SyncTests *ClientTest::createSyncTests(const std::string &name, std::vector<int> sourceIndices, bool isClientA)
{
    return new SyncTests(name, *this, sourceIndices, isClientA);
}

int ClientTest::dump(ClientTest &client, SyncSource &source, const char *file)
{
    std::auto_ptr<SyncItem> item;
    BackupReport report;
    VolatileConfigNode node;

    rm_r(file);
    mkdir_p(file);
    source.backupData(file, node, report);
    return 0;
}

void ClientTest::getItems(const char *file, list<string> &items, std::string &testcases)
{
    items.clear();

    // import the file
    std::ifstream input;
    string server = getenv("CLIENT_TEST_SERVER");
    testcases = string(file) + '.' + server +".tem";
    input.open(testcases.c_str());

    if(input.fail()) {
        testcases = file;
        input.open(testcases.c_str());
    }
    CPPUNIT_ASSERT(!input.bad());
    CPPUNIT_ASSERT(input.is_open());
    std::string data, line;
    while (input) {
        bool wasend = false;
        do {
            getline(input, line);
            CPPUNIT_ASSERT(!input.bad());
            // empty lines directly after line which starts with END mark end of record;
            // check for END necessary becayse vCard 2.1 ENCODING=BASE64 may have empty lines in body of VCARD!
            if ((line != "\r" && line.size() > 0) || !wasend) {
                data += line;
                data += "\n";
            } else {
                if (!data.empty()) {
                    items.push_back(data);
                }
                data = "";
            }
            wasend = !line.compare(0, 4, "END:");
        } while(!input.eof());
    }
    if (!data.empty()) {
        items.push_back(data);
    }
}

int ClientTest::import(ClientTest &client, SyncSource &source, const char *file, std::string &realfile)
{
    list<string> items;
    getItems(file, items, realfile);
    BOOST_FOREACH(string &data, items) {
        importItem(&source, data);
    }
    return 0;
}

bool ClientTest::compare(ClientTest &client, const char *fileA, const char *fileB)
{
    std::string cmdstr = std::string("env PATH=.:$PATH synccompare ") + fileA + " " + fileB;
    setenv("CLIENT_TEST_HEADER", "\n\n", 1);
    setenv("CLIENT_TEST_LEFT_NAME", fileA, 1);
    setenv("CLIENT_TEST_RIGHT_NAME", fileB, 1);
    setenv("CLIENT_TEST_REMOVED", "only in left file", 1);
    setenv("CLIENT_TEST_ADDED", "only in right file", 1);
    const char* compareLog = getenv("CLIENT_TEST_COMPARE_LOG");
    if(compareLog && strlen(compareLog))
    {
       string tmpfile = "____compare.log";
       cmdstr =string("bash -c 'set -o pipefail;") + cmdstr;
       cmdstr += " 2>&1|tee " +tmpfile+"'";
    }
    bool success = system(cmdstr.c_str()) == 0;
    if (!success) {
        printf("failed: env CLIENT_TEST_SERVER=%s PATH=.:$PATH synccompare %s %s\n",
               getenv("CLIENT_TEST_SERVER") ? getenv("CLIENT_TEST_SERVER") : "",
               fileA, fileB);
    }
    return success;
}


#ifndef WIN32
#include <fcntl.h>
#endif

void ClientTest::postSync(int res, const std::string &logname)
{
#ifdef WIN32
    Sleep(serverSleepSeconds * 1000);
#else
    sleep(serverSleepSeconds);

    // make a copy of the server's log (if found), then truncate it
    if (serverLogFileName.size()) {
        int fd = open(serverLogFileName.c_str(), O_RDWR);

        if (fd >= 0) {
            std::string cmd = std::string("cp ") + serverLogFileName + " " + logname + ".server.log";
            if (system(cmd.c_str())) {
                fprintf(stderr, "copying log file failed: %s\n", cmd.c_str());
            }
            if (ftruncate(fd, 0)) {
                perror("truncating log file");
            }
        } else {
            perror(serverLogFileName.c_str());
        }
    }
#endif
}

void ClientTest::getTestData(const char *type, Config &config)
{
    memset(&config, 0, sizeof(config));
    char *numitems = getenv("CLIENT_TEST_NUM_ITEMS");
    config.numItems = numitems ? atoi(numitems) : 100;
    config.retrySync = true;
    config.sourceKnowsItemSemantic = true;
    config.itemType = "";
    config.import = import;
    config.dump = dump;
    config.compare = compare;

    if (!strcmp(type, "vcard30")) {
        config.sourceName = "vcard30";
        config.sourceNameServerTemplate = "addressbook";
        config.uri = "card3"; // ScheduleWorld
        config.type = "text/vcard";
        config.insertItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:FALSE\n"
            "NOTE:<<REVISION>>\n"
            "END:VCARD\n";
        config.updateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "X-EVOLUTION-FILE-AS:Doe\\, Joan\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* adds a second phone number: */
        config.complexUpdateItem =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "X-EVOLUTION-FILE-AS:Doe\\, Joan\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "TEL;TYPE=HOME;TYPE=VOICE:home 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* add a telephone number, email and X-AIM to initial item */
        config.mergeItem1 =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:FALSE\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "EMAIL:john.doe@work.com\n"
            "X-AIM:AIM JOHN\n"
            "END:VCARD\n";
        config.mergeItem2 =
            "BEGIN:VCARD\n"
            "VERSION:3.0\n"
            "TITLE:developer\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "TEL;TYPE=WORK;TYPE=VOICE:123456\n"
            "X-EVOLUTION-FILE-AS:Doe\\, John\n"
            "X-MOZILLA-HTML:TRUE\n"
            "BDAY:2006-01-08\n"
            "END:VCARD\n";
        config.templateItem = config.insertItem;
        config.uniqueProperties = "FN:N:X-EVOLUTION-FILE-AS";
        config.sizeProperty = "NOTE";
        config.testcases = "testcases/vcard30.vcf";
    } else if (!strcmp(type, "vcard21")) {
        config.sourceName = "vcard21";
        config.sourceNameServerTemplate = "addressbook";
        config.uri = "card"; // Funambol
        config.type = "text/x-vcard";
        config.insertItem =
            "BEGIN:VCARD\n"
            "VERSION:2.1\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "X-MOZILLA-HTML:FALSE\n"
            "NOTE:<<REVISION>>\n"
            "END:VCARD\n";
        config.updateItem =
            "BEGIN:VCARD\n"
            "VERSION:2.1\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* adds a second phone number: */
        config.complexUpdateItem =
            "BEGIN:VCARD\n"
            "VERSION:2.1\n"
            "TITLE:tester\n"
            "FN:Joan Doe\n"
            "N:Doe;Joan;;;\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "TEL;TYPE=HOME;TYPE=VOICE:home 2\n"
            "BDAY:2006-01-08\n"
            "X-MOZILLA-HTML:TRUE\n"
            "END:VCARD\n";
        /* add email and X-AIM to initial item */
        config.mergeItem1 =
            "BEGIN:VCARD\n"
            "VERSION:2.1\n"
            "TITLE:tester\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "X-MOZILLA-HTML:FALSE\n"
            "TEL;TYPE=WORK;TYPE=VOICE:business 1\n"
            "EMAIL:john.doe@work.com\n"
            "X-AIM:AIM JOHN\n"
            "END:VCARD\n";
        /* change X-MOZILLA-HTML */
        config.mergeItem2 =
            "BEGIN:VCARD\n"
            "VERSION:2.1\n"
            "TITLE:developer\n"
            "FN:John Doe\n"
            "N:Doe;John;;;\n"
            "X-MOZILLA-HTML:TRUE\n"
            "BDAY:2006-01-08\n"
            "END:VCARD\n";
        config.templateItem = config.insertItem;
        config.uniqueProperties = "FN:N";
        config.sizeProperty = "NOTE";
        config.testcases = "testcases/vcard21.vcf";
    } else if(!strcmp(type, "ical20")) {
        config.sourceName = "ical20";
        config.sourceNameServerTemplate = "calendar";
        config.uri = "cal2"; // ScheduleWorld
        config.type = "text/x-vcalendar";
        config.insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201\n"
            "CREATED:20060409T213201\n"
            "LOCATION:my office\n"
            "DESCRIPTION:let's talk<<REVISION>>\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:1\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:meeting on site\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201\n"
            "CREATED:20060409T213201\n"
            "LOCATION:big meeting room\n"
            "DESCRIPTION:nice to see you\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:1\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location and description of insertItem in testMerge(), add alarm */
        config.mergeItem1 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201\n"
            "CREATED:20060409T213201\n"
            "LOCATION:calling from home\n"
            "DESCRIPTION:let's talk\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:1\n"
            "BEGIN:VALARM\n"
            "DESCRIPTION:alarm\n"
            "ACTION:DISPLAY\n"
            "TRIGGER;VALUE=DURATION;RELATED=START:-PT15M\n"
            "END:VALARM\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location to something else, add category */
        config.mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "UID:1234567890!@#$%^&*()<>@dummy\n"
            "DTSTAMP:20060406T211449Z\n"
            "LAST-MODIFIED:20060409T213201\n"
            "CREATED:20060409T213201\n"
            "LOCATION:my office\n"
            "CATEGORIES:WORK\n"
            "DESCRIPTION:what the heck\\, let's even shout a bit\n"
            "CLASS:PUBLIC\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:1\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.parentItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "UID:20080407T193125Z-19554-727-1-50@gollum\n"
            "DTSTAMP:20080407T193125Z\n"
            "DTSTART:20080406T090000Z\n"
            "DTEND:20080406T093000Z\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:2\n"
            "SUMMARY:Recurring\n"
            "DESCRIPTION:recurs each Monday\\, 10 times\n"
            "CLASS:PUBLIC\n"
            "RRULE:FREQ=WEEKLY;COUNT=10;INTERVAL=1;BYDAY=SU\n"
            "CREATED:20080407T193241\n"
            "LAST-MODIFIED:20080407T193241\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.childItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VEVENT\n"
            "UID:20080407T193125Z-19554-727-1-50@gollum\n"
            "DTSTAMP:20080407T193125Z\n"
            "DTSTART:20080413T090000Z\n"
            "DTEND:20080413T093000Z\n"
            "TRANSP:OPAQUE\n"
            "SEQUENCE:7\n"
            "SUMMARY:Recurring: Modified\n"
            "CLASS:PUBLIC\n"
            "CREATED:20080407T193241\n"
            "LAST-MODIFIED:20080407T193647\n"
            "RECURRENCE-ID:20080413T090000Z\n"
            "DESCRIPTION:second instance modified\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.templateItem = config.insertItem;
        config.uniqueProperties = "SUMMARY:UID:LOCATION";
        config.sizeProperty = "DESCRIPTION";
        config.testcases = "testcases/ical20.ics";
    } if(!strcmp(type, "vcal10")) {
        config.sourceName = "vcal10";
        config.sourceNameServerTemplate = "calendar";
        config.uri = "cal"; // Funambol 3.0
        config.type = "text/x-vcalendar";
        config.insertItem =
            "BEGIN:VCALENDAR\n"
            "VERSION:1.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "DTSTAMP:20060406T211449Z\n"
            "LOCATION:my office\n"
            "DESCRIPTION:let's talk<<REVISION>>\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.updateItem =
            "BEGIN:VCALENDAR\n"
            "VERSION:1.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:meeting on site\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "DTSTAMP:20060406T211449Z\n"
            "LOCATION:big meeting room\n"
            "DESCRIPTION:nice to see you\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        /* change location in insertItem in testMerge() */
        config.mergeItem1 =
            "BEGIN:VCALENDAR\n"
            "VERSION:1.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "DTSTAMP:20060406T211449Z\n"
            "LOCATION:calling from home\n"
            "DESCRIPTION:let's talk\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "VERSION:1.0\n"
            "BEGIN:VEVENT\n"
            "SUMMARY:phone meeting\n"
            "DTEND:20060406T163000Z\n"
            "DTSTART:20060406T160000Z\n"
            "DTSTAMP:20060406T211449Z\n"
            "LOCATION:my office\n"
            "DESCRIPTION:what the heck, let's even shout a bit\n"
            "END:VEVENT\n"
            "END:VCALENDAR\n";
        config.templateItem = config.insertItem;
        config.uniqueProperties = "SUMMARY:UID:LOCATION";
        config.sizeProperty = "DESCRIPTION";
        config.testcases = "testcases/vcal10.ics";
    } else if(!strcmp(type, "itodo20")) {
        config.sourceName = "itodo20";
        config.sourceNameServerTemplate = "todo";
        config.uri = "task2"; // ScheduleWorld
        config.type = "text/x-vcalendar";
        config.insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me\n"
            "DESCRIPTION:to be done<<REVISION>>\n"
            "PRIORITY:0\n"
            "STATUS:IN-PROCESS\n"
            "CREATED:20060417T173712\n"
            "LAST-MODIFIED:20060417T173712\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me ASAP\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:1\n"
            "STATUS:IN-PROCESS\n"
            "CREATED:20060417T173712\n"
            "LAST-MODIFIED:20060417T173712\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        /* change summary in insertItem in testMerge() */
        config.mergeItem1 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me please\\, please\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:0\n"
            "STATUS:IN-PROCESS\n"
            "CREATED:20060417T173712\n"
            "LAST-MODIFIED:20060417T173712\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VTODO\n"
            "UID:20060417T173712Z-4360-727-1-2730@gollum\n"
            "DTSTAMP:20060417T173712Z\n"
            "SUMMARY:do me\n"
            "DESCRIPTION:to be done\n"
            "PRIORITY:7\n"
            "STATUS:IN-PROCESS\n"
            "CREATED:20060417T173712\n"
            "LAST-MODIFIED:20060417T173712\n"
            "END:VTODO\n"
            "END:VCALENDAR\n";
        config.templateItem = config.insertItem;
        config.uniqueProperties = "SUMMARY:UID";
        config.sizeProperty = "DESCRIPTION";
        config.testcases = "testcases/itodo20.ics";
    } else if(!strcmp(type, "text")) {
        // The "text" test uses iCalendar 2.0 VJOURNAL
        // as format because synccompare doesn't handle
        // plain text. A backend which wants to use this
        // test data must support importing/exporting
        // the test data in that format, see EvolutionMemoSource
        // for an example.
        config.uri = "note"; // ScheduleWorld
        config.sourceNameServerTemplate = "memo";
        config.type = "memo";
        config.itemType = "text/calendar";
        config.insertItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody text<<REVISION>>\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        config.updateItem =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary Modified\n"
            "DESCRIPTION:Summary Modified\\nBody text\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";
        /* change summary, as in updateItem, and the body in the other merge item */
        config.mergeItem1 = config.updateItem;
        config.mergeItem2 =
            "BEGIN:VCALENDAR\n"
            "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
            "VERSION:2.0\n"
            "METHOD:PUBLISH\n"
            "BEGIN:VJOURNAL\n"
            "SUMMARY:Summary\n"
            "DESCRIPTION:Summary\\nBody modified\n"
            "END:VJOURNAL\n"
            "END:VCALENDAR\n";                
        config.templateItem = config.insertItem;
        config.uniqueProperties = "SUMMARY:DESCRIPTION";
        config.sizeProperty = "DESCRIPTION";
        config.testcases = "testcases/imemo20.ics";
    }
}

void CheckSyncReport::check(SyncMLStatus status, SyncReport &report) const
{
    stringstream str;

    str << report;
    str << "----------|--------CLIENT---------|--------SERVER---------|\n";
    str << "          |  NEW  |  MOD  |  DEL  |  NEW  |  MOD  |  DEL  |\n";
    str << "----------|-----------------------------------------------|\n";
    str << StringPrintf("Expected  |  %3d  |  %3d  |  %3d  |  %3d  |  %3d  |  %3d  |\n",
                        clientAdded, clientUpdated, clientDeleted,
                        serverAdded, serverUpdated, serverDeleted);
    str << "Expected sync mode: " << PrettyPrintSyncMode(syncMode) << "\n";
    SE_LOG_INFO(NULL, NULL, "sync report:\n%s\n", str.str().c_str());

    if (mustSucceed) {
        CPPUNIT_ASSERT_EQUAL(STATUS_OK, status);
    }

    // this code is intentionally duplicated to produce nicer CPPUNIT asserts
    BOOST_FOREACH(SyncReport::value_type &entry, report) {
        const std::string &name = entry.first;
        const SyncSourceReport &source = entry.second;

        SE_LOG_DEBUG(NULL, NULL, "Checking sync source %s...", name.c_str());
        if (mustSucceed) {
            CLIENT_TEST_EQUAL(name, STATUS_OK, source.getStatus());
        }
        CLIENT_TEST_EQUAL(name, 0, source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                      SyncSourceReport::ITEM_ANY,
                                                      SyncSourceReport::ITEM_REJECT));
        CLIENT_TEST_EQUAL(name, 0, source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                      SyncSourceReport::ITEM_ANY,
                                                      SyncSourceReport::ITEM_REJECT));

        const char* checkSyncModeStr = getenv("CLIENT_TEST_NOCHECK_SYNCMODE");
        bool checkSyncMode = true;
        if (checkSyncModeStr && 
                (!strcmp(checkSyncModeStr, "1") || !strcasecmp(checkSyncModeStr, "t"))) {
            checkSyncMode = false;
        }

        if (syncMode != SYNC_NONE && checkSyncMode) {
            CLIENT_TEST_EQUAL(name, syncMode, source.getFinalSyncMode());
        }

        if (clientAdded != -1) {
            CLIENT_TEST_EQUAL(name, clientAdded,
                              source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                 SyncSourceReport::ITEM_ADDED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (clientUpdated != -1) {
            CLIENT_TEST_EQUAL(name, clientUpdated,
                              source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                 SyncSourceReport::ITEM_UPDATED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (clientDeleted != -1) {
            CLIENT_TEST_EQUAL(name, clientDeleted,
                              source.getItemStat(SyncSourceReport::ITEM_LOCAL,
                                                 SyncSourceReport::ITEM_REMOVED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }

        if (serverAdded != -1) {
            CLIENT_TEST_EQUAL(name, serverAdded,
                              source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                 SyncSourceReport::ITEM_ADDED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (serverUpdated != -1) {
            CLIENT_TEST_EQUAL(name, serverUpdated,
                              source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                 SyncSourceReport::ITEM_UPDATED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
        if (serverDeleted != -1) {
            CLIENT_TEST_EQUAL(name, serverDeleted,
                              source.getItemStat(SyncSourceReport::ITEM_REMOTE,
                                                 SyncSourceReport::ITEM_REMOVED,
                                                 SyncSourceReport::ITEM_TOTAL));
        }
    }
    SE_LOG_DEBUG(NULL, NULL, "Done with checking sync report.");
}

/** @} */
/** @endcond */
#endif // ENABLE_INTEGRATION_TESTS
