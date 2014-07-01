#! /usr/bin/python -u
# -*- coding: utf-8 -*-
# vim: set fileencoding=utf-8 :#
#
# Copyright (C) 2012 Intel Corporation
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) version 3.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301  USA

# PIM Manager specific tests, using Python unittest as framework.
#
# Run with "syncevolution", "synccompare" and "syncevo-dbus-server" in
# the PATH.
#
# Uses the normal testdbus.py infrastructure by including that file.
# Can be run directly from the SyncEvolution source code or after
# copying test/testdbus.py and src/dbus/server/pim/testpim.py into the
# same directory.

import os
import errno
import sys
import inspect
import unittest
import time
import copy
import subprocess
import dbus
import traceback
import re
import itertools
import codecs
import pprint
import shutil

import localed

# Update path so that testdbus.py can be found.
pimFolder = os.path.realpath(os.path.abspath(os.path.split(inspect.getfile(inspect.currentframe()))[0]))
if pimFolder not in sys.path:
     sys.path.insert(0, pimFolder)
testFolder = os.path.realpath(os.path.abspath(os.path.join(os.path.split(inspect.getfile(inspect.currentframe()))[0], "../../../../test")))
if testFolder not in sys.path:
    sys.path.insert(0, testFolder)

# Rely on the glib/gobject compatibility import code in test-dbus.py.
from testdbus import glib, gobject

from testdbus import DBusUtil, timeout, Timeout, property, usingValgrind, xdg_root, bus, logging, NullLogging, loop
import testdbus

def timeFunction(func, *args1, **args2):
     start = time.time()
     res = { }
     args2['reply_handler'] = lambda x: (res.__setitem__('okay', x), loop.quit())
     args2['error_handler'] = lambda x: (res.__setitem__('failed', x), loop.quit())
     func(*args1, **args2)
     loop.run()
     end = time.time()
     if 'failed' in res:
          raise res['failed']
     return (end - start, res['okay'])

@unittest.skip("not a real test")
class ContactsView(dbus.service.Object, unittest.TestCase):
     '''Implements ViewAgent, starts a search and mirrors the remote state locally.'''

     counter = 1

     def __init__(self, manager):
          '''Create ViewAgent with the chosen path.'''
          self.manager = manager
          # Ensure unique path across different tests.
          self.path = '/org/syncevolution/testpim%d' % ContactsView.counter
          ContactsView.counter = ContactsView.counter + 1
          self.view = None
          # List of encountered errors in ViewAgent, should always be empty.
          self.errors = []
          # Currently known contact data, size matches view.
          # Entry is a string (just the ID is known),
          # a tuple (ID + time when reading started), or
          # a dictionary (actual content known).
          self.contacts = []
          # Change events, as list of ("modified/added/removed", start, count).
          self.events = []
          # Number of times that ViewAgent.Quiescent() was called.
          self.quiescentCount = 0

          self.logging = logging

          # Called at the end of each view notification.
          # May throw exceptions, which will be recorded in self.errors.
          self.check = lambda: True

          dbus.service.Object.__init__(self, dbus.SessionBus(), self.path)
          unittest.TestCase.__init__(self)

     # Set self.check and returns a wrapper for use in runUntil.
     # The function passed in should not check self.errors, this
     # will be added by setCheck() for use in runUntil.
     def setCheck(self, check):
          if check:
               self.check = check
          else:
               check = lambda: True
               self.check = check
          return lambda: (self.assertEqual([], self.errors), check())

     def runTest(self):
          pass

     def search(self, filter):
          '''Start a search.'''
          self.viewPath = self.manager.Search(filter, self.path)
          self.view = dbus.Interface(bus.get_object(self.manager.bus_name,
                                                    self.viewPath),
                                     'org._01.pim.contacts.ViewControl')

     def close(self):
          self.view.Close()

     def getIDs(self, start, count):
          '''Return just the IDs for a range of contacts in the current view.'''
          return [isinstance(x, dict) and x['id'] or \
                       isinstance(x, tuple) and x[0] or \
                       x \
                       for x in self.contacts[start:start + count]]

     def countData(self, start, count):
          '''Number of contacts with data in the given range.'''
          total = 0
          for contact in self.contacts[start:start + count]:
               if isinstance(contact, dict):
                    total = total + 1
          return total

     def haveData(self, start, count = 1):
          '''True if all contacts in the range have data.'''
          return count == self.countData(start, count)

     def haveNoData(self, start, count = 1):
          '''True if all contacts in the range have no data.'''
          return 0 == self.countData(start, count)

     def processEvent(self, message, event):
          self.logging.log(message)
          self.events.append(event)

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsModified(self, view, start, ids):
          count = len(ids)
          self.processEvent('contacts modified: %s, start %d, ids %s' % (view, start, ids),
                            ('modified', start, count))
          try:
               self.assertEqual(view, self.viewPath)
               self.assertGreaterEqual(start, 0)
               self.assertGreater(count, 0)
               self.assertLessEqual(start + count, len(self.contacts))
               # Overwrite valid data with just the (possibly modified) ID.
               self.contacts[start:start + count] = [str(x) for x in ids]
               self.logging.printf('contacts modified => %s', self.contacts)
               self.check()
          except:
               error = traceback.format_exc()
               self.logging.printf('contacts modified: error: %s' % error)
               self.errors.append(error)


     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsAdded(self, view, start, ids):
          count = len(ids)
          self.processEvent('contacts added: %s, start %d, ids %s' % (view, start, ids),
                            ('added', start, count))
          try:
               self.assertEqual(view, self.viewPath)
               self.assertGreaterEqual(start, 0)
               self.assertGreater(count, 0)
               self.assertLessEqual(start, len(self.contacts))
               self.contacts[start:start] = [str(x) for x in ids]
               self.logging.printf('contacts added => %s', self.contacts)
               self.check()
          except:
               error = traceback.format_exc()
               self.logging.printf('contacts added: error: %s' % error)
               self.errors.append(error)

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='oias', out_signature='')
     def ContactsRemoved(self, view, start, ids):
          count = len(ids)
          self.processEvent('contacts removed: %s, start %d, ids %s' % (view, start, ids),
                            ('removed', start, count))
          try:
               self.assertEqual(view, self.viewPath)
               self.assertGreaterEqual(start, 0)
               self.assertGreater(count, 0)
               self.assertLessEqual(start + count, len(self.contacts))
               self.assertEqual(self.getIDs(start, count), ids)
               del self.contacts[start:start + count]
               self.logging.printf('contacts removed => %s', self.contacts)
               self.check()
          except:
               error = traceback.format_exc()
               self.logging.printf('contacts removed: error: %s' % error)
               self.errors.append(error)

     @dbus.service.method(dbus_interface='org._01.pim.contacts.ViewAgent',
                          in_signature='o', out_signature='')
     def Quiescent(self, view):
          # Allow exceptions from self.processEvent to be returned to caller,
          # because Quiescent() is allowed to fail. testQuiescentOptional
          # depends on that after hooking into the self.processEvent call.
          self.quiescentCount = self.quiescentCount + 1
          self.processEvent('quiescent: %s' % view,
                            ('quiescent',))
          try:
               self.check()
          except:
               error = traceback.format_exc()
               self.logging.printf('quiescent: error: %s' % error)
               self.errors.append(error)

     def read(self, start, count=1):
          '''Read the specified range of contact data.'''
          starttime = time.time()
          ids = []
          for index, entry in enumerate(self.contacts[start:start+count]):
               if isinstance(entry, str):
                    ids.append(entry)
                    self.contacts[start + index] = (entry, starttime)
          # Avoid composing too large requests because they make the
          # server unresponsive and trigger our Watchdog. Instead chop up
          # into pieces and ask for more once we get the response.
          def step(contacts, start):
               for index, contact in contacts:
                    if index >= 0:
                         self.contacts[index] = contact
               if start < len(ids):
                    end = min(start + 50, len(ids))
                    self.view.ReadContacts(ids[start:end],
                                           reply_handler=lambda contacts: step(contacts, end),
                                           error_handler=lambda error: self.errors.append(x))
          step([], 0)

class Watchdog():
     '''Send D-Bus queries regularly to the daemon and measure response time.'''
     def __init__(self, test, manager, threshold=0.1, interval=0.2):
          self.test = test
          self.manager = manager
          self.started = None
          self.results = [] # tuples of start time + duration
          self.threshold = threshold
          self.interval = interval
          self.timeout = None

     def start(self):
          self.timeout = glib.Timeout(int(self.interval * 1000))
          self.timeout.set_callback(self._ping)
          self.timeout.attach(loop.get_context())
          if self.threshold < 0:
               print '\nPinging server at intervals of %fs.' % self.interval

     def stop(self):
          if self.timeout:
               self.timeout.destroy()
          self.started = None

     def check(self):
          '''Assert that all queries were served quickly enough.'''
          if self.threshold > 0:
               tooslow = [x for x in self.results if x[1] > self.threshold]
               self.test.assertEqual([], tooslow)
               if self.started:
                    self.test.assertLess(time.time() - self.started, self.threshold)

     def reset(self):
          self.results = []
          self.started = None

     def checkpoint(self, name):
          self.check()
          logging.printf('ping results for %s: %s', name, self.results)
          if self.threshold < 0:
               for result in self.results:
                    print '%s: ping duration: %f' % (name, result[1])
          self.reset()

     def _ping(self):
          if not self.started:
               # Run with a long timeout. We want to know how long it
               # takes to reply, even if it is too long.
               started = time.time()
               self.started = started
               self.manager.GetAllPeers(reply_handler=lambda peers: self._done(started, self.results, None),
                                        error_handler=lambda error: self._done(started, self.results, error))
          return True

     def _done(self, started, results, error):
          '''Record result. Intentionally uses the results array from the time when the call started,
          to handle intermittent checkpoints.'''
          duration = time.time() - started
          if self.threshold > 0 and duration > self.threshold or error:
               logging.printf('ping failure: duration %fs, error %s', duration, error)
          if error:
               results.append((started, duration, error))
          else:
               results.append((started, duration))
          if self.started == started:
               self.started = None

class TestPIMUtil(DBusUtil):

    def setUp(self):
        self.cleanup = []
        self.manager = dbus.Interface(bus.get_object('org._01.pim.contacts',
                                                     '/org/01/pim/contacts'),
                                      'org._01.pim.contacts.Manager')

        # Determine location of EDS source configs.
        config = os.environ.get("XDG_CONFIG_HOME", None)
        if config:
            self.sourcedir = os.path.join(config, "evolution", "sources")
        else:
            self.sourcedir = os.path.expanduser("~/.config/evolution/sources")

        # SyncEvolution uses a local temp dir.
        self.configdir = os.path.join(xdg_root, "syncevolution")

        # Common prefix for peer UIDs. Use different prefixes in each test,
        # because evolution-addressbook-factory keeps the old instance
        # open when syncevo-dbus-server stops or crashes and then fails
        # to work with that database when we remove it.
        self.uidPrefix = self.testname.replace('_', '-').lower() + '-'

        # Prefix used by PIM Manager in EDS.
        self.managerPrefix = 'pim-manager-'

        # Remove all sources and configs which were created by us
        # before running the test.
        removed = False
        if os.path.exists(self.sourcedir):
             for source in os.listdir(self.sourcedir):
                  if source.startswith(self.managerPrefix + self.uidPrefix):
                       os.unlink(os.path.join(self.sourcedir, source))
                       removed = True
        if removed:
            # Give EDS time to notice the removal.
            time.sleep(5)

    def tearDown(self):
         for x in self.cleanup:
              x()

    def setUpView(self, peers=['foo'], withSystemAddressBook=False, search=[], withLogging=True):
        '''Set up peers and create a view for them.'''
        # Ignore all currently existing EDS databases.
        self.sources = self.currentSources()
        self.expected = self.sources.copy()
        self.peers = {}

        # dummy peer directory
        self.contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(self.contacts)

        # add peers
        self.uid = None
        self.uids = []
        for peer in peers:
             uid = self.uidPrefix + peer
             if self.uid == None:
                  # Remember first uid for tests which only use one.
                  self.uid = uid
             self.uids.append(uid)
             self.peers[uid] = {'protocol': 'PBAP',
                                'address': 'xxx'}
             self.manager.SetPeer(uid,
                                  self.peers[uid])
             self.expected.add(self.managerPrefix + uid)
             self.assertEqual(self.peers, self.manager.GetAllPeers())
             self.assertEqual(self.expected, self.currentSources())

             # Delete local data in the cache.
             logging.log('deleting all items of ' + uid)
             self.runCmdline(['--delete-items', '@' + self.managerPrefix + uid, 'local', '*'])

        # Limit active databases to the one we just created.
        addressbooks = ['peer-' + uid for uid in self.uids]
        if withSystemAddressBook:
             addressbooks.append('')
             # Delete content of system address book. The check at the start of
             # the script ensures that this is not the real one of the user.
             logging.log('deleting all items of system address book')
             self.runCmdline(['--delete-items', 'backend=evolution-contacts', '--luids', '*'])

        self.manager.SetActiveAddressBooks(addressbooks)
        self.assertEqual(addressbooks, self.manager.GetActiveAddressBooks(),
                         sortLists=True)

        # Start view.
        self.view = ContactsView(self.manager)
        if not withLogging:
             self.view.processEvent = lambda message, event: True
             self.view.logging = NullLogging()

        # Optional: search and wait for it to be stable.
        if search != None:
             self.view.search(search)
             self.runUntil('empty view',
                           check=lambda: self.assertEqual([], self.view.errors),
                           until=lambda: self.view.quiescentCount > 0)

        # Clear unknown sequence of events.
        self.view.events = []


    def runCmdline(self, command, **args):
         '''use syncevolution command line without syncevo-dbus-server, for the sake of keeping code here minimal'''
         cmdline = testdbus.TestCmdline()
         cmdline.setUp()
         try:
              cmdline.running = True
              cmdline.session = None
              # Must use our own self.storedenv here, cmdline doesn't have it.
              c = [ '--daemon=no' ] + command
              logging.printf('running syncevolution command line: %s' % c)
              return cmdline.runCmdline(c,
                                        testInstance=self,
                                        env=self.storedenv,
                                        sessionFlags=None,
                                        **args)
         finally:
              cmdline.running = False

    def exportCache(self, uid, filename):
        '''dump local cache content into file'''
        self.runCmdline(['--export', filename, '@' + self.managerPrefix + uid, 'local'])
        contacts = open(filename, 'r').read()
        # Ignore one empty vcard because the Nokia N97 always sends such a vcard,
        # despite having deleted everything via SyncML.
        contacts = re.sub(r'''BEGIN:VCARD\r?
VERSION:3.0\r?
((UID|PRODID|REV):.*\r?
|N:;;;;\r?
|FN:\r?
)*END:VCARD(\r|\n)*''',
                          '',
                          contacts,
                          1)
        open(filename, 'w').write(contacts)

    def extractLUIDs(self, out):
         '''Extract the LUIDs from syncevolution --import/update output.'''
         r = re.compile(r'''#.*: (\S+)\n''')
         matches = r.split(out)
         # Even entry is text (empty here), odd entry is the match group.
         return matches[1::2]

    def compareDBs(self, expected, real, ignoreExtensions=True):
        '''ensure that two sets of items (file or directory) are identical at the semantic level'''
        env = copy.deepcopy(os.environ)
        if ignoreExtensions:
            # Allow the phone to add extensions like X-CLASS=private
            # (seen with Nokia N97 mini - FWIW, the phone should have
            # use CLASS=PRIVATE, because it was using vCard 3.0).
            # Also removes X-EVOLUTION-FILE-AS.
            env['CLIENT_TEST_STRIP_PROPERTIES'] = 'X-[-_a-zA-Z0-9]*'
        sub = subprocess.Popen(['synccompare', expected, real],
                               env=env,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        stdout, stderr = sub.communicate()
        self.assertEqual(0, sub.returncode,
                         msg="env 'CLIENT_TEST_STRIP_PROPERTIES=%s' synccompare %s %s\n%s" %
                         (env.get('CLIENT_TEST_STRIP_PROPERTIES', ''),
                          expected,
                          real,
                          stdout))

    def configurePhone(self, phone, uid, contacts):
        '''set up SyncML for copying all vCard 3.0 files in 'contacts' to the phone, if phone was set'''
        if phone:
             self.runCmdline(['--configure',
                              'syncURL=obex-bt://' + phone,
                              'backend=file',
                              'database=file://' + contacts,
                              'databaseFormat=text/vcard',
                              # Hard-coded Nokia config.
                              'remoteIdentifier=PC Suite',
                              'peerIsClient=1',
                              'uri=Contacts',
                              # Config name and source for syncPhone().
                              'phone@' + self.managerPrefix + uid,
                              'addressbook'])

    def syncPhone(self, phone, uid, syncMode='refresh-from-local'):
        '''use SyncML config for copying all vCard 3.0 files in 'contacts' to the phone, if phone was set'''
        if phone:
             self.runCmdline(['--sync', syncMode,
                              'phone@' + self.managerPrefix + uid,
                              'addressbook'])

    def run(self, result, serverArgs=[]):
        # No errors must be logged. During testRead, libphonenumber used to print
        #   [ERROR] Number too short to be viable: 8
        #   [ERROR] The string supplied did not seem to be a phone number.
        # to stdout until we reduced the log level.
        #
        # ==4039== ERROR SUMMARY: 2 errors from 2 contexts (suppressed: 185 from 175)
        # as printed by valgrind is okay, so don't match that.
        #
        # We check both D-Bus messages (which did not contain that
        # text, but some other error messages) and the servers stdout.
        def unicodeLog(test, log):
             open('/tmp/out', 'wb').write(log)
             print re.match(r'ERROR(?! SUMMARY:)', log)
        # Using assertNotRegexMatches with a negative lookahead led to unicode errors?!
        # Therefore stick to plain text checks and avoid false matches against valgind's
        # 'ERROR SUMMARY' by replacing that first.
        self.runTestDBusCheck = lambda test, log: test.assertNotIn('ERROR', log.replace('ERROR SUMMARY:', 'error summary:'))
        self.runTestOutputCheck = self.runTestDBusCheck

        # We have to clean the xdg_root ourselves. We have to be nice
        # to EDS and can't just wipe out the entire directory.
        # Same for GNOME Online Accounts.
        items = list(os.walk(xdg_root))
        items.reverse()
        for dirname, dirs, files in items:
            reldir = os.path.relpath(dirname, xdg_root)
            for dir in dirs:
                # evolution-source-registry gets confused when we remove
                # the "sources" directory itself.
                # GNOME Online Accounts settings and GNOME keyrings must survive.
                if (reldir == 'config/evolution' and dir == 'sources') or \
                         (reldir == 'data' and dir == 'keyrings') or \
                         (reldir == 'config' and dir.startswith('goa')):
                    continue
                dest = os.path.join(dirname, dir)
                try:
                    os.rmdir(dest)
                except OSError, ex:
                    if ex.errno != errno.ENOTEMPTY:
                        raise
            for file in files:
                dest = os.path.join(dirname, file)
                # Don't delete a DB that may still be in use by
                # evolution-addressbook-factory and that we may still need.
                # Other DBs can be removed because we are not going to depend on
                # them anymore thanks to the per-test uid prefix.
                if reldir == 'data/evolution/addressbook/system' or \
                         reldir == 'data/keyrings' or \
                         reldir.startswith('config/goa'):
                    continue
                os.unlink(dest)

        # We have to wait until evolution-source-registry catches up
        # and recognized that the sources are gone, otherwise
        # evolution-addressbook-factory will keep the .db files open
        # although we already removed them.
        while True:
            out, err = subprocess.Popen(['syncevolution', '--print-databases', '--daemon=no', 'backend=evolution-contacts'],
                                        stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE).communicate()
            self.assertEqual('', err)
            # Count the number of database entries. An exact
            # comparison against the output does not work, because the
            # name of the system address book due to localization and
            # (to a lesser degree) its UID may change.
            if len([x for x in out.split('\n') if x.startswith('   ')]) == 1:
                break
            else:
                time.sleep(0.5)

        # Does not work, Reload()ing a running registry confuses evolution-addressbook-factory.
        #
        # for i in range(0, 100):
        #     try:
        #         registry = dbus.Interface(bus.get_object('org.gnome.evolution.dataserver.Sources%d' % i,
        #                                                  '/org/gnome/evolution/dataserver/SourceManager'),
        #                                   'org.gnome.evolution.dataserver.SourceManager')
        #     except dbus.exceptions.DBusException, ex:
        #         if ex.get_dbus_name() != 'org.freedesktop.DBus.Error.ServiceUnknown':
        #             raise
        # registry.Reload()
        # # Give it some time...
        # time.sleep(2)

        # Runtime varies a lot when using valgrind, because
        # of the need to check an additional process. Allow
        # a lot more time when running under valgrind.
        self.runTest(result, own_xdg=False, own_home=False,
                     serverArgs=serverArgs,
                     defTimeout=usingValgrind() and 600 or 20)

    def currentSources(self):
        '''returns current set of EDS sources as set of UIDs, without the .source suffix'''
        return set([os.path.splitext(x)[0] for x in (os.path.exists(self.sourcedir) and os.listdir(self.sourcedir) or [])])


    def readManagerIni(self):
         '''returns content of manager.ini file, split into lines and sorted, None if not found'''
         filename = os.path.join(xdg_root, "config", "syncevolution", "pim-manager.ini")
         if os.path.exists(filename):
              lines = open(filename, "r").readlines()
              lines.sort()
              return lines
         else:
              return None


class TestContacts(TestPIMUtil, unittest.TestCase):
    """Tests for org._01.pim.contacts API.

The tests use the system's EDS, which must be >= 3.6.
They create additional databases in EDS under the normal
location. This is necessary because the tests cannot
tell the EDS source registry daemon to run with a different
XDG root.
"""

    def testUIDError(self):
        '''TestContacts.testUIDError - check that invalid UID is properly detected and reported'''
        with self.assertRaisesRegexp(dbus.DBusException,
                                     'invalid peer uid: CAPITAL-LETTERS-NOT-ALLOWED'):
            self.manager.SetPeer('CAPITAL-LETTERS-NOT-ALLOWED',
                                 {})

    @property("snapshot", "simple-sort")
    def testConfig(self):
        '''TestContacts.testConfig - set and remove peers'''
        sources = self.currentSources()
        expected = sources.copy()
        peers = {}

        # add foo
        uid = self.uidPrefix + 'foo'
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'xxx'}
        self.manager.SetPeer(uid,
                             peers[uid])
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['sort =\n'],
                         self.readManagerIni())

        # Check effect of SetActiveAddressBooks() on pim-manager.ini.
        self.manager.SetActiveAddressBooks(['peer-' + uid])
        self.assertEqual(['active = pim-manager-' + uid + '\n',
                          'sort =\n'],
                         self.readManagerIni())
        self.manager.SetActiveAddressBooks([])
        self.assertEqual(['active = \n',
                          'sort =\n'],
                         self.readManagerIni())

        # PIM Manager must not allow overwriting an existing config.
        # Uses the new name for SetPeer().
        with self.assertRaisesRegexp(dbus.DBusException,
                                     'org._01.pim.contacts.Manager.AlreadyExists: uid ' + uid + ' is already in use') as cm:
             self.manager.CreatePeer(uid,
                                     peers[uid])
        self.assertEqual('org._01.pim.contacts.Manager.AlreadyExists', cm.exception.get_dbus_name())

        # TODO: work around EDS bug: e_source_remove_sync() quickly after
        # e_source_registry_create_sources_sync() leads to an empty .source file:
        # [Data Source]
        # DisplayName=Unnamed
        # Enabled=true
        # Parent=
        #
        # That prevents reusing the same UID.
        time.sleep(2)

        # remove foo
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        self.manager.RemovePeer(uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['active = \n',
                          'sort =\n'],
                         self.readManagerIni())

        # add and remove foo again, this time while its address book is active
        uid = self.uidPrefix + 'foo4' # work around EDS bug with reusing UID
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'xxx'}
        self.manager.SetPeer(uid,
                             peers[uid])
        expected.add(self.managerPrefix + uid)
        self.manager.SetActiveAddressBooks(['peer-' + uid])
        self.assertEqual(['active = pim-manager-' + uid + '\n',
                          'sort =\n'],
                         self.readManagerIni())
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        time.sleep(2)
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        self.manager.RemovePeer(uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['active = \n',
                          'sort =\n'],
                         self.readManagerIni())

        # add foo, bar, xyz
        addressbooks = []
        uid = self.uidPrefix + 'foo2'
        addressbooks.append('peer-' + uid)
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'xxx'}
        self.manager.SetPeer(uid,
                             peers[uid])
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['active = \n',
                          'sort =\n'],
                         self.readManagerIni())

        uid = self.uidPrefix + 'bar'
        addressbooks.append('peer-' + uid)
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'yyy'}
        self.manager.SetPeer(uid,
                             peers[uid])
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())

        uid = self.uidPrefix + 'xyz'
        addressbooks.append('peer-' + uid)
        peers[uid] = {'protocol': 'PBAP',
                      'address': 'zzz'}
        self.manager.SetPeer(uid,
                             peers[uid])
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['active = \n',
                          'sort =\n'],
                         self.readManagerIni())

        self.manager.SetActiveAddressBooks(addressbooks)
        addressbooks.sort()
        self.assertEqual(['active = ' + ' '.join([x.replace('peer-', 'pim-manager-') for x in addressbooks]) + '\n',
                          'sort =\n'],
                         self.readManagerIni())

        # EDS workaround
        time.sleep(2)

        # remove yxz, bar, foo
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        addressbooks.remove('peer-' + uid)
        self.manager.RemovePeer(uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['active = ' + ' '.join([x.replace('peer-', 'pim-manager-') for x in addressbooks]) + '\n',
                          'sort =\n'],
                         self.readManagerIni())

        # EDS workaround
        time.sleep(2)

        uid = self.uidPrefix + 'bar'
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        addressbooks.remove('peer-' + uid)
        self.manager.RemovePeer(uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['active = ' + ' '.join([x.replace('peer-', 'pim-manager-') for x in addressbooks]) + '\n',
                          'sort =\n'],
                         self.readManagerIni())

        # EDS workaround
        time.sleep(2)

        uid = self.uidPrefix + 'foo2'
        expected.remove(self.managerPrefix + uid)
        del peers[uid]
        addressbooks.remove('peer-' + uid)
        self.manager.RemovePeer(uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())
        self.assertEqual(['active = ' + ' '.join([x.replace('peer-', 'pim-manager-') for x in addressbooks]) + '\n',
                          'sort =\n'],
                         self.readManagerIni())

        # EDS workaround
        time.sleep(2)

    @property("snapshot", "broken-config")
    def testBrokenConfig(self):
        '''TestContacts.testBrokenConfig - start with broken pim-manager.ini'''
        self.manager.Start()
        self.assertEqual("last/first", self.manager.GetSortOrder())

    @timeout(os.environ.get('TESTPIM_TEST_SYNC_TESTCASES', False) and 300000 or 300)
    @property("snapshot", "simple-sort")
    def testSync(self):
        '''TestContacts.testSync - test caching of a dummy peer which uses a real phone or a local directory as fallback'''
        sources = self.currentSources()
        expected = sources.copy()
        peers = {}
        logdir = xdg_root + '/cache/syncevolution'
        # If set, then test importing all these contacts,
        # matching them, and removing them. Prints timing information.
        testcases = os.environ.get('TESTPIM_TEST_SYNC_TESTCASES', None)
        if testcases:
             def progress(step, duration):
                  print
                  edslogs = [x for x in os.listdir(logdir) if x.startswith('eds@')]
                  edslogs.sort()
                  print '%s: %fs, see %s' % (step, duration,
                                             os.path.join(logdir, edslogs[-1]))
        else:
             def progress(*args1, **args2):
                  pass

        syncProgress = []
        signal = bus.add_signal_receiver(lambda uid, event, data: (logging.printf('received SyncProgress: %s, %s, %s', self.stripDBus(uid, sortLists=False), self.stripDBus(event, sortLists=False), self.stripDBus(data, sortLists=False)), syncProgress.append((uid, event, data)), logging.printf('progress %s' % self.stripDBus(syncProgress, sortLists=False))),
                                         'SyncProgress',
                                         'org._01.pim.contacts.Manager',
                                         None, #'org._01.pim.contacts',
                                         '/org/01/pim/contacts',
                                         byte_arrays=True,
                                         utf8_strings=True)
        def checkSync(expectedResult, result, intermediateResult=None):
             self.assertEqual(expectedResult, result)
             while not (uid, 'done', {}) in syncProgress:
                  self.loopIteration('added signal')
             progress = [ (uid, 'started', {}) ]
             if intermediateResult:
                  progress.append((uid, 'modified', intermediateResult))
             progress.append((uid, 'modified', expectedResult))
             progress.append((uid, 'done', {}))
             self.assertEqual(progress, [(u, e, d) for u, e, d in syncProgress if e != 'progress'])


        # Must be the Bluetooth MAC address (like A0:4E:04:1E:AD:30)
        # of a phone which is paired, currently connected, and
        # supports both PBAP and SyncML. SyncML is needed for putting
        # data onto the phone. Nokia phones like the N97 Mini are
        # known to work and easily available, therefore the test
        # hard-codes the Nokia SyncML settings (could be changed).
        #
        # If set, that phone will be used instead of local sync with
        # the file backend.
        phone = os.environ.get('TEST_DBUS_PBAP_PHONE', None)

        # dummy peer directory
        contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(contacts)

        # add foo
        uid = self.uidPrefix + 'foo'
        if phone:
            peers[uid] = {'protocol': 'PBAP',
                          'address': phone}
        else:
            peers[uid] = {'protocol': 'files',
                          'address': contacts}
        self.manager.SetPeer(uid,
                             peers[uid])
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())

        # Clear data in the phone.
        self.configurePhone(phone, uid, contacts)
        self.syncPhone(phone, uid)

        def listall(dirs, exclude=[]):
             result = {}
             def append(dirname, entry):
                  fullname = os.path.join(dirname, entry)
                  for pattern in exclude:
                       if re.match(pattern, fullname):
                            return
                  result[fullname] = os.stat(fullname).st_mtime
             for dir in dirs:
                  for dirname, dirnames, filenames in os.walk(dir):
                       for subdirname in dirnames:
                            append(dirname, subdirname)
                       for filename in filenames:
                            append(dirname, filename)
             return result

        def listsyncevo(exclude=[]):
             '''find all files owned by SyncEvolution, excluding the logs for syncing with a real phone'''
             return listall([os.path.join(xdg_root, x) for x in ['config/syncevolution', 'cache/syncevolution']],
                            exclude + ['.*/phone@.*', '.*/peers/phone.*'])

        # Remember current list of files and modification time stamp.
        files = listsyncevo()

        # Remove all data locally. There may or may not have been data
        # locally, because the database of the peer might have existed
        # from previous tests.
        duration, res = timeFunction(self.manager.SyncPeer,
                                     uid)
        progress('clear', duration)
        # TODO: check that syncPhone() really used PBAP - but how?

        # Should not have written files, except for specific exceptions:
        exclude = []
        # - directories in which we need to create files
        exclude.extend([xdg_root + '/cache/syncevolution/eds@[^/]*$',
                        xdg_root + '/cache/syncevolution/target_.config@[^/]*$'])
        # - some files which are allowed to be written
        exclude.extend([xdg_root + '/cache/syncevolution/[^/]*/(status.ini|syncevolution-log.html)$'])
        # - synthesis client files (should not be written at all, but that's harder - redirect into cache for now)
        exclude.extend([xdg_root + '/cache/syncevolution/[^/]*/synthesis(/|$)'])

        # Now compare files and their modification time stamp.
        self.assertEqual(files, listsyncevo(exclude=exclude))

        # Export data from local database into a file via the --export
        # operation in the syncevo-dbus-server. Depends on (and tests)
        # that the SyncEvolution configuration was created as
        # expected. It does not actually check that EDS is used - the
        # test would also pass for any other storage.
        export = os.path.join(xdg_root, 'local.vcf')
        self.exportCache(uid, export)

        # Must be empty now.
        self.compareDBs(contacts, export)

        # Add a contact.
        john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQEASABIAAD/4QAWRXhpZgAATU0AKgAA
 AAgAAAAAAAD//gAXQ3JlYXRlZCB3aXRoIFRoZSBHSU1Q/9sAQwAFAwQEBAMFBAQEBQUFBgcM
 CAcHBwcPCwsJDBEPEhIRDxERExYcFxMUGhURERghGBodHR8fHxMXIiQiHiQcHh8e/9sAQwEF
 BQUHBgcOCAgOHhQRFB4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e
 Hh4eHh4eHh4e/8AAEQgAFwAkAwEiAAIRAQMRAf/EABkAAQADAQEAAAAAAAAAAAAAAAAGBwgE
 Bf/EADIQAAECBQMCAwQLAAAAAAAAAAECBAADBQYRBxIhEzEUFSIIFjNBGCRHUVZ3lqXD0+P/
 xAAUAQEAAAAAAAAAAAAAAAAAAAAA/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEQMR
 AD8AuX6UehP45/aXv9MTPTLVKxNSvMPcqu+a+XdLxf1SfJ6fU37PioTnOxfbOMc/KIZ7U/2V
 fmTR/wCaKlu6+blu/Ui72zxWtUmmUOrTaWwkWDT09FPR4K587OVrUfVsIwElPPPAbAjxr2um
 hWXbDu5rmfeApLPZ4hx0lzNm9aUJ9KAVHKlJHAPf7ozPLqWt9y6Z0EPGmoLNjTq48a1iaybJ
 YV52yEtCms5KJmAT61JXtJyUdyQTEc1WlMql7N1/oZ6jagVZVFfUyZPpFy5lvWcxU7Z03BUk
 GZLWJqVhPYLkIIPBEBtSEUyNAsjI1q1m/VP+UICwL/sqlXp7v+aOHsnyGttq218MtKd8+Ru2
 JXuScoO45Awe2CIi96aKW1cVyubkYVy6rTqz0J8a5t2qqZl0UjAMwYKScfPAJ+cIQHHP0Dth
 VFaMWt0XwxetnM50Ks2rsxL6ZMnJlJmb5hBBBEiVxjA28dznqo+hdksbQuS3Hs6tVtNzdM1Z
 /VH5nO3Bl/CJmYHKDynjv3zCEB5rLQNo0bIbydWNWxKljbLQLoWkISOAkBKAABCEID//2Q==
END:VCARD'''

        # Test all fields that PIM Manager supports in its D-Bus API
        # when using the file backend, because (in contrast to a phone)
        # we know that it supports them, too.
        if not phone:
             john = r'''BEGIN:VCARD
VERSION:3.0
URL:http://john.doe.com
TITLE:Senior Tester
ORG:Test Inc.;Testing;test#1
ROLE:professional test case
X-EVOLUTION-MANAGER:John Doe Senior
X-EVOLUTION-ASSISTANT:John Doe Junior
NICKNAME:user1
BDAY:2006-01-08
X-EVOLUTION-ANNIVERSARY:2006-01-09
X-EVOLUTION-SPOUSE:Joan Doe
NOTE:This is a test case which uses almost all Evolution fields.
FN:John Doe
N:Doe;John;;;
X-EVOLUTION-FILE-AS:Doe\, John
CATEGORIES:TEST
X-EVOLUTION-BLOG-URL:web log
GEO:30.12;-130.34
CALURI:calender
FBURL:free/busy
X-EVOLUTION-VIDEO-URL:chat
X-MOZILLA-HTML:TRUE
ADR;TYPE=WORK:Test Box #2;;Test Drive 2;Test Town;Upper Test County;12346;O
 ld Testovia
LABEL;TYPE=WORK:Test Drive 2\nTest Town\, Upper Test County\n12346\nTest Bo
 x #2\nOld Testovia
ADR;TYPE=HOME:Test Box #1;;Test Drive 1;Test Village;Lower Test County;1234
 5;Testovia
LABEL;TYPE=HOME:Test Drive 1\nTest Village\, Lower Test County\n12345\nTest
  Box #1\nTestovia
ADR:Test Box #3;;Test Drive 3;Test Megacity;Test County;12347;New Testonia
LABEL;TYPE=OTHER:Test Drive 3\nTest Megacity\, Test County\n12347\nTest Box
  #3\nNew Testonia
UID:pas-id-43C0ED3900000001
EMAIL;TYPE=WORK;X-EVOLUTION-UI-SLOT=1:john.doe@work.com
EMAIL;TYPE=HOME;X-EVOLUTION-UI-SLOT=2:john.doe@home.priv
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=3:john.doe@other.world
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=4:john.doe@yet.another.world
TEL;TYPE=work;TYPE=Voice;X-EVOLUTION-UI-SLOT=1:business 1
TEL;TYPE=homE;TYPE=VOICE;X-EVOLUTION-UI-SLOT=2:home 2
TEL;TYPE=CELL;X-EVOLUTION-UI-SLOT=3:mobile 3
TEL;TYPE=WORK;TYPE=FAX;X-EVOLUTION-UI-SLOT=4:businessfax 4
TEL;TYPE=HOME;TYPE=FAX;X-EVOLUTION-UI-SLOT=5:homefax 5
TEL;TYPE=PAGER;X-EVOLUTION-UI-SLOT=6:pager 6
TEL;TYPE=CAR;X-EVOLUTION-UI-SLOT=7:car 7
TEL;TYPE=PREF;X-EVOLUTION-UI-SLOT=8:primary 8
END:VCARD
'''

        if testcases:
             # Split test case file into files.
             numPhotos = 0
             hasPhoto = re.compile('^PHOTO[;:].+\r?$', re.MULTILINE)
             for i, data in enumerate(open(testcases).read().split('\n\n')):
                  item = os.path.join(contacts, '%d.vcf' % i)
                  output = open(item, "w")
                  output.write(data)
                  if hasPhoto.search(data):
                       numPhotos = numPhotos + 1
                  output.close()
             numItems = i + 1
        else:
             item = os.path.join(contacts, 'john.vcf')
             output = open(item, "w")
             output.write(john)
             output.close()
             numItems = 1
             numPhotos = 1
        self.syncPhone(phone, uid)
        syncProgress = []
        duration, result = timeFunction(self.manager.SyncPeer,
                                        uid)
        progress('import', duration)
        incrementalSync = phone and os.environ.get('SYNCEVOLUTION_PBAP_SYNC', 'incremental') == 'incremental'
        if incrementalSync:
             # Single contact gets added, then updated to add the photo.
             finalUpdated = numPhotos
             intermediate = {'modified': True,
                             'added': numItems,
                             'updated': 0,
                             'removed': 0}
        else:
             finalUpdated = 0
             intermediate = None

        checkSync({'modified': True,
                    'added': numItems,
                    'updated': finalUpdated,
                    'removed': 0},
                  result,
                  intermediate)

        # Also exclude modified database files.
        self.assertEqual(files, listsyncevo(exclude=exclude))

        # Testcase data does not necessarily import/export without changes.
        if not testcases:
             self.exportCache(uid, export)
             self.compareDBs(contacts, export)

        # Skip logdir tests when focusing on syncing data.
        if not testcases:
             # Keep one session directory in a non-default location.
             logdir = xdg_root + '/pim-logdir'
             peers[uid]['logdir'] = logdir
             peers[uid]['maxsessions'] = '1'
             self.manager.SetPeer(uid,
                                  peers[uid])
             files = listsyncevo(exclude=exclude)
             syncProgress = []
             result = self.manager.SyncPeer(uid)
             expectedResult = {'modified': False,
                               'added': 0,
                               'updated': 0,
                               'removed': 0}
             checkSync(expectedResult,
                       result,
                       incrementalSync and expectedResult)
             exclude.append(logdir + '(/$)')
             self.assertEqual(files, listsyncevo(exclude=exclude))
             self.assertEqual(2, len(os.listdir(logdir)))

        # No changes.
        syncProgress = []
        duration, result = timeFunction(self.manager.SyncPeer,
                                        uid)
        progress('match', duration)
        expectedResult = {'modified': False,
                          'added': 0,
                          'updated': 0,
                          'removed': 0}
        checkSync(expectedResult,
                  result,
                  incrementalSync and expectedResult)
        self.assertEqual(files, listsyncevo(exclude=exclude))
        if not phone:
             self.assertEqual(testcases and 6 or 2, len(os.listdir(logdir)))

        if not testcases:
             # And now prune none.
             peers[uid]['maxsessions'] = '0'
             self.manager.SetPeer(uid,
                                  peers[uid])
             files = listsyncevo(exclude=exclude)
             syncProgress = []
             result = self.manager.SyncPeer(uid)
             expectedResult = {'modified': False,
                               'added': 0,
                               'updated': 0,
                               'removed': 0}
             checkSync(expectedResult,
                       result,
                       incrementalSync and expectedResult)
             exclude.append(logdir + '(/$)')
             self.assertEqual(files, listsyncevo(exclude=exclude))
             self.assertEqual(4, len(os.listdir(logdir)))

        # Test incremental sync API. Only possible with real phone.
        if phone:
             expectedResult = {'modified': False,
                               'added': 0,
                               'updated': 0,
                               'removed': 0}

             syncProgress = []
             duration, result = timeFunction(self.manager.SyncPeerWithFlags, uid, { 'pbap-sync': 'text' })
             # TODO: check that we actually do text-only sync
             checkSync(expectedResult,
                       result,
                       None)

             syncProgress = []
             duration, result = timeFunction(self.manager.SyncPeerWithFlags, uid, { 'pbap-sync': 'all' })
             expectedResult = {'modified': False,
                               'added': 0,
                               'updated': 0,
                               'removed': 0}
             # TODO: check that we actually do complete sync
             checkSync(expectedResult,
                       result,
                       None)

             syncProgress = []
             duration, result = timeFunction(self.manager.SyncPeerWithFlags, uid, { 'pbap-sync': 'incremental' })
             expectedResult = {'modified': False,
                               'added': 0,
                               'updated': 0,
                               'removed': 0}
             # TODO: check that we actually do incremental sync *without* relying on
             # "modified" begin emitted at the end of the first cycle despite there being
             # no changes.
             checkSync(expectedResult,
                       result,
                       expectedResult)


        # Cannot update data when using pre-defined test cases.
        if not testcases:
             # Update contact, removing extra properties.
             john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD'''
             output = open(item, "w")
             output.write(john)
             output.close()
             self.syncPhone(phone, uid)
             syncProgress = []
             result = self.manager.SyncPeer(uid)
             checkSync({'modified': True,
                        'added': 0,
                        'updated': 1,
                        'removed': 0},
                       result,
                       incrementalSync and {'modified': False,
                                            'added': 0,
                                            'updated': 0,
                                            'removed': 0})

        # Remove contact(s).
        for file in os.listdir(contacts):
             os.remove(os.path.join(contacts, file))
        self.syncPhone(phone, uid)
        syncProgress = []
        duration, result = timeFunction(self.manager.SyncPeer,
                                        uid)
        progress('remove', duration)
        expectedResult = {'modified': True,
                          'added': 0,
                          'updated': 0,
                          'removed': numItems}
        checkSync(expectedResult,
                  result,
                  incrementalSync and expectedResult)

        # Test invalid maxsession values.
        if not testcases:
             with self.assertRaisesRegexp(dbus.DBusException,
                                          "negative 'maxsessions' not allowed: -1"):
                  self.manager.SetPeer(uid,
                                       {'protocol': 'PBAP',
                                        'address': 'foo',
                                        'maxsessions': '-1'})
             self.assertEqual(files, listsyncevo(exclude=exclude))

             with self.assertRaisesRegexp(dbus.DBusException,
                                          'bad lexical cast: source type value could not be interpreted as target'):
                  self.manager.SetPeer(uid,
                                       {'protocol': 'PBAP',
                                        'address': 'foo',
                                        'maxsessions': '1000000000000000000000000000000000000000000000'})

        self.assertEqual(files, listsyncevo(exclude=exclude))

    @timeout(100)
    @property("ENV", "SYNCEVOLUTION_SYNC_DELAY=5") # first parent sleeps, then child -> total delay 10s
    @property("snapshot", "simple-sort")
    def testSyncSuspend(self):
        '''TestContacts.testSyncSuspend - test Suspend/ResumeSync()'''
        self.setUpServer()
        sources = self.currentSources()
        expected = sources.copy()
        peers = {}

        # Disable the default checking because
        # we trigger one ERROR message.
        self.runTestDBusCheck = None
        self.runTestOutputCheck = None

        # dummy peer directory
        contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(contacts)

        # add foo
        uid = self.uidPrefix + 'foo'
        peers[uid] = {'protocol': 'files',
                      'address': contacts}
        self.manager.SetPeer(uid,
                             peers[uid])
        expected.add(self.managerPrefix + uid)
        self.assertEqual(peers, self.manager.GetAllPeers())
        self.assertEqual(expected, self.currentSources())

        # Can't suspend or resume. Not an error, though.
        self.assertEqual(False, self.manager.SuspendSync(uid))
        self.assertEqual(False, self.manager.ResumeSync(uid))

        # Start a sync. Because of SYNCEVOLUTION_SYNC_DELAY, this will block long
        # enough for us to suspend the sync. Normally, the delay would be small.
        # When suspending, we resume after the end of the delay and thus can
        # detect that suspending really worked by looking at timing.
        syncCompleted = [ False ]
        self.suspended = None
        self.resumed = None
        self.syncStarted = None
        def result(index, res):
             syncCompleted[index] = res
        def output(path, level, text, procname):
            if self.running and text == 'ready to sync':
                logging.printf('suspending sync')
                self.syncStarted = time.time()
                self.suspended = self.manager.SuspendSync(uid)
                self.suspended2 = self.manager.SuspendSync(uid)
                logging.printf('waiting 20 seconds')
                time.sleep(20)
                logging.printf('resuming sync')
                self.resumed = self.manager.ResumeSync(uid)
                self.resumed2 = self.manager.ResumeSync(uid)
        receiver = bus.add_signal_receiver(output,
                                           'LogOutput',
                                           'org.syncevolution.Server',
                                           self.server.bus_name,
                                           byte_arrays=True,
                                           utf8_strings=True)
        try:
             self.manager.SyncPeer(uid,
                                   reply_handler=lambda x: result(0, True),
                                   error_handler=lambda x: result(0, x))
             self.runUntil('sync done',
                           check=lambda: True,
                           until=lambda: not False in syncCompleted)
             self.syncEnded = time.time()
        finally:
            receiver.remove()

        # Check for successful sync, with intermediate suspend/resume.
        self.assertEqual(True, self.suspended)
        self.assertEqual(False, self.suspended2)
        self.assertEqual(True, self.resumed)
        self.assertEqual(False, self.resumed2)
        self.assertEqual(True, syncCompleted[0])
        self.assertLess(20, self.syncEnded - self.syncStarted)

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testView(self):
        '''TestContacts.testView - test making changes to the unified address book'''
        self.setUpView()

        # Insert new contact.
        self.view.quiescentCount = 0
        john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD'''
        item = os.path.join(self.contacts, 'john.vcf')
        output = open(item, "w")
        output.write(john)
        output.close()
        logging.log('inserting John')
        out, err, returncode = self.runCmdline(['--import', item, '@' + self.managerPrefix + self.uid, 'local'])
        luid = self.extractLUIDs(out)[0]

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)

        # Check for the one expected event.
        self.assertEqual([('added', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0

        # Read contact.
        logging.log('reading contact')
        self.view.read(0)
        self.runUntil('contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual('John Doe', self.view.contacts[0]['full-name'])

        # Update contacts.
        johnBDay = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
BDAY:20120924
END:VCARD'''
        output = open(item, "w")
        output.write(johnBDay)
        output.close()
        logging.log('updating John')
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luid])
        self.runUntil('view with changed contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('modified', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0

        # Remove contact.
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])
        self.runUntil('view without contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('removed', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testViewSorting(self):
        '''TestContacts.testViewSorting - check that sorting works when changing contacts'''
        self.setUpView()

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list. The default sort method is active, which is not
        # locale-aware. Therefore the test data only uses ASCII characters.
        for i, contact in enumerate(['''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham
END:VCARD''',

'''BEGIN:VCARD
VERSION:3.0
FN:Benjamin Yeah
N:Yeah;Benjamin
END:VCARD''',

'''BEGIN:VCARD
VERSION:3.0
FN:Charly Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = open(item, "w")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with three contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3)

        # Check for the one expected event.
        # TODO: self.assertEqual([('added', 0, 3)], view.events)
        self.view.events = []

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # We should have seen all events for the changes above now,
        # start with a clean slate.
        self.view.events = []
        self.view.quiescentCount = 0

        # No re-ordering necessary: criteria doesn't change.
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #2',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('modified', 2, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(2))
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # No re-ordering necessary: criteria changes, but not in a relevant way, at end.
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham;Middle
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #2',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('modified', 2, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(2))
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # No re-ordering necessary: criteria changes, but not in a relevant way, in the middle.
        item = os.path.join(self.contacts, 'contact1.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Benjamin Yeah
N:Yeah;Benjamin;Middle
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[1]])
        self.runUntil('view with changed contact #1',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('modified', 1, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0
        logging.log('reading updated contact')
        self.view.read(1, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(1))
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # No re-ordering necessary: criteria changes, but not in a relevant way, in the middle.
        item = os.path.join(self.contacts, 'contact2.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Charly Xing
N:Xing;Charly;Middle
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[2]])
        self.runUntil('view with changed contact #0',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('modified', 0, 1), ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0
        logging.log('reading updated contact')
        self.view.read(0, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

        # Re-ordering necessary: last item (Zoo;Abraham) becomes first (Ace;Abraham).
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Ace
N:Ace;Abraham;Middle
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #0',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('removed', 2, 1),
                          ('added', 0, 1),
                          ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0
        logging.log('reading updated contact')
        self.view.read(0, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual('Abraham Ace', self.view.contacts[0]['full-name'])
        self.assertEqual('Charly Xing', self.view.contacts[1]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[2]['full-name'])

        # Re-ordering necessary: first item (Ace;Abraham) becomes last (Zoo;Abraham).
        item = os.path.join(self.contacts, 'contact0.vcf')
        output = open(item, "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:Abraham Zoo
N:Zoo;Abraham;Middle
BDAY:20120924
END:VCARD''')
        output.close()
        self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with changed contact #2',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('removed', 0, 1),
                          ('added', 2, 1),
                          ('quiescent',)], self.view.events)
        self.view.events = []
        self.view.quiescentCount = 0
        logging.log('reading updated contact')
        self.view.read(2, 1)
        self.runUntil('updated contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(2))
        self.assertEqual('Charly Xing', self.view.contacts[0]['full-name'])
        self.assertEqual('Benjamin Yeah', self.view.contacts[1]['full-name'])
        self.assertEqual('Abraham Zoo', self.view.contacts[2]['full-name'])

    @timeout(60)
    # boost::locale checks LC_TYPE first, then LC_ALL, LANG. Set all, just
    # to be sure.
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    @property("snapshot", "first-last-sort")
    def testSortOrder(self):
        '''TestContacts.testSortOrder - check that sorting works when changing the comparison'''
        self.setUpView()

        # Locale-aware "first/last" sorting from "first-last-sort" config.
        self.assertEqual("first/last", self.manager.GetSortOrder())

        # Expect an error, no change to sort order.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     'sort order.*not supported'):
             self.manager.SetSortOrder('no-such-order')
        self.assertEqual("first/last", self.manager.GetSortOrder())

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Äbraham
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Bénjamin
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Chàrly Xing
N:Xing;Chàrly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with three contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3)

        # Check for the one expected event.
        # TODO: self.assertEqual([('added', 0, 3)], view.events)
        self.view.events = []

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Äbraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Bénjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Chàrly', self.view.contacts[2]['structured-name']['given'])

        # Invert sort order.
        self.manager.SetSortOrder("last/first")

        # Check that order was adapted and stored permanently.
        self.assertEqual("last/first", self.manager.GetSortOrder())
        self.assertIn("sort = last/first\n",
                      self.readManagerIni())

        # Contact in the middle may or may not become invalidated.
        self.runUntil('reordered',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3 and \
                           self.view.haveNoData(0) and \
                           self.view.haveNoData(2))
        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual('Xing', self.view.contacts[0]['structured-name']['family'])
        self.assertEqual('Yeah', self.view.contacts[1]['structured-name']['family'])
        self.assertEqual('Zoo', self.view.contacts[2]['structured-name']['family'])

        # Sort by FN or <first last> as fallback.
        self.manager.SetSortOrder("fullname")
        self.runUntil('reordered',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3 and \
                           self.view.haveNoData(0) and \
                           self.view.haveNoData(2))
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Äbraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Bénjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Chàrly', self.view.contacts[2]['structured-name']['given'])

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testRead(self):
        '''TestContacts.testRead - check that folks and PIM Manager deliver EDS data correctly'''
        self.setUpView()

        # Insert new contacts.
        #
        # Not all of the vCard properties need to be available via PIM Manager.
        testcases = [r'''BEGIN:VCARD
VERSION:3.0
URL:http://john.doe.com
TITLE:Senior Tester
ORG:Test Inc.;Testing;test#1
ROLE:professional test case
X-EVOLUTION-MANAGER:John Doe Senior
X-EVOLUTION-ASSISTANT:John Doe Junior
NICKNAME:user1
BDAY:2006-01-08
X-FOOBAR-EXTENSION;X-FOOBAR-PARAMETER=foobar:has to be stored internally by engine and preserved in testExtensions test\; never sent to a peer
X-TEST;PARAMETER1=nonquoted;PARAMETER2="quoted because of spaces":Content with\nMultiple\nText lines\nand national chars: äöü
X-EVOLUTION-ANNIVERSARY:2006-01-09
X-EVOLUTION-SPOUSE:Joan Doe
NOTE:This is a test case which uses almost all Evolution fields.
FN:John Doe
N:Doe;John;;;
X-EVOLUTION-FILE-AS:Doe\, John
CATEGORIES:TEST1,TEST2
GEO:30.12;-130.34
X-EVOLUTION-BLOG-URL:web log
CALURI:calender
FBURL:free/busy
X-EVOLUTION-VIDEO-URL:chat
X-MOZILLA-HTML:TRUE
ADR;TYPE=WORK:Test Box #2;;Test Drive 2;Test Town;Upper Test County;12346;O
 ld Testovia
LABEL;TYPE=WORK:Test Drive 2\nTest Town\, Upper Test County\n12346\nTest Bo
 x #2\nOld Testovia
ADR;TYPE=HOME:Test Box #1;;Test Drive 1;Test Village;Lower Test County;1234
 5;Testovia
LABEL;TYPE=HOME:Test Drive 1\nTest Village\, Lower Test County\n12345\nTest
  Box #1\nTestovia
ADR:Test Box #3;;Test Drive 3;Test Megacity;Test County;12347;New Testonia
LABEL;TYPE=OTHER:Test Drive 3\nTest Megacity\, Test County\n12347\nTest Box
  #3\nNew Testonia
UID:pas-id-43C0ED3900000001
EMAIL;TYPE=WORK;X-EVOLUTION-UI-SLOT=1:john.doe@work.com
EMAIL;TYPE=HOME;X-EVOLUTION-UI-SLOT=2:john.doe@home.priv
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=3:john.doe@other.world
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=4:john.doe@yet.another.world
TEL;TYPE=work;TYPE=Voice;X-EVOLUTION-UI-SLOT=1:business 1
TEL;TYPE=homE;TYPE=VOICE;X-EVOLUTION-UI-SLOT=2:home 2
TEL;TYPE=CELL;X-EVOLUTION-UI-SLOT=3:mobile 3
TEL;TYPE=WORK;TYPE=FAX;X-EVOLUTION-UI-SLOT=4:businessfax 4
TEL;TYPE=HOME;TYPE=FAX;X-EVOLUTION-UI-SLOT=5:homefax 5
TEL;TYPE=PAGER;X-EVOLUTION-UI-SLOT=6:pager 6
TEL;TYPE=CAR;X-EVOLUTION-UI-SLOT=7:car 7
TEL;TYPE=PREF;X-EVOLUTION-UI-SLOT=8:primary 8
X-AIM;X-EVOLUTION-UI-SLOT=1:AIM JOHN
X-YAHOO;X-EVOLUTION-UI-SLOT=2:YAHOO JDOE
X-ICQ;X-EVOLUTION-UI-SLOT=3:ICQ JD
X-GROUPWISE;X-EVOLUTION-UI-SLOT=4:GROUPWISE DOE
X-GADUGADU:GADUGADU DOE
X-JABBER:JABBER DOE
X-MSN:MSN DOE
X-SKYPE:SKYPE DOE
X-SIP:SIP DOE
PHOTO;ENCODING=b;TYPE=JPEG:/9j/4AAQSkZJRgABAQEASABIAAD/4QAWRXhpZgAATU0AKgAA
 AAgAAAAAAAD//gAXQ3JlYXRlZCB3aXRoIFRoZSBHSU1Q/9sAQwAFAwQEBAMFBAQEBQUFBgcM
 CAcHBwcPCwsJDBEPEhIRDxERExYcFxMUGhURERghGBodHR8fHxMXIiQiHiQcHh8e/9sAQwEF
 BQUHBgcOCAgOHhQRFB4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4eHh4e
 Hh4eHh4eHh4e/8AAEQgAFwAkAwEiAAIRAQMRAf/EABkAAQADAQEAAAAAAAAAAAAAAAAGBwgE
 Bf/EADIQAAECBQMCAwQLAAAAAAAAAAECBAADBQYRBxIhEzEUFSIIFjNBGCRHUVZ3lqXD0+P/
 xAAUAQEAAAAAAAAAAAAAAAAAAAAA/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAwDAQACEQMR
 AD8AuX6UehP45/aXv9MTPTLVKxNSvMPcqu+a+XdLxf1SfJ6fU37PioTnOxfbOMc/KIZ7U/2V
 fmTR/wCaKlu6+blu/Ui72zxWtUmmUOrTaWwkWDT09FPR4K587OVrUfVsIwElPPPAbAjxr2um
 hWXbDu5rmfeApLPZ4hx0lzNm9aUJ9KAVHKlJHAPf7ozPLqWt9y6Z0EPGmoLNjTq48a1iaybJ
 YV52yEtCms5KJmAT61JXtJyUdyQTEc1WlMql7N1/oZ6jagVZVFfUyZPpFy5lvWcxU7Z03BUk
 GZLWJqVhPYLkIIPBEBtSEUyNAsjI1q1m/VP+UICwL/sqlXp7v+aOHsnyGttq218MtKd8+Ru2
 JXuScoO45Awe2CIi96aKW1cVyubkYVy6rTqz0J8a5t2qqZl0UjAMwYKScfPAJ+cIQHHP0Dth
 VFaMWt0XwxetnM50Ks2rsxL6ZMnJlJmb5hBBBEiVxjA28dznqo+hdksbQuS3Hs6tVtNzdM1Z
 /VH5nO3Bl/CJmYHKDynjv3zCEB5rLQNo0bIbydWNWxKljbLQLoWkISOAkBKAABCEID//2Q==
END:VCARD
''']
        for i, contact in enumerate(testcases):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = open(item, "w")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == len(testcases))

        # Check for the one expected event.
        # TODO: self.assertEqual([('added', 0, len(testcases))], view.events)
        self.view.events = []

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, len(testcases))
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        contact = copy.deepcopy(self.view.contacts[0])
        # Simplify the photo URI, if there was one. Avoid any assumptions
        # about the filename, except that it is a file:/// uri.
        if contact.has_key('photo'):
             contact['photo'] = re.sub('^file:///.*', 'file:///<stripped>', contact['photo'])
        if contact.has_key('id'):
             contact['id'] = '<stripped>'
        self.assertEqual({'full-name': 'John Doe',
                          'groups': ['TEST1', 'TEST2'],
                          'location': (30.12, -130.34),
                          'nickname': 'user1',
                          'structured-name': {'given': 'John', 'family': 'Doe'},
                          'birthday': (2006, 1, 8),
                          'photo': 'file:///<stripped>',
                          'roles': [
                       {
                        'organisation': 'Test Inc.',
                        'role': 'professional test case',
                        'title': 'Senior Tester',
                        },
                       ],
                          'source': [
                       (self.uidPrefix + 'foo', luids[0])
                       ],
                          'id': '<stripped>',
                          'notes': [
                       'This is a test case which uses almost all Evolution fields.',
                       ],
                          'emails': [
                       ('john.doe@home.priv', ['home']),
                       ('john.doe@other.world', ['other']),
                       ('john.doe@work.com', ['work']),
                       ('john.doe@yet.another.world', ['other']),
                       ],
                          'phones': [
                       ('business 1', ['voice', 'work']),
                       ('businessfax 4', ['fax', 'work']),
                       ('car 7', ['car']),
                       ('home 2', ['home', 'voice']),
                       ('homefax 5', ['fax', 'home']),
                       ('mobile 3', ['cell']),
                       ('pager 6', ['pager']),
                       ('primary 8', ['pref']),
                       ],
                          'addresses': [
                       ({'country': 'New Testonia',
                         'locality': 'Test Megacity',
                         'po-box': 'Test Box #3',
                         'postal-code': '12347',
                         'region': 'Test County',
                         'street': 'Test Drive 3'},
                        []),
                       ({'country': 'Old Testovia',
                         'locality': 'Test Town',
                         'po-box': 'Test Box #2',
                         'postal-code': '12346',
                         'region': 'Upper Test County',
                         'street': 'Test Drive 2'},
                        ['work']),
                       ({'country': 'Testovia',
                         'locality': 'Test Village',
                         'po-box': 'Test Box #1',
                         'postal-code': '12345',
                         'region': 'Lower Test County',
                         'street': 'Test Drive 1'},
                        ['home']),
                       ],
                          'urls': [
                       ('chat', ['x-video']),
                       ('free/busy', ['x-free-busy']),
                       ('http://john.doe.com', ['x-home-page']),
                       ('web log', ['x-blog']),
                       ],
                          },
                         # Order of list entries in the result is not specified.
                         # Must sort before comparing.
                         contact,
                         sortLists=True)

    def addressbooks(self):
        entries = os.listdir(os.path.join(os.environ["XDG_DATA_HOME"], "evolution", "addressbook"))
        entries.sort();
        # Ignore trash folder and system DB, because they may or may not be present.
        for db in ('trash', 'system'):
            try:
                entries.remove(db)
            except ValueError:
                pass
        return entries

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testRemove(self):
        '''TestContacts.testRemove - check that EDS database is created and removed'''
        self.setUpView(search=None)

        # Force sqlite DB files to exist by inserting a contact.
        testcases = [r'''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
TEL:1234-5678
EMAIL:john.doe@example.com
URL:http://john.doe.com
X-JABBER:jd@example.com
END:VCARD
''']
        for i, contact in enumerate(testcases):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = open(item, "w")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])

        self.assertEqual([self.managerPrefix + self.uid], self.addressbooks())
        self.manager.RemovePeer(self.uid)
        self.assertEqual([], self.addressbooks())

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testRemoveLive(self):
        '''TestContacts.testRemove - check that EDS database is created and removed while it is open in a view'''
        self.setUpView()

        # Force sqlite DB files to exist by inserting a contact.
        testcases = [r'''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
TEL:1234-5678
EMAIL:john.doe@example.com
URL:http://john.doe.com
X-JABBER:jd@example.com
END:VCARD
''']
        for i, contact in enumerate(testcases):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = open(item, "w")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) > 0)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(self.view.contacts))

        self.assertEqual([self.managerPrefix + self.uid], self.addressbooks())
        self.manager.RemovePeer(self.uid)
        self.assertEqual([], self.addressbooks())

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testStop(self):
        '''TestContacts.testStop - stop a started server'''
        # Auto-start.
        self.assertEqual(False, self.manager.IsRunning())
        self.setUpView(peers=['foo'])
        self.assertEqual(True, self.manager.IsRunning())

        # Must not stop now.
        self.manager.Stop()
        self.view.read(0, 0)

        # It may stop after closing the view.
        self.view.view.Close()
        self.manager.Stop()
        self.assertEqual(False, self.manager.IsRunning())

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testEmpty(self):
        '''TestContacts.testEmpty - start with empty view without databases'''
        self.setUpView(peers=[])
        # Let it run for a bit longer, to catch further unintentional changes.
        now = time.time()
        self.runUntil('delay',
                      check=lambda: (self.assertEqual([], self.view.errors),
                                     self.assertEqual([], self.view.contacts)),
                      until=lambda: time.time() - now > 10)

    @timeout(60)
    @property("snapshot", "simple-sort")
    def testMerge(self):
        '''TestContacts.testMerge - merge identical contacts from two stores'''
        self.setUpView(peers=['foo', 'bar'])

        # folks merges this because a) X-JABBER (always) b) EMAIL (only
        # with patch for https://bugzilla.gnome.org/show_bug.cgi?id=685401).
        john = '''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
TEL:1234-5678
EMAIL:john.doe@example.com
URL:http://john.doe.com
X-JABBER:jd@example.com
END:VCARD'''
        item = os.path.join(self.contacts, 'john.vcf')
        output = open(item, "w")
        output.write(john)
        output.close()

        check = self.view.setCheck(lambda: self.assertGreater(2, len(self.view.contacts)))

        luids = {}
        for uid in self.uids:
             logging.log('inserting John into ' + uid)
             out, err, returncode = self.runCmdline(['--import', item, '@' + self.managerPrefix + uid, 'local'])
             luids[uid] = self.extractLUIDs(out)

        # Run until the view has adapted. We accept Added + Removed + Added
        # (happens when folks switches IDs, which happens when the "primary" store
        # changes) and Added + Updated.
        #
        # Let it run for a bit longer, to catch further unintentional changes
        # and ensure that changes from both stores where processed.
        now = time.time()
        self.runUntil('delay',
                      check=check,
                      until=lambda: time.time() - now > 10)
        self.assertEqual(1, len(self.view.contacts))
        self.view.setCheck(None)

        # Read contact.
        logging.log('reading contact')
        self.view.read(0, 1)
        self.runUntil('contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        contact = copy.deepcopy(self.view.contacts[0])
        self.assertEqual({'full-name': 'John Doe',
                          'structured-name': {'given': 'John', 'family': 'Doe'},
                          'emails': [('john.doe@example.com', [])],
                          'phones': [('1234-5678', [])],
                          'urls': [('http://john.doe.com', ['x-home-page'])],
                          'id': contact.get('id', '<???>'),
                          'source': [
                       (self.uidPrefix + 'bar', luids[self.uidPrefix + 'bar'][0]),
                       (self.uidPrefix + 'foo', luids[self.uidPrefix + 'foo'][0])

                       ],
                          },
                         # Order of list entries in the result is not specified.
                         # Must sort before comparing.
                         contact,
                         sortLists=True)

    @timeout(int(os.environ.get('TESTPIM_TEST_ACTIVE_NUM', 10)) * (usingValgrind() and 15 or 6))
    @property("snapshot", "db-active")
    def testActive(self):
        '''TestContacts.testActive - reconfigure active address books several times'''

        contactsPerPeer = int(os.environ.get('TESTPIM_TEST_ACTIVE_NUM', 10))

        self.assertEqual(['', 'peer-' + self.uidPrefix + 'a', 'peer-' + self.uidPrefix + 'c'],
                         self.manager.GetActiveAddressBooks(),
                         sortLists=True)

        withLogging = (contactsPerPeer <= 10)
        checkPerformance = os.environ.get('TESTPIM_TEST_ACTIVE_RESPONSE', None)
        threshold = 0
        if checkPerformance:
             threshold = float(checkPerformance)

        # When starting the search right away and then add more
        # contact data later, we test the situation where new data
        # comes in because of a sync.
        #
        # When adding data first and then searching, we cover the
        # startup situation with already populated caches.
        #
        # Both are relevant scenarios. The first one stresses folks
        # more, because SyncEvolution adds contacts one at a time,
        # which then leads to many D-Bus messages containing a single
        # "contact added" notification that need to be processed by
        # folks. Testing this is the default.
        if os.environ.get('TESTPIM_TEST_ACTIVE_LOAD', False):
             # Delay starting the PIM Manager until data is ready to be read.
             # More realistic that way; otherwise folks must process new
             # contacts one-by-one.
             search=None
        else:
             # Start folks right away.
             search=''

        peers = ['a', 'b', 'c']
        self.setUpView(peers=peers, withSystemAddressBook=True,
                       search=search,
                       withLogging=withLogging)
        active = [''] + peers

        # Check that active databases were adapted and stored permanently.
        self.assertEqual(['', 'peer-' + self.uidPrefix + 'a', 'peer-' + self.uidPrefix + 'b', 'peer-' + self.uidPrefix + 'c'],
                         self.manager.GetActiveAddressBooks(),
                         sortLists=True)
        # Order mirrors the one of SetActiveAddressBooks() in setUpView(),
        # assuming that the PIM Manager preserves that order (not really guaranteed
        # by the API, but is how it is implemented).
        self.assertIn('active = pim-manager-' + self.uidPrefix + 'a pim-manager-' + self.uidPrefix + 'b pim-manager-' + self.uidPrefix + 'c system-address-book\n',
                      self.readManagerIni())

        for peer in active:
             for index in range(0, contactsPerPeer):
                  item = os.path.join(self.contacts, 'john%d.vcf' % index)
                  output = open(item, "w")
                  output.write('''BEGIN:VCARD
VERSION:3.0
FN:John_%(peer)s%(index)04d Doe
N:Doe;John_%(peer)s%(index)04d
END:VCARD''' % {'peer': peer, 'index': index})
                  output.close()

             uid = self.uidPrefix + peer
             logging.log('inserting data into ' + uid)
             if peer != '':
                  out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + uid, 'local'])
             else:
                  out, err, returncode = self.runCmdline(['--import', self.contacts, 'database=', 'backend=evolution-contacts'])

        # Ping server regularly and check that it remains responsive.
        # Depends on processing all D-Bus replies with minimum
        # delay, because delays caused by us would lead to false negatives.
        w = Watchdog(self, self.manager, threshold=threshold)
        if checkPerformance:
             w.start()
             self.cleanup.append(w.stop)

        # Start the view if not done yet and run until the view has adapted.
        if search == None:
             self.view.search('')
        self.runUntil('view with contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == contactsPerPeer * len(active),
                      may_block=checkPerformance)
        w.checkpoint('full view')

        def checkContacts():
             contacts = copy.deepcopy(self.view.contacts)
             for contact in contacts:
                  del contact['id']
                  del contact['source']
             expected = [{'full-name': first + ' Doe',
                                'structured-name': {'given': first, 'family': 'Doe'}} for \
                                    first in ['John_%(peer)s%(index)04d' % {'peer': peer,
                                                                            'index': index} \
                                                   for peer in active \
                                                   for index in range(0, contactsPerPeer)] ]
             return (expected, contacts)

        def assertHaveContacts():
             expected, contacts = checkContacts()
             self.assertEqual(expected, contacts)

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, contactsPerPeer * len(active))
        self.runUntil('contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, contactsPerPeer * len(active)),
                      may_block=not withLogging)
        assertHaveContacts()
        w.checkpoint('contact data')

        def haveExpectedView():
             current = time.time()
             logNow = withLogging or current - haveExpectedView.last > 1
             if len(self.view.contacts) == contactsPerPeer * len(active):
                  if self.view.haveData(0, contactsPerPeer * len(active)):
                       expected, contacts = checkContacts() # TODO: avoid calling this
                       if expected == contacts:
                            logging.log('got expected data')
                            return True
                       else:
                            if withLogging:
                                 logging.printf('data mismatch, keep waiting; currently have: %s', contacts)
                            elif logNow:
                                 logging.printf('data mismatch, keep waiting')
                  else:
                       self.view.read(0, len(self.view.contacts))
                       if logNow:
                            logging.log('still waiting for all data')
             elif logNow:
                  logging.printf('wrong contact count, keep waiting: have %d, want %d',
                                 len(self.view.contacts),
                                 contactsPerPeer * len(active))
             haveExpectedView.last = current
             return False
        haveExpectedView.last = time.time()

        # Now test all subsets until we are back at 'all active'.
        current = ['', 'a', 'b', 'c']
        for active in [filter(lambda x: x != None,
                              [s, a, b, c])
                       for s in [None, '']
                       for a in [None, 'a']
                       for b in [None, 'b']
                       for c in [None, 'c']]:
             logging.printf('changing address books %s -> %s', current, active)
             self.manager.SetActiveAddressBooks([x != '' and 'peer-' + self.uidPrefix + x or x for x in active])
             self.runUntil('contacts %s' % str(active),
                           check=lambda: self.assertEqual([], self.view.errors),
                           until=haveExpectedView,
                           may_block=not withLogging)
             w.checkpoint('%s -> %s' % (current, active))
             current = active


    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterExisting(self):
        '''TestContacts.testFilterExisting - check that filtering works when applied to static contacts'''
        self.setUpView()

        # Can refine full view. Doesn't change anything here.
        self.view.view.RefineSearch([])

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder())
        self.manager.SetSortOrder("first/last")

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/7888-99
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+1-89-7888-99
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with three contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3)

        # Check for the one expected event.
        # TODO: self.assertEqual([('added', 0, 3)], view.events)
        self.view.events = []

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Find Charly by his FN (case insensitive by default).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'chÁrles']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('charles',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])

        # We can expand the search with ReplaceSearch().
        view.view.ReplaceSearch([], False)
        self.runUntil('expanded view with three contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3)
        self.view.read(0, 3)
        self.runUntil('expanded contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Find Charly by his FN (case insensitive explicitly).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'chÁrless', 'case-insensitive']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('charles',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])

        # Find Charly by his FN (case sensitive explicitly).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'Chárleß', 'case-sensitive']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('charles',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])

        # Do not find Charly by his FN (case sensitive explicitly).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'charles', 'case-sensitive']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Find Abraham and Benjamin.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am']])
        self.runUntil('"am" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(2, len(view.contacts))
        view.read(0, 2)
        self.runUntil('two contacts',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', view.contacts[1]['structured-name']['given'])

        # Refine search without actually changing the result.
        for refine in [True, False]:
             view.quiescentCount = 0
             view.view.ReplaceSearch([['any-contains', 'am']], refine)
             self.runUntil('end of search refinement',
                           check=lambda: self.assertEqual([], view.errors),
                           until=lambda: view.quiescentCount > 0)
             self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
             self.assertEqual(u'Benjamin', view.contacts[1]['structured-name']['given'])

        # Restrict search to Benjamin. The result is a view
        # which has different indices than the full view.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']])
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine again, without changes.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']])
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine to empty view.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'XXXBenjamin']])
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Find Abraham by his nickname.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'ace']])
        self.runUntil('"ace" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('two contacts',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his email.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'az@']])
        self.runUntil('"az@" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('two contacts',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 1234 telephone number.
        view = ContactsView(self.manager)
        view.search([['any-contains', '1234']])
        self.runUntil('"1234" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('1234 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 1234 telephone number, as sub-string.
        view = ContactsView(self.manager)
        view.search([['any-contains', '23']])
        self.runUntil('"23" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('23 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 1234 telephone number, ignoring
        # formatting.
        view = ContactsView(self.manager)
        view.search([['any-contains', '12/34']])
        self.runUntil('"12/34" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('12/34 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham by his 56/78 telephone number, ignoring
        # slash in contact.
        view = ContactsView(self.manager)
        view.search([['any-contains', '5678']])
        self.runUntil('"5678" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('5678 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via the +1-800-FOOBAR vanity number.
        view = ContactsView(self.manager)
        view.search([['any-contains', '+1-800-foobar']])
        self.runUntil('"+1-800-foobar" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('+1-800-foobar data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via the +1-800-FOOBAR vanity number, with digits
        # instead of alpha characters.
        view = ContactsView(self.manager)
        view.search([['any-contains', '366227']])
        self.runUntil('"366227" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('366227 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via caller ID for +1-800-FOOBAR.
        view = ContactsView(self.manager)
        view.search([['phone', '+1800366227']])
        self.runUntil('"+1800366227" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('+1800366227 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via caller ID for 089/7888-99 (country is Germany).
        view = ContactsView(self.manager)
        view.search([['phone', '+4989788899']])
        self.runUntil('"+4989788899" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('+4989788899 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham via caller ID for +44 89 7888-99 (Abraham has no country code
        # set and matches, whereas Benjamin has +1 as country code and does not match).
        view = ContactsView(self.manager)
        view.search([['phone', '+4489788899']])
        self.runUntil('"+4489788899" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('+4489788899 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Find Abraham and Benjamin via 089/7888-99 (not a full caller
        # ID, but at least a valid phone number).  Benjamin matches
        # despite having +1 as country code because the search term
        # doesn't have a country code.
        view = ContactsView(self.manager)
        view.search([['phone', '089788899']])
        self.runUntil('"089788899" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(2, len(view.contacts))
        view.read(0, 2)
        self.runUntil('089788899 data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', view.contacts[1]['structured-name']['given'])

        # Don't find anyone.
        view = ContactsView(self.manager)
        view.search([['phone', '+49897888000']])
        self.runUntil('"+49897888000" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

    def doFilter(self, testdata, searches):
        self.setUpView()

        msg = None
        view = None
        try:
             # Insert new contacts and calculate their family names.
             names = []
             numtestcases = len(testdata)
             for i, contact in enumerate(testdata):
                  item = os.path.join(self.contacts, 'contact%d.vcf' % i)
                  output = codecs.open(item, "w", "utf-8")
                  if isinstance(contact, tuple):
                       # name + vcard
                       output.write(contact[1])
                       names.append(contact[0])
                  else:
                       # just the name
                       output.write(u'''BEGIN:VCARD
VERSION:3.0
FN:%(name)s
N:%(name)s;;;;
END:VCARD
''' % { 'name': contact })
                       names.append(contact)
                  output.close()

             logging.log('inserting contacts')
             out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
             # Relies on importing contacts sorted ascending by file name.
             luids = self.extractLUIDs(out)
             logging.printf('created contacts with luids: %s' % luids)

             # Run until the view has adapted.
             self.runUntil('view with three contacts',
                           check=lambda: self.assertEqual([], self.view.errors),
                           until=lambda: len(self.view.contacts) == numtestcases)

             # Check for the one expected event.
             # TODO: self.assertEqual([('added', 0, 3)], view.events)
             self.view.events = []

             # Read contacts.
             logging.log('reading contacts')
             self.view.read(0, numtestcases)
             self.runUntil('contacts',
                           check=lambda: self.assertEqual([], self.view.errors),
                           until=lambda: self.view.haveData(0, numtestcases))
             for i, name in enumerate(names):
                  msg = u'contact #%d with name %s in\n%s' % (i, name, pprint.pformat(self.stripDBus(self.view.contacts, sortLists=False)))
                  self.assertEqual(name, self.view.contacts[i]['full-name'])

             # Run searches and compare results.
             for i, (query, names) in enumerate(searches):
                  msg = u'query %s, names %s' % (query, names)
                  view = ContactsView(self.manager)
                  view.search(query)
                  self.runUntil('search %d: %s' % (i, query),
                                check=lambda: self.assertEqual([], view.errors),
                                until=lambda: view.quiescentCount > 0)
                  msg = u'query %s, names %s in\n%s' % (query, names, pprint.pformat(self.stripDBus(view.contacts, sortLists=False)))
                  self.assertEqual(len(names), len(view.contacts))
                  view.read(0, len(names))
                  self.runUntil('data %d: %s' % (i, query),
                                check=lambda: self.assertEqual([], view.errors),
                                until=lambda: view.haveData(0, len(names)))
                  for e, name in enumerate(names):
                       msg = u'query %s, names %s, name #%d %s in\n%s' % (query, names, e, name, pprint.pformat(self.stripDBus(view.contacts, sortLists=False)))
                       self.assertEqual(name, view.contacts[e]['full-name'])
        except Exception, ex:
             if msg:
                  info = sys.exc_info()
                  raise Exception('%s:\n%s' % (msg, repr(ex))), None, info[2]
             else:
                  raise
        return view

    @timeout(60)
    @property("ENV", "LC_TYPE=ja_JP.UTF-8 LC_ALL=ja_JP.UTF-8 LANG=ja_JP.UTF-8")
    def testFilterJapanese(self):
         self.doFilter(# Names of all contacts, sorted as expected.
                       ('111', u'1月', 'Bad'),
                       # Query + expected results.
                       (([], ('111', u'1月', 'Bad')),
                        ([['any-contains', '1']], ('111', u'1月')),
                        ([['any-contains', u'1月']], (u'1月',)))
                       )

    @timeout(60)
    @property("ENV", "LC_TYPE=zh_CN.UTF-8 LC_ALL=zh_CN.UTF-8 LANG=zh_CN.UTF-8")
    def testFilterChinesePinyin(self):
         self.doFilter(# Names of all contacts, sorted as expected.
                       # 江 = jiāng = Jiang when using Pinyin and thus after Jeffries and before Meadows.
                       # 鳥 = niǎo before 女性 = nǚ xìng (see FDO #66618)
                       ('Adams', 'Jeffries', u'江', 'jiang', 'Meadows', u'鳥', u'女性' ),
                       # 'J' may or may not match Jiang; by default, it matches.
                       (([['any-contains', 'J']], ('Jeffries', u'江', 'jiang')),
                        ([['any-contains', 'J', 'no-transliteration']], ('Jeffries', 'jiang')),
                        ([['any-contains', 'J', 'no-transliteration', 'case-sensitive']], ('Jeffries',)),
                        ([['any-contains', u'江']], (u'江', 'jiang')),
                        ([['any-contains', u'jiang']], (u'江', 'jiang')),
                        ([['any-contains', u'jiāng']], (u'江', 'jiang')),
                        ([['any-contains', u'jiāng', 'no-transliteration']], ('jiang',)),
                        ([['any-contains', u'jiāng', 'accent-sensitive']], (u'江',)),
                        ([['any-contains', u'jiāng', 'accent-sensitive', 'case-sensitive']], (u'江',)),
                        ([['any-contains', u'Jiāng', 'accent-sensitive', 'case-sensitive']], ()),
                        ([['any-contains', u'Jiang']], (u'江', 'jiang')),
                        ),
                       )

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterGermany(self):
         self.doFilter(# Names of all contacts, sorted as expected.
                       # DIN 5007 Variant 2 defines phone book sorting in
                       # Germany. It does not apply to Austria.
                       # Example from http://de.wikipedia.org/wiki/Alphabetische_Sortierung
                       (u'Göbel', u'Goethe', u'Göthe', u'Götz', u'Goldmann'),
                       (),
                       )

    @timeout(60)
    @property("ENV", "LC_TYPE=zh_CN.UTF-8 LANG=zh_CN.UTF-8")
    def testLocaled(self):
         # Use mixed Chinese/Western names, because then the locale really matters.
         namespinyin = ('Adams', 'Jeffries', u'江', 'Meadows', u'鳥', u'女性' )
         namesgerman = ('Adams', 'Jeffries', 'Meadows', u'女性', u'江', u'鳥' )
         numtestcases = len(namespinyin)
         self.doFilter(namespinyin, ())

         daemon = localed.Localed()
         msg = None
         try:
              # Broadcast Locale value together with PropertiesChanged signal.
              self.view.quiescentCount = 0
              daemon.SetLocale(['LC_TYPE=de_DE.UTF-8', 'LANG=de_DE.UTF-8'], False)
              logging.log('reading contacts, German')
              self.runUntil('German sorting',
                            check=lambda: self.assertEqual([], self.view.errors),
                            until=lambda: self.view.quiescentCount > 1)
              self.view.read(0, numtestcases)
              self.runUntil('German contacts',
                            check=lambda: self.assertEqual([], self.view.errors),
                            until=lambda: self.view.haveData(0, numtestcases))
              for i, name in enumerate(namesgerman):
                   msg = u'contact #%d with name %s in\n%s' % (i, name, pprint.pformat(self.stripDBus(self.view.contacts, sortLists=False)))
                   self.assertEqual(name, self.view.contacts[i]['full-name'])

              # Switch back to Pinyin without including the new value.
              self.view.quiescentCount = 0
              daemon.SetLocale(['LC_TYPE=zh_CN.UTF-8', 'LANG=zh_CN.UTF-8'], True)
              logging.log('reading contacts, Pinyin')
              self.runUntil('Pinyin sorting',
                            check=lambda: self.assertEqual([], self.view.errors),
                            until=lambda: self.view.quiescentCount > 1)
              self.view.read(0, numtestcases)
              self.runUntil('Pinyin contacts',
                            check=lambda: self.assertEqual([], self.view.errors),
                            until=lambda: self.view.haveData(0, numtestcases))
              for i, name in enumerate(namespinyin):
                   msg = u'contact #%d with name %s in\n%s' % (i, name, pprint.pformat(self.stripDBus(self.view.contacts, sortLists=False)))
                   self.assertEqual(name, self.view.contacts[i]['full-name'])
         except Exception, ex:
             if msg:
                  info = sys.exc_info()
                  raise Exception('%s:\n%s' % (msg, repr(ex))), None, info[2]
             else:
                  raise
         finally:
              daemon.remove_from_connection()

    @timeout(60)
    # Must disable usage of pre-computed phone numbers from EDS, because although we can
    # tell EDS about locale changes, it currently crashes when we do that (https://bugs.freedesktop.org/show_bug.cgi?id=59571#c20).
    #
    # Remove the SYNCEVOLUTION_PIM_EDS_NO_E164=1 part from ENV to test and use EDS.
    @property("ENV", "LANG=en_US.UTF-8 SYNCEVOLUTION_PIM_EDS_NO_E164=1")
    def testLocaledPhone(self):
         # Parsing of 01164 3 331 6005 depends on locale: in the US, 011 is followed
         # by a country code, and therefore libphonenumber parses the string as
         # +64 (for New Zealand) with 3 331 6005 as national number.
         # Germany (and other countries) don't have that prefix; libphonenumber
         # ends up returning 116433316005 as national number with +49 as the
         # default country code. Same example as in phonenumberutil_test.cc.
         #
         # We use this difference to determine whether the desired locale was used
         # for normalizing the phone number.

         usingEDS = not 'SYNCEVOLUTION_PIM_EDS_NO_E164=1' in self.getTestProperty("ENV", "")

         if usingEDS:
              daemon = localed.Localed()
              daemon.SetLocale(['LANG=en_US.UTF-8'], True)
              # Give EDS some time to notice the new daemon and it's en_US setting.
              Timeout.addTimeout(5, loop.quit)
              loop.run()

         testcases = (('Doe', '''BEGIN:VCARD
VERSION:3.0
FN:Doe
N:Doe;;;;
TEL:01164 3 331 6005
END:VCARD
'''),)
         names = ('Doe')
         numtestcases = len(testcases)
         view = self.doFilter(testcases,
                              (([['phone', '+64 3 331 6005']], ('Doe',)),))

         msg = None
         if not usingEDS:
              # Don't do that too early, otherwise EDS also sees the daemon
              # and crashes (https://bugs.freedesktop.org/show_bug.cgi?id=59571#c20).
              daemon = localed.Localed()
         try:
              # Contact no longer matched because it's phone number normalization
              # becomes different.
              view.quiescentCount = 0
              daemon.SetLocale(['LANG=de_DE.UTF-8'], True)
              self.runUntil('German locale',
                            check=lambda: self.assertEqual([], view.errors),
                            until=lambda: view.quiescentCount > 1)
              self.assertEqual(len(view.contacts), 0)

              # Switch back to US.
              view.quiescentCount = 0
              daemon.SetLocale(['LANG=en_US.UTF-8'], True)
              self.runUntil('US locale',
                            check=lambda: self.assertEqual([], view.errors),
                            until=lambda: view.quiescentCount > 1)
              self.assertEqual(len(view.contacts), 1)
         except Exception, ex:
             if msg:
                  info = sys.exc_info()
                  raise Exception('%s:\n%s' % (msg, repr(ex))), None, info[2]
             else:
                  raise
         finally:
              daemon.remove_from_connection()

    # Not supported correctly by ICU?
    # See icu-support "Subject: Austrian phone book sorting"
    # @timeout(60)
    # @property("ENV", "LC_TYPE=de_AT.UTF-8 LC_ALL=de_AT.UTF-8 LANG=de_AT.UTF-8")
    # def testFilterAustria(self):
    #      self.doFilter(# Names of all contacts, sorted as expected.
    #                    # Austrian phone book sorting.
    #                    # Example from http://de.wikipedia.org/wiki/Alphabetische_Sortierung
    #                    (u'Goethe', u'Goldmann', u'Göbel', u'Göthe', u'Götz'),
    #                    (),
    #                    )

    @timeout(60)
    def testFilterLogic(self):
         '''TestContacts.testFilterLogic - check logic operators'''
         self.doFilter(('Xing', 'Yeah', 'Zooh'),
                       ((['or', ['any-contains', 'X'], ['any-contains', 'Z']], ('Xing', 'Zooh')),
                        (['or', ['any-contains', 'X']], ('Xing',)),
                        (['or', ['any-contains', 'Z']], ('Zooh',)),
                        (['or'], ()),
                        (['and', ['any-contains', 'h'], ['any-contains', 'Z']], ('Zooh',)),
                        (['and', ['any-contains', 'h']], ('Yeah', 'Zooh')),
                        (['and', ['any-contains', 'Z']], ('Zooh',)),
                        (['and', ['any-contains', 'h'], ['any-contains', 'Z'], ['any-contains', 'A']], ()),
                        (['and'], ()),
                        # Python D-Bus does not like mixing elements of different types in a list.
                        # In a tuple that's fine, and also works with the PIM Manager.
                        (('or', ('and', ('any-contains', 'h'), ('any-contains', 'Z')), ('any-contains', 'X')), ('Xing', 'Zooh')),
                        (('and', ('or', ('any-contains', 'X'), ('any-contains', 'Z')), ('any-contains', 'h')), ('Zooh',))))

    @timeout(60)
    def testFilterFields(self):
         '''TestContacts.testFilterFields - check filter operations on fields'''
         self.doFilter([('John Doe', r'''BEGIN:VCARD
VERSION:3.0
URL:http://john.doe.com
TITLE:Senior Tester
ORG:Test Inc.;Testing;test#1
ROLE:professional test case
X-EVOLUTION-MANAGER:John Doe Senior
X-EVOLUTION-ASSISTANT:John Doe Junior
NICKNAME:user1
BDAY:2006-01-08
X-EVOLUTION-ANNIVERSARY:2006-01-09
X-EVOLUTION-SPOUSE:Joan Doe
NOTE:This is a test case which uses almost all Evolution fields.
FN:John Doe
N:Doe;John;Johnny;;
X-EVOLUTION-FILE-AS:Doe\, John
CATEGORIES:TEST
X-EVOLUTION-BLOG-URL:web log
GEO:30.12;-130.34
CALURI:calender
FBURL:free/busy
X-EVOLUTION-VIDEO-URL:chat
X-MOZILLA-HTML:TRUE
ADR;TYPE=WORK:Test Box #2;;Test Drive 2;Test Town;Upper Test County;12346;O
 ld Testovia
LABEL;TYPE=WORK:Test Drive 2\nTest Town\, Upper Test County\n12346\nTest Bo
 x #2\nOld Testovia
ADR;TYPE=HOME:Test Box #1;;Test Drive 1;Test Village;Lower Test County;1234
 5;Testovia
LABEL;TYPE=HOME:Test Drive 1\nTest Village\, Lower Test County\n12345\nTest
  Box #1\nTestovia
ADR:Test Box #3;Test Extension;Test Drive 3;Test Megacity;Test County;12347;New Testonia
LABEL;TYPE=OTHER:Test Drive 3\nTest Megacity\, Test County\n12347\nTest Box
  #3\nNew Testonia
UID:pas-id-43C0ED3900000001
EMAIL;TYPE=WORK;X-EVOLUTION-UI-SLOT=1:john.doe@work.com
EMAIL;TYPE=HOME;X-EVOLUTION-UI-SLOT=2:john.doe@home.priv
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=3:john.doe@other.world
EMAIL;TYPE=OTHER;X-EVOLUTION-UI-SLOT=4:john.doe@yet.another.world
TEL;TYPE=work;TYPE=Voice;X-EVOLUTION-UI-SLOT=1:business 1
TEL;TYPE=homE;TYPE=VOICE;X-EVOLUTION-UI-SLOT=2:home 2
TEL;TYPE=CELL;X-EVOLUTION-UI-SLOT=3:mobile 3
TEL;TYPE=WORK;TYPE=FAX;X-EVOLUTION-UI-SLOT=4:businessfax 4
TEL;TYPE=HOME;TYPE=FAX;X-EVOLUTION-UI-SLOT=5:homefax 5
TEL;TYPE=PAGER;X-EVOLUTION-UI-SLOT=6:pager 6
TEL;TYPE=CAR;X-EVOLUTION-UI-SLOT=7:car 7
TEL;TYPE=PREF;X-EVOLUTION-UI-SLOT=8:primary 8
TEL:12 34-5
END:VCARD
''')],
                       ((['is', 'full-name', 'john doe'], ('John Doe',)),
                        (['is', 'full-name', 'John Doe', 'case-sensitive'], ('John Doe',)),
                        (['is', 'full-name', 'john doe', 'case-sensitive'], ()),
                        (['is', 'full-name', 'John Doe', 'case-insensitive'], ('John Doe',)),
                        (['is', 'full-name', 'john'], ()),

                        (['contains', 'full-name', 'ohn d'], ('John Doe',)),
                        (['contains', 'full-name', 'ohn D', 'case-sensitive'], ('John Doe',)),
                        (['contains', 'full-name', 'ohn d', 'case-sensitive'], ()),
                        (['contains', 'full-name', 'ohn d', 'case-insensitive'], ('John Doe',)),
                        (['contains', 'full-name', 'foobar'], ()),

                        (['begins-with', 'full-name', 'john'], ('John Doe',)),
                        (['begins-with', 'full-name', 'John', 'case-sensitive'], ('John Doe',)),
                        (['begins-with', 'full-name', 'john', 'case-sensitive'], ()),
                        (['begins-with', 'full-name', 'John', 'case-insensitive'], ('John Doe',)),
                        (['begins-with', 'full-name', 'doe'], ()),

                        (['ends-with', 'full-name', 'doe'], ('John Doe',)),
                        (['ends-with', 'full-name', 'Doe', 'case-sensitive'], ('John Doe',)),
                        (['ends-with', 'full-name', 'doe', 'case-sensitive'], ()),
                        (['ends-with', 'full-name', 'Doe', 'case-insensitive'], ('John Doe',)),
                        (['ends-with', 'full-name', 'john'], ()),

                        (['is', 'nickname', 'user1'], ('John Doe',)),
                        (['is', 'nickname', 'Johnny'], ()),
                        (['is', 'structured-name/family', 'Doe'], ('John Doe',)),
                        (['is', 'structured-name/family', 'John'], ()),
                        (['is', 'structured-name/given', 'John'], ('John Doe',)),
                        (['is', 'structured-name/given', 'Doe'], ()),
                        (['is', 'structured-name/additional', 'Johnny'], ('John Doe',)),
                        (['is', 'structured-name/additional', 'John'], ()),
                        (['is', 'emails/value', 'john.doe@work.com'], ('John Doe',)),
                        (['is', 'emails/value', 'foo@abc.com'], ()),
                        (['is', 'addresses/po-box', 'Test Box #3'], ('John Doe',)),
                        (['is', 'addresses/po-box', 'Foo Box'], ()),
                        (['is', 'addresses/extension', 'Test Extension'], ('John Doe',)),
                        (['is', 'addresses/extension', 'Foo Extension'], ()),
                        (['is', 'addresses/street', 'Test Drive 3'], ('John Doe',)),
                        (['is', 'addresses/street', 'Rodeo Drive'], ()),
                        (['is', 'addresses/locality', 'Test Megacity'], ('John Doe',)),
                        (['is', 'addresses/locality', 'New York'], ()),
                        (['is', 'addresses/region', 'Test County'], ('John Doe',)),
                        (['is', 'addresses/region', 'Testovia'], ()),
                        (['is', 'addresses/postal-code', '54321'], ()),
                        (['is', 'addresses/country', 'New Testonia'], ('John Doe',)),
                        (['is', 'addresses/country', 'America'], ()),

                        (['is', 'phones/value', 'business 1'], ('John Doe',)),
                        (['is', 'phones/value', 'business 123'], ()),
                        (['is', 'phones/value', '12345'], ('John Doe',)),
                        (['is', 'phones/value', '123456'], ()),
                        ))

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterQuiescence(self):
        '''TestContacts.testFilterQuiescence - check that starting server via filter leads to quiescence signal'''
        self.setUpView(peers=[], withSystemAddressBook=True, search=[('any-contains', 'foo')])
        self.assertEqual(1, self.view.quiescentCount)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFullQuiescence(self):
        '''TestContacts.testFullQuiescence - check that starting server via filter leads to quiescence signal'''
        self.setUpView(peers=[], withSystemAddressBook=True, search=[])
        self.assertEqual(1, self.view.quiescentCount)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLive(self):
        '''TestContacts.testFilterLive - check that filtering works while adding contacts'''
        self.setUpView()

        self.manager.SetSortOrder("first/last")

        # Find Charly by his FN (case insensitive by default).
        view = ContactsView(self.manager)
        view.search([['any-contains', 'chÁrles']])
        self.runUntil('charles search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Search for telephone number.
        phone = ContactsView(self.manager)
        phone.search([['phone', '1234']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], phone.errors),
                      until=lambda: phone.quiescentCount > 0)
        self.assertEqual(1, len(phone.contacts))

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Unmatched contact remains unmatched.
        # Modified phone number no longer matched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:King
TEL:123
EMAIL:az@example.com
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham')
        # Check as part of event processing:
        # - view has one unmodified contact
        check1 = view.setCheck(lambda: (self.assertEqual(1, len(view.contacts)),
                                        self.assertIsInstance(view.contacts[0], dict)))
        # - self.view changes, must not encounter errors
        check2 = self.view.setCheck(None)
        check = lambda: (check1(), check2())

        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed',
                      check=check,
                      until=lambda: self.view.haveNoData(0))
        self.view.read(0, 1)
        self.runUntil('Abraham nickname read',
                      check=check,
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # No longer part of the telephone search view.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], phone.errors),
                      until=lambda: len(phone.contacts) == 0)
        phone.close()

        # Matched contact remains matched, but we loose the data.
        check1 = view.setCheck(lambda: self.assertEqual(1, len(view.contacts)))
        item = os.path.join(self.contacts, 'contact%d.vcf' % 2)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
NICKNAME:Angel
N:Xing;Charly
END:VCARD''')
        output.close()
        logging.log('change nick of Charly')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[2]])
        self.runUntil('Charly nickname changed',
                      check=check,
                      until=lambda: self.view.haveNoData(2) and \
                           view.haveNoData(0))
        view.read(0, 1)
        self.view.read(2, 1)
        self.runUntil('Charly nickname read',
                      check=check,
                      until=lambda: self.view.haveData(2) and \
                           view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Unmatched contact gets matched.
        check1 = view.setCheck(lambda: self.assertLess(0, len(view.contacts)))
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Chárleß alter ego
TEL:1234
EMAIL:az@example.com
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham, II')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and \
                           len(view.contacts) == 2)
        check1 = view.setCheck(lambda: self.assertEqual(2, len(view.contacts)))
        self.assertNotIsInstance(view.contacts[0], dict)
        self.assertIsInstance(view.contacts[1], dict)
        view.read(0, 1)
        self.view.read(0, 1)
        self.runUntil('Abraham nickname read, II',
                      check=check,
                      until=lambda: self.view.haveData(0) and \
                           view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # Invert sort order.
        check1 = view.setCheck(None)
        self.manager.SetSortOrder("last/first")
        self.runUntil('reordering',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 2))
        view.read(0, 2)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts',
                      check=check,
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 2))
        self.assertEqual(2, len(view.contacts))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', view.contacts[1]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[2]['structured-name']['given'])

        # And back again.
        self.manager.SetSortOrder("first/last")
        self.runUntil('reordering, II',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 2))
        view.read(0, 2)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts, II',
                      check=check,
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 2))
        self.assertEqual(2, len(view.contacts))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', view.contacts[1]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Matched contact gets unmatched.
        check1 = view.setCheck(lambda: self.assertLess(0, len(view.contacts)))
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:None
TEL:1234
EMAIL:az@example.com
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham, II')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed to None',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and \
                           len(view.contacts) == 1)
        self.assertIsInstance(view.contacts[0], dict)
        self.view.read(0, 1)
        check1 = view.setCheck(lambda: (self.assertEqual(1, len(view.contacts)),
                                        self.assertIsInstance(view.contacts[0], dict)))
        self.runUntil('Abraham nickname read, None',
                      check=check,
                      until=lambda: self.view.haveData(0))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # Finally, remove everything.
        logging.log('remove contacts')
        check1 = view.setCheck(None)
        out, err, returncode = self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])
        self.runUntil('all contacts removed',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: len(self.view.contacts) == 0 and \
                           len(view.contacts) == 0)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterExistingLimit(self):
        '''TestContacts.testFilterExistingLimit - check that filtering works when applied to static contacts, with maximum number of results'''
        self.setUpView()

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder())
        self.manager.SetSortOrder("first/last")

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/7888-99
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+1-89-7888-99
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with three contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 3)

        # Check for the one expected event.
        # TODO: self.assertEqual([('added', 0, 3)], view.events)
        self.view.events = []

        # Read contacts.
        logging.log('reading contacts')
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Browse initial two contacts (= uses MatchAll filter with limit).
        view = ContactsView(self.manager)
        view.search([['limit', '2']])
        self.runUntil('browse results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(2, len(view.contacts))
        view.read(0, 2)
        self.runUntil('browse data',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        view.close()

        # Find Abraham and Benjamin but stop at first contact.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('"am" search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Changing the limit is not supported.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'.*: refining the search must not change the maximum number of results$'):
             view.view.RefineSearch([['limit', '3'], ['any-contains', 'foo']])

        # Refine search without actually changing the result.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])

        # Restrict search to Benjamin. We can leave out the limit, the old
        # stays active automatically. Abraham drops out of the view
        # and Benjamin enters the result subset.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']])
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertFalse(view.haveData(0))
        view.read(0, 1)
        self.runUntil('Benjamin',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine again, without changes.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'Benjamin']])
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

        # Refine to empty view.
        view.quiescentCount = 0
        view.view.RefineSearch([['any-contains', 'XXXBenjamin']])
        self.runUntil('end of search refinement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Expand back to view with Benjamin.
        view.quiescentCount = 0
        view.view.ReplaceSearch([['any-contains', 'Benjamin']], False)
        self.runUntil('end of search replacement',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))
        view.read(0, 1)
        self.runUntil('Benjamin',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.haveData(0))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLiveLimit(self):
        '''TestContacts.testFilterLiveLimit - check that filtering works while modifying contacts, with a maximum number of results'''
        self.setUpView()

        self.manager.SetSortOrder("first/last")

        # Find Abraham and Benjamin, but limit results to first one.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('am search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:Ace
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0 and len(self.view.contacts) == 3)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Check conditions as part of view updates.
        check1 = view.setCheck(None)
        check2 = self.view.setCheck(None)
        check = lambda: (check1(), check2())

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=check,
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Matched contact remains matched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:King
END:VCARD''')
        output.close()
        logging.log('change nick of Abraham')
        check1 = view.setCheck(lambda: self.assertEqual(1, len(view.contacts)))
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham nickname changed',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and view.haveNoData(0))
        view.read(0, 1)
        self.view.read(0, 1)
        self.runUntil('Abraham nickname read',
                      check=check,
                      until=lambda: self.view.haveData(0) and view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])

        # Unmatched contact gets matched, but stays out of view.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
NICKNAME:mamam
END:VCARD''')
        output.close()
        logging.log('change nick of Charly')
        check1 = view.setCheck(lambda: self.assertLess(0, len(view.contacts)))
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[2]])
        self.runUntil('Charly nickname changed',
                      check=check,
                      until=lambda: self.view.haveNoData(2))
        check1 = view.setCheck(lambda: self.assertEqual(1, len(view.contacts)))
        self.assertEqual(1, len(view.contacts))
        self.assertTrue(view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertFalse(self.view.haveData(2))
        self.view.read(2, 1)
        self.runUntil('Abraham nickname read, II',
                      check=check,
                      until=lambda: self.view.haveData(2) and \
                           view.haveData(0))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Invert sort order.
        check1 = view.setCheck(None)
        self.manager.SetSortOrder("last/first")
        self.runUntil('reordering',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 1))
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts',
                      check=check,
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 1))
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Charly', view.contacts[0]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[2]['structured-name']['given'])

        # And back again.
        self.manager.SetSortOrder("first/last")
        self.runUntil('reordering, II',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and \
                           self.view.haveNoData(2) and \
                           view.haveNoData(0, 1))
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('read reordered contacts, II',
                      check=check,
                      until=lambda: self.view.haveData(0, 3) and \
                           view.haveData(0, 1))
        self.assertEqual(1, len(view.contacts))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(3, len(self.view.contacts))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Matched contact gets unmatched.
        item = os.path.join(self.contacts, 'contact%d.vcf' % 0)
        output = codecs.open(item, "w", "utf-8")
        output.write(u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abrahan
FN:Abrahan Zoo
NICKNAME:None
END:VCARD''')
        output.close()
        logging.log('change name of Abraham')
        out, err, returncode = self.runCmdline(['--update', item, '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('Abraham -> Abrahan',
                      check=check,
                      until=lambda: self.view.haveNoData(0) and \
                           len(view.contacts) == 1 and \
                           view.haveNoData(0))
        check1 = view.setCheck(lambda: self.assertEqual(1, len(view.contacts)))
        view.read(0, 1)
        self.view.read(0, 1)
        self.runUntil('Abrahan read',
                      check=check,
                      until=lambda: self.view.haveData(0) and view.haveData(0))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abrahan', self.view.contacts[0]['structured-name']['given'])

        # Finally, remove everything.
        logging.log('remove contacts')
        check1 = view.setCheck(None)
        out, err, returncode = self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', '*'])
        self.runUntil('all contacts removed',
                      check=check,
                      until=lambda: len(self.view.contacts) == 0 and \
                           len(view.contacts) == 0)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLiveLimitInverted(self):
        '''TestContacts.testFilterLiveLimitInverted - check that filtering works while modifying contacts, with a maximum number of results, and inverted sort order'''
        self.setUpView()

        self.manager.SetSortOrder("last/first")

        # Find Benjamin and Abraham, but limit results to first one.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('am search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:Ace
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
FN:Benjamin Yeah
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0 and len(self.view.contacts) == 3)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[2]['structured-name']['given'])

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testFilterLiveLimitRemove(self):
        '''TestContacts.testFilterLiveLimitRemove - check that filtering works while removing a contact, with a maximum number of results'''
        self.setUpView()

        self.manager.SetSortOrder("first/last")

        # Find Abraham and Benjamin, but limit results to first one.
        view = ContactsView(self.manager)
        view.search([['any-contains', 'am'], ['limit', '1']])
        self.runUntil('am search results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(0, len(view.contacts))

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
FN:Abraham Zoo
NICKNAME:Ace
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
END:VCARD''',

# Chárleß has chárless as representation after folding the case.
# This is different from lower case.
# See http://www.boost.org/doc/libs/1_51_0/libs/locale/doc/html/glossary.html#term_case_folding
u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])
        # Relies on importing contacts sorted ascending by file name.
        luids = self.extractLUIDs(out)
        logging.printf('created contacts with luids: %s' % luids)

        # Run until the view has adapted.
        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) > 0 and len(self.view.contacts) == 3)
        # Don't wait for more contacts here. They shouldn't come, and if
        # they do, we'll notice it below.
        self.assertEqual(1, len(view.contacts))

        # Read contacts.
        logging.log('reading contacts')
        view.read(0, 1)
        self.view.read(0, 3)
        self.runUntil('contacts',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 3))
        self.assertEqual(u'Abraham', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[2]['structured-name']['given'])

        # Remove Abraham. Gets replaced by Benjamin in the view.
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', luids[0]])
        self.runUntil('view with Benjamin',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) == 1 and view.haveNoData(0) and len(self.view.contacts) == 2)
        view.read(0, 1)
        self.runUntil('Benjamin',
                      check=lambda: (self.assertEqual([], view.errors),
                                     self.assertEqual([], self.view.errors)),
                      until=lambda: view.haveData(0) and \
                           self.view.haveData(0, 2))
        self.assertEqual(u'Benjamin', view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Charly', self.view.contacts[1]['structured-name']['given'])

        # Remove Benjamin.
        self.runCmdline(['--delete-items', '@' + self.managerPrefix + self.uid, 'local', luids[1]])
        self.runUntil('view without Benjamin',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) == 0 and len(self.view.contacts) == 1)
        self.assertEqual(u'Charly', self.view.contacts[0]['structured-name']['given'])

    @timeout(60)
    @property("snapshot", "simple-sort")
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8")
    def testContactWrite(self):
        '''TestContacts.testContactWrite - add, update and remove contact'''
        self.setUpView(peers=[], withSystemAddressBook=True)

        # Use unicode strings to make the assertEqual output nicer in case
        # of a mismatch. It's not necessary otherwise.
        #
        # This covers all fields which can be written by the folks EDS
        # backend.
        john = {
             'full-name': 'John Doe',
             'groups': ['Foo', 'Bar'],
             'location': (30.12, -130.34),
             'structured-name': {
                  'family': 'Doe',
                  'given': 'John',
                  'additional': 'D.',
                  'prefixes': 'Mr.',
                  'suffixes': 'Sr.'
                  },
             # 'nickname': 'Johnny', TODO: should be stored by folks, currently not supported
             'birthday': (2011, 12, 1),
             # 'photo': 'file:///tmp/photo.png', TODO: test with real file, folks will store the content of it
             # 'gender', 'male', not exposed via D-Bus
             # 'im': ...
             # 'is-favorite': ...
             'emails': [
                  ( 'john.doe@work', [ 'work' ] ),
                  ( 'john@home', [ 'home' ] ),
                  ],
             'phones': [
                  ( '1234', ['fax']),
                  ( '5678', ['cell', 'work'] ),
                  ( 'foobar', dbus.Array(signature="s")), # empty string list
                  ],
             'addresses': [
                  (
                       {
                            'country': 'United States of America',
                            'extension': 'ext',
                            'locality': 'New York',
                            'po-box': 'box',
                            'region': 'NY',
                            'street': 'Lower East Side',
                            },
                       ['work']
                       ),
                  (
                       {
                            'locality': 'Boston',
                            'street': 'Main Street',
                            },
                       dbus.Array(signature="s") # empty string list
                       ),
                  ],
             # 'web-services'
             'roles': [
                  {
                       'organisation': 'ACME',
                       'title': 'president',
                       'role': 'decision maker',
                       },
                  {
                       'organisation': 'BAR',
                       'title': 'CEO',
                       'role': 'spokesperson',
                       },
                  ],
             'notes': [
                  'note\n\ntext',
                  # TODO: notes -> note (EDS only supports one NOTE)
                  ],
             'urls': [
                  ('chat', ['x-video']),
                  ('free/busy', ['x-free-busy']),
                  ('http://john.doe.com', ['x-home-page']),
                  ('web log', ['x-blog']),
                  ],
             }

        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'.*: only the system address book is writable'):
             self.manager.AddContact('no-such-address-book',
                                     john)

        # Add new contact.
        localID = self.manager.AddContact('', john)
        john['source'] = [('', unicode(localID))]

        self.runUntil('view with one contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) > 0)
        self.assertEqual(1, len(self.view.contacts))
        self.view.read(0, 1)
        self.runUntil('contact data',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0))
        contact = self.view.contacts[0]
        john['id'] = contact.get('id', '<???>')
        self.assertEqual(john, contact, sortLists=True)

        with self.assertRaisesRegexp(dbus.DBusException,
                                     r'''.*: contact with local ID 'no-such-local-id' not found in system address book'''):
             self.manager.ModifyContact('',
                                        'no-such-local-id',
                                        john)

        # Update the contact.
        john = {
             'source': [('', localID)],
             'id': contact.get('id', '<???>'),

             'full-name': 'John A. Doe',
             'groups': ['Foo', 'Bar'],
             'location': (30.12, -130.34),
             'structured-name': {
                  'family': 'Doe',
                  'given': 'John',
                  'additional': 'A.',
                  'prefixes': 'Mr.',
                  'suffixes': 'Sr.'
                  },
             # 'nickname': 'Johnny', TODO: should be stored by folks, currently not supported - https://bugzilla.gnome.org/show_bug.cgi?id=686695
             'birthday': (2011, 12, 24),
             # 'photo': 'file:///tmp/photo.png', TODO: test with real file, folks will store the content of it
             # 'gender', 'male', not exposed via D-Bus
             # 'im': ...
             # 'is-favorite': ...
             'emails': [
                  ( 'john2@home', [ 'home' ] ),
                  ],
             'phones': [
                  ( '1234', ['fax']),
                  ( '56789', ['work'] ),
                  ],
             'addresses': [
                  (
                       {
                            'country': 'United States of America',
                            'extension': 'ext',
                            'locality': 'New York',
                            'po-box': 'box',
                            'region': 'NY',
                            'street': 'Upper East Side',
                            },
                       ['work']
                       ),
                  (
                       {
                            'country': 'United States of America',
                            'locality': 'Boston',
                            'street': 'Main Street',
                            },
                       dbus.Array(signature="s") # empty string list
                       ),
                  ],
             # 'web-services'
             'roles': [
                  {
                       'organisation': 'ACME',
                       'title': 'senior president',
                       'role': 'scapegoat',
                       },
                  ],
             'notes': [
                  'note\n\ntext modified',
                  # TODO: notes -> note (EDS only supports one NOTE)
                  ],
             'urls': [
                  ('http://john.A.doe.com', ['x-home-page']),
                  ('web log 2', ['x-blog']),
                  ],
             }
        self.manager.ModifyContact('', localID, john)
        self.runUntil('modified contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 1 and self.view.haveNoData(0))
        # Keep asking for data for a while: we may get "modified" signals multiple times,
        # which invalidates data that we just read until the unified address book
        # is stable again.
        start = time.time()
        self.runUntil('modified contact data',
                      check=lambda: (self.assertEqual([], self.view.errors),
                                     self.view.haveData(0) or self.view.read(0, 1) or True),
                      until=lambda: self.view.haveData(0) and time.time() - start > 5)
        self.assertEqual(john, self.view.contacts[0], sortLists=True)

        # Search for modified telephone number.
        # Depends on having a valid country set via env variables.
        view = ContactsView(self.manager)
        view.search([['phone', '56789']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: view.quiescentCount > 0)
        self.assertEqual(1, len(view.contacts))

        # Remove all properties, except for a minimal name.
        # Entirely emtpy contacts make no sense.
        john = {
             'source': [('', localID)],
             'id': contact.get('id', '<???>'),

             'full-name': 'nobody',
             'structured-name': {
                  'given': 'nobody',
                  },
             }
        self.manager.ModifyContact('', localID, john)
        self.runUntil('modified contact',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 1 and self.view.haveNoData(0))
        start = time.time()
        self.runUntil('modified contact data',
                      check=lambda: (self.assertEqual([], self.view.errors),
                                     self.view.haveData(0) or self.view.read(0, 1) or True),
                      until=lambda: self.view.haveData(0) and time.time() - start > 5)
        self.assertEqual(john, self.view.contacts[0], sortLists=True)

        # No longer part of the telephone search view.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], view.errors),
                      until=lambda: len(view.contacts) == 0)

        # Remove the contact.
        self.manager.RemoveContact('', localID)
        self.runUntil('empty view',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: len(self.view.contacts) == 0)

    # TODO: check that deleting or modifying a contact works directly
    # after starting the PIM manager. The problem is that FolksPersonaStore
    # might still be loading the contacts, in which case looking up the
    # contact would fail.

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5")
    def testFilterStartup(self):
        '''TestContacts.testFilterStartup - phone number lookup while folks still loads'''
        self.setUpView(search=None)

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder())
        self.manager.SetSortOrder("first/last")

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/788899
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+49-89-788899
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])

        # Now start with a phone number search which must look
        # directly in EDS because the unified address book is not
        # ready (delayed via env variable).
        self.view.search([['phone', '089/788899']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Wait for final results from folks. The same in this case.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 1)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Nothing changed when folks became active.
        self.assertEqual([
                  ('added', 0, 2),
                  ('quiescent',),
                  ('quiescent',),
                  ],
                         self.view.events)

    def doFilterStartupRefine(self, simpleSearch=True):
        '''TestContacts.testFilterStartupRefine - phone number lookup while folks still loads, with folks finding more contacts (simple search in EDS) or the same contacts (intelligent search)'''
        self.setUpView(search=None)

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder())
        self.manager.SetSortOrder("first/last")

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/788899
EMAIL:az@example.com
END:VCARD''',

# Extra space, breaks suffix match in EDS.
# A more intelligent phone number search in EDS
# will find this again.
u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+49-89-7888 99
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact%d.vcf' % i)
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
        logging.log('inserting contacts')
        out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + self.uid, 'local'])

        # Now start with a phone number search which must look
        # directly in EDS because the unified address book is not
        # ready (delayed via env variable).
        self.view.search([['phone', '089/788899']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        if simpleSearch:
             self.assertEqual(1, len(self.view.contacts))
             self.view.read(0, 1)
        else:
             self.assertEqual(2, len(self.view.contacts))
             self.view.read(0, 2)

        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, simpleSearch and 1 or 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        if not simpleSearch:
             self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Wait for final results from folks. Also finds Benjamin.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 1)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        if simpleSearch:
             # One contact added by folks.
             self.assertEqual([
                       ('added', 0, 1),
                       ('quiescent',),
                       ('added', 1, 1),
                       ('quiescent',),
                       ],
                              self.view.events)
        else:
             # Two contacts added initially, not updated by folks.
             self.assertEqual([
                       ('added', 0, 2),
                       ('quiescent',),
                       ('quiescent',),
                       ],
                              self.view.events)


    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5 SYNCEVOLUTION_PIM_EDS_SUBSTRING=1")
    def testFilterStartupRefine(self):
        '''TestContacts.testFilterStartupRefine - phone number lookup while folks still loads, with folks finding more contacts because we use substring search in EDS'''
        self.doFilterStartupRefine()

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5")
    def testFilterStartupRefineSmart(self):
        '''TestContacts.testFilterStartupRefine - phone number lookup while folks still loads, with folks finding the same contacts because we use smart search in EDS'''
        # This test depends on libphonenumber support in EDS!
        self.doFilterStartupRefine(simpleSearch=False)

    @timeout(60)
    @property("ENV", "LC_TYPE=de_DE.UTF-8 LC_ALL=de_DE.UTF-8 LANG=de_DE.UTF-8 SYNCEVOLUTION_PIM_DELAY_FOLKS=5")
    def testFilterStartupMany(self):
        '''TestContacts.testFilterStartupMany - phone number lookup in many address books'''
        self.setUpView(search=None, peers=['0', '1', '2'])

        # Override default sorting.
        self.assertEqual("last/first", self.manager.GetSortOrder())
        self.manager.SetSortOrder("first/last")

        # Insert new contacts.
        #
        # The names are chosen so that sorting by first name and sorting by last name needs to
        # reverse the list.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
N:Zoo;Abraham
NICKNAME:Ace
TEL:1234
TEL:56/78
TEL:+1-800-FOOBAR
TEL:089/788899
EMAIL:az@example.com
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
N:Yeah;Benjamin
TEL:+49-89-788899
END:VCARD''',

u'''BEGIN:VCARD
VERSION:3.0
FN:Charly 'Chárleß' Xing
N:Xing;Charly
END:VCARD''']):
             item = os.path.join(self.contacts, 'contact.vcf')
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
             logging.printf('inserting contact %d', i)
             uid = self.uidPrefix + str(i)
             out, err, returncode = self.runCmdline(['--import', self.contacts, '@' + self.managerPrefix + uid, 'local'])

        # Now start with a phone number search which must look
        # directly in EDS because the unified address book is not
        # ready (delayed via env variable).
        self.view.search([['phone', '089/788899']])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Wait for final results from folks. The same in this case.
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 1)
        self.assertEqual(2, len(self.view.contacts))
        self.view.read(0, 2)
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.haveData(0, 2))
        self.assertEqual(u'Abraham', self.view.contacts[0]['structured-name']['given'])
        self.assertEqual(u'Benjamin', self.view.contacts[1]['structured-name']['given'])

        # Nothing changed when folks became active.
        self.assertEqual([
                  ('added', 0, 2),
                  ('quiescent',),
                  ('quiescent',),
                  ],
                         self.view.events)

    @timeout(60)
    def testDeadAgent(self):
        '''TestContacts.testDeadAgent - an error from the agent kills the view'''
        self.setUpView(search=None, peers=[], withSystemAddressBook=True)

        # Insert new contact.
        for i, contact in enumerate([u'''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD''',
]):
             item = os.path.join(self.contacts, 'contact.vcf')
             output = codecs.open(item, "w", "utf-8")
             output.write(contact)
             output.close()
             logging.printf('inserting contact %d', i)

        out, err, returncode = self.runCmdline(['--import', self.contacts, 'backend=evolution-contacts'])

        # Plug into processEvent() method so that it throws an error
        # when receiving the ContactsAdded method call. The same cannot be
        # done for Quiescent, because that call is optional and thus allowed
        # to fail.
        original = self.view.processEvent
        def intercept(message, event):
             if event[0] == 'quiescent':
                  # Sometimes the aggregator was seen as idle before
                  # it loaded the item above, leading to one
                  # additional 'quiescent' before 'added'. Not sure
                  # why. Anyway, that belongs into a different test,
                  # so ignore 'quiescent' here.
                  return
             # Record it.
             original(message, event)
             # Raise error?
             if event[0] == 'added':
                  logging.printf('raising "fake error" for event %s' % event)
                  raise Exception('fake error')
        self.view.processEvent = intercept
        self.view.search([])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.events)
        self.assertEqual([('added', 0, 1)],
                         self.view.events)

        # Expect an error, view should have been closed already.
        with self.assertRaisesRegexp(dbus.DBusException,
                                     "org.freedesktop.DBus.Error.UnknownMethod: .*"):
             self.view.close()

    @timeout(60)
    def testQuiescentOptional(self):
        '''TestContacts.testQuiescentOptional - the Quiescent() method is allowed to fail'''
        self.setUpView(search=None, peers=[], withSystemAddressBook=True)

        # Plug into "Quiescent" method so that it throws an error.
        original = self.view.processEvent
        def intercept(message, event):
             original(message, event)
             if event[0] == 'quiescent':
                  raise Exception('fake error')
        self.view.processEvent = intercept
        self.view.search([])
        self.runUntil('phone results',
                      check=lambda: self.assertEqual([], self.view.errors),
                      until=lambda: self.view.quiescentCount > 0)
        self.assertEqual([('quiescent',)],
                         self.view.events)
        self.view.close()

class TestSlowSync(TestPIMUtil, unittest.TestCase):
    """Test PIM Manager Sync"""

    def run(self, result):
         TestPIMUtil.run(self, result, serverArgs=['-d', '10'])

    @timeout(usingValgrind() and 600 or 60)
    @property("ENV", "SYNCEVOLUTION_SYNC_DELAY=20")
    def testSlowSync(self):
        '''TestSlowSync.testSlowSync - run a sync which takes longer than the 10 second inactivity duration'''

        # dummy peer directory
        contacts = os.path.abspath(os.path.join(xdg_root, 'contacts'))
        os.makedirs(contacts)

        # add foo
        peers = {}
        uid = self.uidPrefix + 'foo'
        peers[uid] = {'protocol': 'files',
                      'address': contacts}
        self.manager.SetPeer(uid,
                             peers[uid])
        self.manager.SyncPeer(uid)


if __name__ == '__main__':
    error = ''
    paths = [ (os.path.dirname(x), os.path.basename(x)) for x in \
              [ os.environ.get(y, '') for y in ['XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME'] ] ]
    xdg_root = paths[0][0]
    if not xdg_root or xdg_root != paths[1][0] or xdg_root != paths[2][0] or \
             paths[0][1] != 'config' or paths[1][1] != 'data' or  paths[2][1] != 'cache':
         # Don't allow user of the script to erase his normal EDS data and enforce
         # common basedir with well-known names for each xdg home.
         error = error + 'testpim.py must be started in a D-Bus session with XDG_CONFIG_HOME, XDG_DATA_HOME, XDG_CACHE_HOME set to temporary directories <foo>/config, <foo>/data, <foo>/cache because it will modify system EDS databases there and relies on a known, flat layout underneath a common directory.\n'
    else:
         # Tell test-dbus.py about the temporary directory that we expect
         # to use. It'll wipe it clean for us because we run with own_xdg=true.
         testdbus.xdg_root = xdg_root
    if os.environ.get('LANG', '') != 'de_DE.utf-8':
         error = error + 'EDS daemon must use the same LANG=de_DE.utf-8 as tests to get phone number normalization right.\n'
    if error:
         sys.exit(error)
    unittest.main()
