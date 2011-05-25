#! /usr/bin/python
#
# Copyright (C) 2009 Intel Corporation
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

import random
import unittest
import subprocess
import time
import os
import signal
import shutil
import copy
import heapq

import dbus
from dbus.mainloop.glib import DBusGMainLoop
import dbus.service
import gobject
import sys
import re

# introduced in python-gobject 2.16, not available
# on all Linux distros => make it optional
try:
    import glib
    have_glib = True
except ImportError:
    have_glib = False

DBusGMainLoop(set_as_default=True)

bus = dbus.SessionBus()
loop = gobject.MainLoop()

debugger = "" # "gdb"
server = ["syncevo-dbus-server"]
monitor = ["dbus-monitor"]
xdg_root = "test-dbus"
configName = "dbus_unittest"

def property(key, value):
    """Function decorator which sets an arbitrary property of a test.
    Use like this:
         @property("foo", "bar")
         def testMyTest:
             ...

             print self.getTestProperty("foo", "default")
    """
    def __setProperty(func):
        if not "properties" in dir(func):
            func.properties = {}
        func.properties[key] = value
        return func
    return __setProperty

def timeout(seconds):
    """Function decorator which sets a non-default timeout for a test.
    The default timeout, enforced by DBusTest.runTest(), are 5 seconds.
    Use like this:
        @timeout(10)
        def testMyTest:
            ...
    """
    return property("timeout", seconds)

class Timeout:
    """Implements global time-delayed callbacks."""
    alarms = []
    next_alarm = None
    previous_handler = None
    debugTimeout = False

    @classmethod
    def addTimeout(cls, delay_seconds, callback, use_glib=True):
        """Call function after a certain delay, specified in seconds.
        If possible and use_glib=True, then it will only fire inside
        glib event loop. Otherwise it uses signals. When signals are
        used it is a bit uncertain what kind of Python code can
        be executed. It was observed that trying to append to
        DBusUtil.quit_events before calling loop.quit() caused
        a KeyboardInterrupt"""
        if have_glib and use_glib:
            glib.timeout_add(delay_seconds, callback)
            # TODO: implement removal of glib timeouts
            return None
        else:
            now = time.time()
            if cls.debugTimeout:
                print "addTimeout", now, delay_seconds, callback, use_glib
            timeout = (now + delay_seconds, callback)
            heapq.heappush(cls.alarms, timeout)
            cls.__check_alarms()
            return timeout

    @classmethod
    def removeTimeout(cls, timeout):
        """Remove a timeout returned by a previous addTimeout call.
        None and timeouts which have already fired are acceptable."""
        try:
            cls.alarms.remove(timeout)
        except ValueError:
            pass
        else:
            heapq.heapify(cls.alarms)
            cls.__check_alarms()

    @classmethod
    def __handler(cls, signum, stack):
        """next_alarm has fired, check for expired timeouts and reinstall"""
        if cls.debugTimeout:
            print "fired", time.time()
        cls.next_alarm = None
        cls.__check_alarms()

    @classmethod
    def __check_alarms(cls):
        now = time.time()
        while cls.alarms and cls.alarms[0][0] <= now:
            timeout = heapq.heappop(cls.alarms)
            if cls.debugTimeout:
                print "invoking", timeout
            timeout[1]()

        if cls.alarms:
            if not cls.next_alarm or \
                    cls.next_alarm > cls.alarms[0][0]:
                if cls.previous_handler == None:
                    cls.previous_handler = signal.signal(signal.SIGALRM, cls.__handler)
                cls.next_alarm = cls.alarms[0][0]
                delay = int(cls.next_alarm - now + 0.5)
                if not delay:
                    delay = 1
                if cls.debugTimeout:
                    print "next alarm", cls.next_alarm, delay
                signal.alarm(delay)
        elif cls.next_alarm:
            if cls.debugTimeout:
                print "disarming alarm"
            signal.alarm(0)
            cls.next_alarm = None

# commented out because running it takes time
#class TestTimeout(unittest.TestCase):
class TimeoutTest:
    """unit test for Timeout mechanism"""

    def testOneTimeout(self):
        """simple timeout of two seconds"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(2, callback, use_glib=False)
        time.sleep(10)
        end = time.time()
        self.failUnless(self.called)
        self.failIf(end - start < 2)
        self.failIf(end - start >= 3)

    def testEmptyTimeout(self):
        """called immediately because of zero timeout"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(0, callback, use_glib=False)
        if not self.called:
            time.sleep(10)
        end = time.time()
        self.failUnless(self.called)
        self.failIf(end - start < 0)
        self.failIf(end - start >= 1)

    def testTwoTimeouts(self):
        """two timeouts after 2 and 5 seconds, installed in order"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(2, callback, use_glib=False)
        Timeout.addTimeout(5, callback, use_glib=False)
        time.sleep(10)
        end = time.time()
        self.failUnless(self.called)
        self.failIf(end - start < 2)
        self.failIf(end - start >= 3)
        time.sleep(10)
        end = time.time()
        self.failUnless(self.called)
        self.failIf(end - start < 5)
        self.failIf(end - start >= 6)

    def testTwoReversedTimeouts(self):
        """two timeouts after 2 and 5 seconds, installed in reversed order"""
        self.called = False
        start = time.time()
        def callback():
            self.called = True
        Timeout.addTimeout(5, callback, use_glib=False)
        Timeout.addTimeout(2, callback, use_glib=False)
        time.sleep(10)
        end = time.time()
        self.failUnless(self.called)
        self.failIf(end - start < 2)
        self.failIf(end - start >= 3)
        time.sleep(10)
        end = time.time()
        self.failUnless(self.called)
        self.failIf(end - start < 5)
        self.failIf(end - start >= 6)

class DBusUtil(Timeout):
    """Contains the common run() method for all D-Bus test suites
    and some utility functions."""

    # Use class variables because that way it is ensured that there is
    # only one set of them. Previously instance variables were used,
    # which had the effect that D-Bus signal handlers from test A
    # wrote into variables which weren't the ones used by test B.
    # Unfortunately it is impossible to remove handlers when
    # completing test A.
    events = []
    quit_events = []
    reply = None
    pserver = None

    def getTestProperty(self, key, default):
        """retrieve values set with @property()"""
        test = eval(self.id().replace("__main__.", ""))
        if "properties" in dir(test):
            return test.properties.get(key, default)
        else:
            return default

    def runTest(self, result, own_xdg=True, serverArgs=[] ):
        """Starts the D-Bus server and dbus-monitor before the test
        itself. After the test run, the output of these two commands
        are added to the test's failure, if any. Otherwise the output
        is ignored. A non-zero return code of the D-Bus server is
        logged as separate failure.

        The D-Bus server must print at least one line of output
        before the test is allowed to start.
        
        The commands are run with XDG_DATA_HOME, XDG_CONFIG_HOME,
        XDG_CACHE_HOME pointing towards local dirs
        test-dbus/[data|config|cache] which are removed before each
        test."""

        DBusUtil.events = []
        DBusUtil.quit_events = []
        DBusUtil.reply = None

        kill = subprocess.Popen("sh -c 'killall -9 syncevo-dbus-server dbus-monitor >/dev/null 2>&1'", shell=True)
        kill.communicate()

        """own_xdg is saved in self for we use this flag to check whether
        copying reference directory tree."""
        self.own_xdg = own_xdg
        env = copy.deepcopy(os.environ)
        if own_xdg:
            shutil.rmtree(xdg_root, True)
            env["XDG_DATA_HOME"] = xdg_root + "/data"
            env["XDG_CONFIG_HOME"] = xdg_root + "/config"
            env["XDG_CACHE_HOME"] = xdg_root + "/cache"

        # set additional environment variables for the test run,
        # as defined by @property("ENV", "foo=bar x=y")
        for assignment in self.getTestProperty("ENV", "").split():
            var, value = assignment.split("=")
            env[var] = value

        dbuslog = "dbus.log"
        syncevolog = "syncevo.log"
        pmonitor = subprocess.Popen(monitor,
                                    stdout=open(dbuslog, "w"),
                                    stderr=subprocess.STDOUT)
        
        if debugger:
            print "\n%s: %s\n" % (self.id(), self.shortDescription())
            DBusUtil.pserver = subprocess.Popen([debugger] + server,
                                                env=env)

            while True:
                check = subprocess.Popen("ps x | grep %s | grep -w -v -e %s -e grep -e ps" % \
                                             (server[0], debugger),
                                         env=env,
                                         shell=True,
                                         stdout=subprocess.PIPE)
                out, err = check.communicate()
                if out:
                    # process exists, but might still be loading,
                    # so give it some more time
                    time.sleep(2)
                    break
        else:
            DBusUtil.pserver = subprocess.Popen(server + serverArgs,
                                                env=env,
                                                stdout=open(syncevolog, "w"),
                                                stderr=subprocess.STDOUT)
            while os.path.getsize(syncevolog) == 0:
                time.sleep(1)

        numerrors = len(result.errors)
        numfailures = len(result.failures)
        if debugger:
            print "\nrunning\n"

        # Find out what test function we run and look into
        # the function definition to see whether it comes
        # with a non-default timeout, otherwise use a 5 second
        # timeout.
        timeout = self.getTestProperty("timeout", 5)
        timeout_handle = None
        if timeout and not debugger:
            def timedout():
                error = "%s timed out after %d seconds" % (self.id(), timeout)
                if Timeout.debugTimeout:
                    print error
                raise Exception(error)
            timeout_handle = self.addTimeout(timeout, timedout, use_glib=False)
        try:
            self.running = True
            unittest.TestCase.run(self, result)
        except KeyboardInterrupt:
            # somehow this happens when timedout() above raises the exception
            # while inside glib main loop
            result.errors.append((self,
                                  "interrupted by timeout or CTRL-C or Python signal handler problem"))
        self.running = False
        self.removeTimeout(timeout_handle)
        if debugger:
            print "\ndone, quit gdb now\n"
        hasfailed = numerrors + numfailures != len(result.errors) + len(result.failures)

        if not debugger and DBusUtil.pserver.poll() == None:
            os.kill(DBusUtil.pserver.pid, signal.SIGTERM)
        DBusUtil.pserver.communicate()
        serverout = open(syncevolog).read()
        if DBusUtil.pserver is not None and DBusUtil.pserver.returncode != -15:
            hasfailed = True
        if hasfailed:
            # give D-Bus time to settle down
            time.sleep(1)
        os.kill(pmonitor.pid, signal.SIGTERM)
        pmonitor.communicate()
        monitorout = open(dbuslog).read()
        report = "\n\nD-Bus traffic:\n%s\n\nserver output:\n%s\n" % \
            (monitorout, serverout)
        if DBusUtil.pserver.returncode and DBusUtil.pserver.returncode != -15:
            # create a new failure specifically for the server
            result.errors.append((self,
                                  "server terminated with error code %d%s" % (DBusUtil.pserver.returncode, report)))
        elif numerrors != len(result.errors):
            # append report to last error
            result.errors[-1] = (result.errors[-1][0], result.errors[-1][1] + report)
        elif numfailures != len(result.failures):
            # same for failure
            result.failures[-1] = (result.failures[-1][0], result.failures[-1][1] + report)

    def isServerRunning(self):
        """True while the syncevo-dbus-server executable is still running"""
        return DBusUtil.pserver and DBusUtil.pserver.poll() == None

    def serverExecutable(self):
        """returns full path of currently running syncevo-dbus-server binary"""
        self.failUnless(self.isServerRunning())
        maps = open("/proc/%d/maps" % DBusUtil.pserver.pid, "r")
        regex = re.compile(r'[0-9a-f]*-[0-9a-f]* r-xp [0-9a-f]* [^ ]* \d* *(.*)\n')
        for line in maps:
            match = regex.match(line)
            if match:
                return match.group(1)
        self.fail("no executable found")

    def setUpServer(self):
        self.server = dbus.Interface(bus.get_object('org.syncevolution',
                                                    '/org/syncevolution/Server'),
                                     'org.syncevolution.Server')

    def createSession(self, config, wait, flags=[]):
        """Return sessionpath and session object for session using 'config'.
        A signal handler calls loop.quit() when this session becomes ready.
        If wait=True, then this call blocks until the session is ready.
        """
        if flags:
            sessionpath = self.server.StartSessionWithFlags(config, flags)
        else:
            sessionpath = self.server.StartSession(config)

        def session_ready(object, ready):
            if self.running and ready and object == sessionpath:
                DBusUtil.quit_events.append("session " + object + " ready")
                loop.quit()

        signal = bus.add_signal_receiver(session_ready,
                                         'SessionChanged',
                                         'org.syncevolution.Server',
                                         'org.syncevolution',
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)
        session = dbus.Interface(bus.get_object('org.syncevolution',
                                                sessionpath),
                                 'org.syncevolution.Session')
        status, error, sources = session.GetStatus(utf8_strings=True)
        if wait and status == "queuing":
            # wait for signal
            loop.run()
            self.failUnlessEqual(DBusUtil.quit_events, ["session " + sessionpath + " ready"])
        elif DBusUtil.quit_events:
            # signal was processed inside D-Bus call?
            self.failUnlessEqual(DBusUtil.quit_events, ["session " + sessionpath + " ready"])
        if wait:
            # signal no longer needed, remove it because otherwise it
            # might record unexpected "session ready" events
            signal.remove()
        DBusUtil.quit_events = []
        return (sessionpath, session)

    def setUpSession(self, config):
        """stores ready session in self.sessionpath and self.session"""
        self.sessionpath, self.session = self.createSession(config, True)

    def progressChanged(self, *args):
        '''subclasses override this method to write specified callbacks for ProgressChanged signals
           It is called by progress signal receivers in setUpListeners'''
        pass

    def statusChanged(self, *args):
        '''subclasses override this method to write specified callbacks for StatusChanged signals
           It is called by status signal receivers in setUpListeners'''
        pass

    def setUpListeners(self, sessionpath):
        """records progress and status changes in DBusUtil.events and
        quits the main loop when the session is done"""

        def progress(*args):
            if self.running:
                DBusUtil.events.append(("progress", args))
                self.progressChanged(args)

        def status(*args):
            if self.running:
                DBusUtil.events.append(("status", args))
                self.statusChanged(args)
                if args[0] == "done":
                    if sessionpath:
                        DBusUtil.quit_events.append("session " + sessionpath + " done")
                    else:
                        DBusUtil.quit_events.append("session done")
                    loop.quit()

        bus.add_signal_receiver(progress,
                                'ProgressChanged',
                                'org.syncevolution.Session',
                                'org.syncevolution',
                                sessionpath,
                                byte_arrays=True, 
                                utf8_strings=True)
        bus.add_signal_receiver(status,
                                'StatusChanged',
                                'org.syncevolution.Session',
                                'org.syncevolution',
                                sessionpath,
                                byte_arrays=True, 
                                utf8_strings=True)

    def setUpConfigListeners(self):
        """records ConfigChanged signal and records it in DBusUtil.events, then quits the loop"""

        def config():
            if self.running:
                DBusUtil.events.append("ConfigChanged")
                loop.quit()

        bus.add_signal_receiver(config,
                                'ConfigChanged',
                                'org.syncevolution.Server',
                                'org.syncevolution',
                                byte_arrays=True, 
                                utf8_strings=True)

    def setUpConnectionListeners(self, conpath):
        """records connection signals (abort and reply), quits when
        getting an abort"""

        def abort():
            if self.running:
                DBusUtil.events.append(("abort",))
                DBusUtil.quit_events.append("connection " + conpath + " aborted")
                loop.quit()

        def reply(*args):
            if self.running:
                DBusUtil.reply = args
                if args[3]:
                    DBusUtil.quit_events.append("connection " + conpath + " got final reply")
                else:
                    DBusUtil.quit_events.append("connection " + conpath + " got reply")
                loop.quit()

        bus.add_signal_receiver(abort,
                                'Abort',
                                'org.syncevolution.Connection',
                                'org.syncevolution',
                                conpath,
                                byte_arrays=True, 
                                utf8_strings=True)
        bus.add_signal_receiver(reply,
                                'Reply',
                                'org.syncevolution.Connection',
                                'org.syncevolution',
                                conpath,
                                byte_arrays=True, 
                                utf8_strings=True)

    def setupFiles(self, snapshot):
        """ Copy reference directory trees from
        test/test-dbus/<snapshot> to own xdg_root (=./test-dbus). To
        be used only in tests which called runTest() with
        own_xdg=True."""
        self.failUnless(self.own_xdg)

        # Get the absolute path of the current python file.
        scriptpath = os.path.abspath(os.path.expanduser(os.path.expandvars(sys.argv[0])))

        # reference directory 'test-dbus' is in the same directory as the current python file
        sourcedir = os.path.join(os.path.dirname(scriptpath), 'test-dbus', snapshot)

        """ Directories in test/test-dbus are copied to xdg_root, but
        maybe with different names, mappings are:
         test/test-dbus/<snapshot>   ./test-dbus
            sync4j                       .sync4j
            data                         data
            config                       config
            cache                        cache   """
        pairs = { 'sync4j' : '.sync4j', 'config' : 'config', 'cache' : 'cache', 'data' : 'data'}
        for src, dest in pairs.items():
            destpath = os.path.join(xdg_root, dest)
            # make sure the dest directory does not exist, which is required by shutil.copytree
            shutil.rmtree(destpath, True)
            sourcepath = os.path.join(sourcedir, src)
            # if source exists and could be accessed, then copy them
            if os.access(sourcepath, os.F_OK):
                shutil.copytree(sourcepath, destpath)

    def getDatabaseName(self, configName):
        # get database names with the environment variable
        prefix = os.getenv("CLIENT_TEST_EVOLUTION_PREFIX")
        source = configName + '_1'
        if prefix == None:
            prefix = 'SyncEvolution_Test_'
        return prefix + source;

    def getDatabases(self, config):
        # get 'database' for each source
        updateProps = { }
        for key, value in config.items():
            if key != "":
                tmpdict = { }
                [source, sep, name] = key.partition('/')
                if sep == '/':
                    tmpdict["database"] = self.getDatabaseName(name)
                updateProps[key] = tmpdict
        return updateProps

    def checkSync(self, expectedError=0, expectedResult=0):
        # check recorded events in DBusUtil.events, first filter them
        statuses = []
        progresses = []
        # Dict is used to check status order.  
        statusPairs = {"": 0, "idle": 1, "running" : 2, "aborting" : 3, "done" : 4}
        for item in DBusUtil.events:
            if item[0] == "status":
                statuses.append(item[1])
            elif item[0] == "progress":
                progresses.append(item[1])

        # check statuses
        lastStatus = ""
        lastSources = {}
        lastError = 0
        for status, error, sources in statuses:
            # consecutive entries should not be equal
            self.failIfEqual((lastStatus, lastError, lastSources), (status, error, sources))
            # no error, unless expected
            if expectedError:
                if error:
                    self.failUnlessEqual(expectedError, error)
            else:
                self.failUnlessEqual(error, 0)
            # keep order: session status must be unchanged or the next status 
            seps = status.split(';')
            lastSeps = lastStatus.split(';')
            self.failUnless(statusPairs.has_key(seps[0]))
            self.failUnless(statusPairs[seps[0]] >= statusPairs[lastSeps[0]])
            # check specifiers
            if len(seps) > 1:
                self.failUnlessEqual(seps[1], "waiting")
            for sourcename, value in sources.items():
                # no error
                self.failUnlessEqual(value[2], 0)
                # keep order: source status must also be unchanged or the next status
                if lastSources.has_key(sourcename):
                    lastValue = lastSources[sourcename]
                    self.failUnless(statusPairs[value[1]] >= statusPairs[lastValue[1]])

            lastStatus = status
            lastSources = sources
            lastError = error

        # check increasing progress percentage
        lastPercent = 0
        for percent, sources in progresses:
            self.failIf(percent < lastPercent)
            lastPercent = percent

        status, error, sources = self.session.GetStatus(utf8_strings=True)
        self.failUnlessEqual(status, "done")
        self.failUnlessEqual(error, expectedError)

        # now check that report is sane
        reports = self.session.GetReports(0, 100, utf8_strings=True)
        self.failUnlessEqual(len(reports), 1)
        if expectedResult:
            self.failUnlessEqual(int(reports[0]["status"]), expectedResult)
        else:
            self.failUnlessEqual(int(reports[0]["status"]), 200)
            self.failIf("error" in reports[0])
        return reports[0]

class TestDBusServer(unittest.TestCase, DBusUtil):
    """Tests for the read-only Server API."""

    def setUp(self):
        self.setUpServer()

    def run(self, result):
        self.runTest(result)

    def testCapabilities(self):
        """check the Server.GetCapabilities() call"""
        capabilities = self.server.GetCapabilities()
        capabilities.sort()
        self.failUnlessEqual(capabilities, ['ConfigChanged', 'DatabaseProperties', 'GetConfigName', 'Notifications', 'SessionAttach', 'SessionFlags', 'Version'])

    def testVersions(self):
        """check the Server.GetVersions() call"""
        versions = self.server.GetVersions()
        self.failIfEqual(versions["version"], "")
        self.failIfEqual(versions["system"], None)
        self.failIfEqual(versions["backends"], None)

    def testGetConfigsEmpty(self):
        """GetConfigs() with no configurations available"""
        configs = self.server.GetConfigs(False, utf8_strings=True)
        self.failUnlessEqual(configs, [])

    def testGetConfigsTemplates(self):
        """read templates"""
        configs = self.server.GetConfigs(True, utf8_strings=True)
        configs.sort()
        self.failUnlessEqual(configs, ["Funambol",
                                       "Google",
                                       "Goosync",
                                       "Memotoo",
                                       "Mobical",
                                       "Oracle",
                                       "Ovi",
                                       "ScheduleWorld",
                                       "SyncEvolution",
                                       "Synthesis"])

    def testGetConfigScheduleWorld(self):
        """read ScheduleWorld template"""
        config1 = self.server.GetConfig("scheduleworld", True, utf8_strings=True)
        config2 = self.server.GetConfig("ScheduleWorld", True, utf8_strings=True)
        self.failIfEqual(config1[""]["deviceId"], config2[""]["deviceId"])
        config1[""]["deviceId"] = "foo"
        config2[""]["deviceId"] = "foo"
        self.failUnlessEqual(config1, config2)

    def testInvalidConfig(self):
        """check that the right error is reported for invalid config name"""
        try:
            config1 = self.server.GetConfig("no-such-config", False, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchConfig: No configuration 'no-such-config' found")
        else:
            self.fail("no exception thrown")

class TestDBusServerTerm(unittest.TestCase, DBusUtil):
    def setUp(self):
        self.setUpServer()

    def run(self, result):
        self.runTest(result, True, ["-d", "10"])

    @timeout(100)
    def testNoTerm(self):
        """Test the dbus server stays alive within the duration"""
        """The server should stay alive because we have dbus call within
        the duration. The loop is to make sure the total time is longer 
        than duration and the dbus server still stays alive for dbus calls.""" 
        for i in range(0, 4):
            time.sleep(4)
            try:
                self.server.GetConfigs(True, utf8_strings=True)
            except dbus.DBusException:
                self.fail("dbus server should work correctly")

    @timeout(100)
    def testTerm(self):
        """Test the dbus server terminates automatically after a duration"""
        #sleep a duration and wait for syncevo-dbus-server termination
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

    @timeout(100)
    def testTermConnection(self):
        """Test the dbus server doesn't terminate if it has connections"""
        conpath = self.server.Connect({'description': 'test-dbus.py',
                                       'transport': 'dummy'},
                                      False,
                                      "")
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            self.fail("dbus server should not terminate")

        connection = dbus.Interface(bus.get_object('org.syncevolution',
                                                   conpath),
                                    'org.syncevolution.Connection')
        connection.Close(False, "good bye", utf8_strings=True)
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

    @timeout(100)
    def testTermAttachedClients(self):
        """Test the dbus server doesn't terminate if it has attached clients"""
        """Also it tries to test the dbus server's behavior when a client 
        attaches the server many times"""
        self.server.Attach()
        self.server.Attach()
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            self.fail("dbus server should not terminate")
        self.server.Detach()
        time.sleep(16)

        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            self.fail("dbus server should not terminate")

        self.server.Detach()
        time.sleep(16)
        try:
            self.server.GetConfigs(True, utf8_strings=True)
        except dbus.DBusException:
            pass
        else:
            self.fail("no exception thrown")

class Connman (dbus.service.Object):
    count = 0
    @dbus.service.method(dbus_interface='net.connman.Manager', in_signature='', out_signature='a{sv}')
    def GetProperties(self):
        self.count = self.count+1
        if (self.count == 1):
            loop.quit()
            return {"ConnectedTechnologies":["ethernet", "some other stuff"],
                    "AvailableTechnologies": ["bluetooth"]}
        elif (self.count == 2):
            """ unplug the ethernet cable """
            loop.quit()
            return {"ConnectedTechnologies":["some other stuff"],
                    "AvailableTechnologies": ["bluetooth"]}
        elif (self.count == 3):
            """ replug the ethernet cable """
            loop.quit()
            return {"ConnectedTechnologies":["ethernet", "some other stuff"],
                    "AvailableTechnologies": ["bluetooth"]}
        elif (self.count == 4):
            """ nothing presence """
            loop.quit()
            return {"ConnectedTechnologies":[""],
                    "AvailableTechnologies": [""]}
        elif (self.count == 5):
            """ come back the same time """
            loop.quit()
            return {"ConnectedTechnologies":["ethernet", "some other stuff"],
                    "AvailableTechnologies": ["bluetooth"]}

    @dbus.service.signal(dbus_interface='net.connman.Manager', signature='sv')
    def PropertyChanged(self, key, value):
        pass

    def emitSignal(self):
        self.count = self.count+1
        if (self.count == 2):
            """ unplug the ethernet cable """
            self.PropertyChanged("ConnectedTechnologies",["some other stuff"])
            return
        elif (self.count == 3):
            """ replug the ethernet cable """
            self.PropertyChanged("ConnectedTechnologies", ["ethernet", "some other stuff"])
            return
        elif (self.count == 4):
            """ nothing presence """
            self.PropertyChanged("ConnectedTechnologies", [""])
            self.PropertyChanged("AvailableTechnologies", [""])
            return
        elif (self.count == 5):
            """ come back the same time """
            self.PropertyChanged("ConnectedTechnologies", ["ethernet", "some other stuff"])
            self.PropertyChanged("AvailableTechnologies", ["bluetooth"])
            return

    def reset(self):
        self.count = 0

class TestDBusServerPresence(unittest.TestCase, DBusUtil):
    """Tests Presence signal and checkPresence API"""
    name = dbus.service.BusName ("net.connman", bus);
    conn = Connman (bus, "/")

    def setUp(self):
        self.setUpServer()

    @timeout(100)
    def testPresenceSignal(self):
        self.conn.reset()
        self.setUpSession("foo")
        self.session.SetConfig(False, False, {"" : {"syncURL":
        "http://http-only-1"}})
        self.session.Detach()
        def cb_http_presence(server, status, transport):
            self.failUnlessEqual (status, "")
            self.failUnlessEqual (server, "foo")
            self.failUnlessEqual (transport, "http://http-only-1")
            loop.quit()

        match = bus.add_signal_receiver(cb_http_presence,
                                'Presence',
                                'org.syncevolution.Server',
                                'org.syncevolution',
                                None,
                                byte_arrays=True,
                                utf8_strings=True)
        loop.run()
        time.sleep(1)
        self.setUpSession("foo")
        self.session.SetConfig(True, False, {"" : {"syncURL":
        "obex-bt://temp-bluetooth-peer-changed-from-http"}})
        def cb_bt_presence(server, status, transport):
            self.failUnlessEqual (status, "")
            self.failUnlessEqual (server, "foo")
            self.failUnlessEqual (transport,
                    "obex-bt://temp-bluetooth-peer-changed-from-http")
            loop.quit()
        match.remove()
        match = bus.add_signal_receiver(cb_bt_presence,
                                'Presence',
                                'org.syncevolution.Server',
                                'org.syncevolution',
                                None,
                                byte_arrays=True,
                                utf8_strings=True)
        self.conn.emitSignal()
        loop.run()
        time.sleep(1)
        self.session.Detach()
        self.setUpSession("bar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "http://http-client-2"}})
        self.session.Detach()
        self.foo = "random string"
        self.bar = "random string"
        match.remove()
        self.conn.emitSignal()
        self.conn.emitSignal()
        def cb_bt_http_presence(server, status, transport):
            if (server == "foo"):
                self.foo = status
            elif (server == "bar"):
                self.bar = status
            else:
                self.fail("wrong server config")
            loop.quit()

        match = bus.add_signal_receiver(cb_bt_http_presence,
                                'Presence',
                                'org.syncevolution.Server',
                                'org.syncevolution',
                                None,
                                byte_arrays=True,
                                utf8_strings=True)
        #count=5, 2 signals recevied
        self.conn.emitSignal()
        loop.run()
        loop.run()
        time.sleep(1)
        self.failUnlessEqual (self.foo, "")
        self.failUnlessEqual (self.bar, "")
        match.remove()

    @timeout(100)
    def testServerCheckPresence(self):
        self.conn.reset()
        self.setUpSession("foo")
        self.session.SetConfig(False, False, {"" : {"syncURL":
        "http://http-client"}})
        self.session.Detach()
        self.setUpSession("bar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "obex-bt://bt-client"}})
        self.session.Detach()
        self.setUpSession("foobar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "obex-bt://bt-client-mixed http://http-client-mixed"}})
        self.session.Detach()

        #let dbus server get the first presence
        loop.run()
        time.sleep(1)
        (status, transports) = self.server.CheckPresence ("foo")
        self.failUnlessEqual (status, "")
        self.failUnlessEqual (transports, ["http://http-client"])
        (status, transports) = self.server.CheckPresence ("bar")
        self.failUnlessEqual (status, "")
        self.failUnlessEqual (transports, ["obex-bt://bt-client"])
        (status, transports) = self.server.CheckPresence ("foobar")
        self.failUnlessEqual (status, "")
        self.failUnlessEqual (transports, ["obex-bt://bt-client-mixed",
        "http://http-client-mixed"])

        #count = 2
        self.conn.emitSignal()
        time.sleep(1)
        (status, transports) = self.server.CheckPresence ("foo")
        self.failUnlessEqual (status, "no transport")
        (status, transports) = self.server.CheckPresence ("bar")
        self.failUnlessEqual (status, "")
        self.failUnlessEqual (transports, ["obex-bt://bt-client"])
        (status, transports) = self.server.CheckPresence ("foobar")
        self.failUnlessEqual (status, "")
        self.failUnlessEqual (transports, ["obex-bt://bt-client-mixed"])

    @timeout(100)
    def testSessionCheckPresence(self):
        self.conn.reset()
        self.setUpSession("foobar")
        self.session.SetConfig(False, False, {"" : {"syncURL":
            "obex-bt://bt-client-mixed http://http-client-mixed"}})
        loop.run()
        time.sleep(1)
        status = self.session.checkPresence()
        self.failUnlessEqual (status, "")
        self.conn.emitSignal()
        self.conn.emitSignal()
        self.conn.emitSignal()
        #count = 4
        time.sleep(1)
        status = self.session.checkPresence()
        self.failUnlessEqual (status, "no transport")

    def run(self, result):
        os.environ["DBUS_TEST_CONNMAN"] = "session"
        self.runTest(result, True)

class TestDBusSession(unittest.TestCase, DBusUtil):
    """Tests that work with an active session."""

    def setUp(self):
        self.setUpServer()
        self.setUpSession("")

    def run(self, result):
        self.runTest(result)

    def testCreateSession(self):
        """ask for session"""
        self.failUnlessEqual(self.session.GetFlags(), [])
        self.failUnlessEqual(self.session.GetConfigName(), "@default");

    def testAttachSession(self):
        """attach to running session"""
        self.session.Attach()
        self.session.Detach()
        self.failUnlessEqual(self.session.GetFlags(), [])
        self.failUnlessEqual(self.session.GetConfigName(), "@default");

    @timeout(70)
    def testAttachOldSession(self):
        """attach to session which no longer has clients"""
        self.session.Detach()
        time.sleep(5)
        # This used to be impossible with SyncEvolution 1.0 because it
        # removed the session right after the previous client
        # left. SyncEvolution 1.1 makes it possible by keeping
        # sessions around for a minute. However, the session is
        # no longer listed because it really should only be used
        # by clients which heard about it before.
        self.failUnlessEqual(self.server.GetSessions(), [])
        self.session.Attach()
        self.failUnlessEqual(self.session.GetFlags(), [])
        self.failUnlessEqual(self.session.GetConfigName(), "@default");
        time.sleep(60)
        self.failUnlessEqual(self.session.GetFlags(), [])        

    @timeout(70)
    def testExpireSession(self):
        """ensure that session stays around for a minute"""
        self.session.Detach()
        time.sleep(5)
        self.failUnlessEqual(self.session.GetFlags(), [])
        self.failUnlessEqual(self.session.GetConfigName(), "@default");
        time.sleep(60)
        try:
            self.session.GetFlags()
        except:
            pass
        else:
            self.fail("Session.GetFlags() should have failed")

    def testCreateSessionWithFlags(self):
        """ask for session with some specific flags and config"""
        self.session.Detach()
        self.sessionpath, self.session = self.createSession("FooBar@no-such-context", True, ["foo", "bar"])
        self.failUnlessEqual(self.session.GetFlags(), ["foo", "bar"])
        self.failUnlessEqual(self.session.GetConfigName(), "foobar@no-such-context");

    @timeout(20)
    def testSecondSession(self):
        """a second session should not run unless the first one stops"""
        sessions = self.server.GetSessions()
        self.failUnlessEqual(sessions, [self.sessionpath])
        sessionpath = self.server.StartSession("")
        sessions = self.server.GetSessions()
        self.failUnlessEqual(sessions, [self.sessionpath, sessionpath])

        def session_ready(object, ready):
            if self.running:
                DBusUtil.quit_events.append("session " + object + (ready and " ready" or " done"))
                loop.quit()

        bus.add_signal_receiver(session_ready,
                                'SessionChanged',
                                'org.syncevolution.Server',
                                'org.syncevolution',
                                None,
                                byte_arrays=True,
                                utf8_strings=True)

        def status(*args):
            if self.running:
                DBusUtil.events.append(("status", args))
                if args[0] == "idle":
                    DBusUtil.quit_events.append("session " + sessionpath + " idle")
                    loop.quit()

        bus.add_signal_receiver(status,
                                'StatusChanged',
                                'org.syncevolution.Session',
                                'org.syncevolution',
                                sessionpath,
                                byte_arrays=True, 
                                utf8_strings=True)

        session = dbus.Interface(bus.get_object('org.syncevolution',
                                                sessionpath),
                                 'org.syncevolution.Session')
        status, error, sources = session.GetStatus(utf8_strings=True)
        self.failUnlessEqual(status, "queueing")
        # use hash so that we can write into it in callback()
        callback_called = {}
        def callback():
            callback_called[1] = "callback()"
            self.session.Detach()
        try:
            t1 = self.addTimeout(2, callback)
            # session 1 done
            loop.run()
            self.failUnless(callback_called)
            # session 2 ready and idle
            loop.run()
            loop.run()
            expected = ["session " + self.sessionpath + " done",
                        "session " + sessionpath + " idle",
                        "session " + sessionpath + " ready"]
            expected.sort()
            DBusUtil.quit_events.sort()
            self.failUnlessEqual(DBusUtil.quit_events, expected)
            status, error, sources = session.GetStatus(utf8_strings=True)
            self.failUnlessEqual(status, "idle")
        finally:
            self.removeTimeout(t1)

class TestSessionAPIsEmptyName(unittest.TestCase, DBusUtil):
    """Test session APIs that work with an empty server name. Thus, all of session APIs which
       need this kind of checking are put in this class. """

    def setUp(self):
        self.setUpServer()
        self.setUpSession("")

    def run(self, result):
        self.runTest(result)

    def testGetConfigEmptyName(self):
        """reading empty default config"""
        config = self.session.GetConfig(False, utf8_strings=True)

    def testGetTemplateEmptyName(self):
        """trigger error by getting template for empty server name"""
        try:
            config = self.session.GetConfig(True, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchConfig: No template '' found")
        else:
            self.fail("no exception thrown")

    def testCheckSourceEmptyName(self):
        """Test the error is reported when the server name is empty for CheckSource"""
        try:
            self.session.CheckSource("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: '' has no '' source")
        else:
            self.fail("no exception thrown")

    def testGetDatabasesEmptyName(self):
        """Test the error is reported when the server name is empty for GetDatabases"""
        try:
            self.session.GetDatabases("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: '' has no '' source")
        else:
            self.fail("no exception thrown")

    def testGetReportsEmptyName(self):
        """Test reports from all peers are returned in order when the peer name is empty for GetReports"""
        self.setupFiles('reports')
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.failUnlessEqual(len(reports), 7)
        refPeers = ["dummy-test", "dummy", "dummy-test", "dummy-test",
                    "dummy-test", "dummy_test", "dummy-test"]
        for i in range(0, len(refPeers)):
            self.failUnlessEqual(reports[i]["peer"], refPeers[i])

    def testGetReportsContext(self):
        """Test reports from a context are returned when the peer name is empty for GetReports"""
        self.setupFiles('reports')
        self.session.Detach()
        self.setUpSession("@context")
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.failUnlessEqual(len(reports), 1)
        self.failUnless(reports[0]["dir"].endswith("dummy_+test@context-2010-01-20-10-10"))


class TestSessionAPIsDummy(unittest.TestCase, DBusUtil):
    """Tests that work for GetConfig/SetConfig/CheckSource/GetDatabases/GetReports in Session.
       This class is only working in a dummy config. Thus it can't do sync correctly. The purpose
       is to test some cleanup cases and expected errors. Also, some unit tests for some APIs 
       depend on a clean configuration so they are included here. For those unit tests depending
       on sync, another class is used """

    def setUp(self):
        self.setUpServer()
        # use 'dummy-test' as the server name
        self.setUpSession("dummy-test")
        # default config
        self.config = { 
                         "" : { "syncURL" : "http://impossible-syncurl-just-for-testing-to-avoid-conflict",
                                "username" : "unknown",
                                "password" : "-",
                                "deviceId" : "foo",
                                "RetryInterval" : "10",
                                "RetryDuration" : "20",
                                "ConsumerReady" : "1",
                                "configName" : "dummy-test"
                              },
                         "source/addressbook" : { "sync" : "slow",
                                                  "backend" : "addressbook",
                                                  "uri" : "card"
                                                },
                         "source/calendar"    : { "sync" : "disabled",
                                                  "backend" : "calendar",
                                                  "uri" : "cal"
                                                },
                         "source/todo"        : { "sync" : "disabled",
                                                  "backend" : "todo",
                                                  "uri" : "task"
                                                },
                         "source/memo"        : { "sync" : "disabled",
                                                  "backend" : "memo",
                                                  "uri" : "text"
                                                }
                       }
        # update config
        self.updateConfig = { 
                               "" : { "username" : "doe"},
                               "source/addressbook" : { "sync" : "slow"}
                            }
        self.sources = ['addressbook', 'calendar', 'todo', 'memo']

        #create default or user settings database
        updateProps = self.getDatabases(self.config)
        for key, dict in updateProps.items():
            self.config[key]["database"] = dict["database"]

    def run(self, result):
        self.runTest(result)

    def clearAllConfig(self):
        """ clear a server config. All should be removed. Used internally. """
        emptyConfig = {}
        self.session.SetConfig(False, False, emptyConfig, utf8_strings=True)

    def setupConfig(self):
        """ create a server with full config. Used internally. """
        self.session.SetConfig(False, False, self.config, utf8_strings=True)

    def testTemporaryConfig(self):
        """various temporary config changes"""
        ref = { "": { "loglevel": "2", "configName": "dummy-test" } }
        config = copy.deepcopy(ref)
        self.session.SetConfig(False, False, config, utf8_strings=True)
        # reset
        self.session.SetConfig(False, True, {}, utf8_strings=True)
        self.failUnlessEqual(config, self.session.GetConfig(False, utf8_strings=True))
        # add sync prop
        self.session.SetConfig(True, True, { "": { "loglevel": "100" } }, utf8_strings=True)
        config[""]["loglevel"] = "100"
        self.failUnlessEqual(config, self.session.GetConfig(False, utf8_strings=True))
        # add source
        self.session.SetConfig(True, True, { "source/foobar": { "sync": "two-way" } }, utf8_strings=True)
        config["source/foobar"] = { "sync": "two-way" }
        self.session.SetConfig(True, True, { "": { "loglevel": "100" } }, utf8_strings=True)
        # add source prop
        self.session.SetConfig(True, True, { "source/foobar": { "database": "xyz" } }, utf8_strings=True)
        config["source/foobar"]["database"] = "xyz"
        self.failUnlessEqual(config, self.session.GetConfig(False, utf8_strings=True))
        # reset temporary settings
        self.session.SetConfig(False, True, { }, utf8_strings=True)
        config = copy.deepcopy(ref)
        self.failUnlessEqual(config, self.session.GetConfig(False, utf8_strings=True))        

    @timeout(20)
    def testCreateGetConfig(self):
        """ test the config is created successfully. """
        self.setUpConfigListeners()
        self.config[""]["username"] = "creategetconfig"
        self.config[""]["password"] = "112233445566778"
        self.setupConfig()
        """ get config and compare """
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config, self.config)
        # terminate session and check whether a "config changed" signal
        # was sent as required
        self.session.Detach()
        loop.run()
        self.failUnlessEqual(DBusUtil.events, ["ConfigChanged"])

    def testUpdateConfig(self):
        """ test the config is permenantly updated correctly. """
        self.setupConfig()
        """ update the given config """
        self.session.SetConfig(True, False, self.updateConfig, utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["username"], "doe")
        self.failUnlessEqual(config["source/addressbook"]["sync"], "slow")

    def testUpdateConfigTemp(self):
        """ test the config is just temporary updated but no effect in storage. """
        self.setupConfig()
        """ set config temporary """
        self.session.SetConfig(True, True, self.updateConfig, utf8_strings=True)
        self.session.Detach()
        """ creat a new session to lose the temporary configs """
        self.setUpSession("dummy-test")
        config = self.session.GetConfig(False, utf8_strings=True)
        """ no change of any properties """
        self.failUnlessEqual(config, self.config)

    def testGetConfigUpdateConfigTemp(self):
        """ test the config is temporary updated and in effect for GetConfig in the current session. """
        self.setupConfig()
        """ set config temporary """
        self.session.SetConfig(True, True, self.updateConfig, utf8_strings=True)
        """ GetConfig is affected """
        config = self.session.GetConfig(False, utf8_strings=True)
        """ no change of any properties """
        self.failUnlessEqual(config[""]["username"], "doe")
        self.failUnlessEqual(config["source/addressbook"]["sync"], "slow")

    def testGetConfigWithTempConfig(self):
        """ test the config is gotten for a new temporary config. """
        """ The given config doesn't exist on disk and it's set temporarily. Then GetConfig should
            return the configs temporarily set. """
        self.session.SetConfig(True, True, self.config, utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config, self.config)

    def testUpdateConfigError(self):
        """ test the right error is reported when an invalid property value is set """
        self.setupConfig()
        config = { 
                     "source/addressbook" : { "sync" : "invalid-value"}
                  }
        try:
            self.session.SetConfig(True, False, config, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.InvalidCall: invalid value 'invalid-value' for "
                                 "property 'sync': 'not one of the valid values (two-way, slow, "
                                 "refresh-from-client = refresh-client, refresh-from-server = "
                                 "refresh-server = refresh, one-way-from-client = one-way-client, "
                                 "one-way-from-server = one-way-server = one-way, disabled = none)'")
        else:
            self.fail("no exception thrown")

    def testUpdateNoConfig(self):
        """ test the right error is reported when updating properties for a non-existing server """
        try:
            self.session.SetConfig(True, False, self.updateConfig, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchConfig: The configuration 'dummy-test' doesn't exist")
        else:
            self.fail("no exception thrown")

    def testUnknownConfigContent(self):
        """config with unkown must be rejected"""
        self.setupConfig()

        try:
            config1 = copy.deepcopy(self.config)
            config1[""]["no-such-sync-property"] = "foo"
            self.session.SetConfig(False, False, config1, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.InvalidCall: unknown property 'no-such-sync-property'")
        else:
            self.fail("no exception thrown")

        try:
            config1 = copy.deepcopy(self.config)
            config1["source/addressbook"]["no-such-source-property"] = "foo"
            self.session.SetConfig(False, False, config1, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.InvalidCall: unknown property 'no-such-source-property'")
        else:
            self.fail("no exception thrown")

        try:
            config1 = copy.deepcopy(self.config)
            config1["no-such-key"] = { "foo": "bar" }
            self.session.SetConfig(False, False, config1, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.InvalidCall: invalid config entry 'no-such-key'")
        else:
            self.fail("no exception thrown")

    def testClearAllConfig(self):
        """ test all configs of a server are cleared correctly. """
        """ first set up config and then clear all configs and also check a non-existing config """
        self.setupConfig()
        self.clearAllConfig()
        try:
            config = self.session.GetConfig(False, utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                "org.syncevolution.NoSuchConfig: No configuration 'dummy-test' found")
        else:
            self.fail("no exception thrown")

    def testCheckSourceNoConfig(self):
        """ test the right error is reported when the server doesn't exist """
        try:
            self.session.CheckSource("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' has no '' source")
        else:
            self.fail("no exception thrown")

    def testCheckSourceNoSourceName(self):
        """ test the right error is reported when the source doesn't exist """
        self.setupConfig()
        try:
            self.session.CheckSource("dummy", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' "
                                 "has no 'dummy' source")
        else:
            self.fail("no exception thrown")

    def testCheckSourceInvalidEvolutionSource(self):
        """ test the right error is reported when the evolutionsource is invalid """
        self.setupConfig()
        config = { "source/memo" : { "database" : "impossible-source"} }
        self.session.SetConfig(True, False, config, utf8_strings=True)
        try:
            self.session.CheckSource("memo", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.SourceUnusable: The source 'memo' is not usable")
        else:
            self.fail("no exception thrown")

    def testCheckSourceInvalidType(self):
        """ test the right error is reported when the type is invalid """
        self.setupConfig()
        # we cannot set an invalid type here, so pick one which is likely
        # to be not supported in syncevo-dbus-server
        config = { "source/memo" : { "backend" : "apple-contacts"} }
        self.session.SetConfig(True, False, config, utf8_strings=True)
        try:
            self.session.CheckSource("memo", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.SourceUnusable: The source 'memo' is not usable")
        else:
            self.fail("no exception thrown")

    def testCheckSourceNoType(self):
        """ test the right error is reported when the source is unusable"""
        self.setupConfig()
        config = { "source/memo" : { "backend" : "file",
                                     "databaseFormat" : "text/calendar",
                                     "database" : "file:///no/such/path" } }
        self.session.SetConfig(True, False, config, utf8_strings=True)
        try:
            self.session.CheckSource("memo", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.SourceUnusable: The source 'memo' is not usable")
        else:
            self.fail("no exception thrown")

    def testCheckSource(self):
        """ test all are right """
        self.setupConfig()
        for source in self.sources:
            self.session.CheckSource(source, utf8_strings=True)

    def testCheckSourceUpdateConfigTemp(self):
        """ test the config is temporary updated and in effect for GetDatabases in the current session. """
        self.setupConfig()
        tempConfig = {"source/temp" : { "backend" : "calendar"}}
        self.session.SetConfig(True, True, tempConfig, utf8_strings=True)
        databases2 = self.session.CheckSource("temp", utf8_strings=True)

    def testGetDatabasesNoConfig(self):
        """ test the right error is reported when the server doesn't exist """
        # make sure the config doesn't exist """
        try:
            self.session.GetDatabases("", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' has no '' source")
        else:
            self.fail("no exception thrown")

    def testGetDatabasesEmpty(self):
        """ test the right error is reported for non-existing source"""
        self.setupConfig()
        try:
            databases = self.session.GetDatabases("never_use_this_source_name", utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.NoSuchSource: 'dummy-test' has no 'never_use_this_source_name' source")
        else:
            self.fail("no exception thrown")

    def testGetDatabases(self):
        """ test the right way to get databases """
        self.setupConfig()

        # don't know actual databases, so compare results of two different times
        sources = ['addressbook', 'calendar', 'todo', 'memo']
        databases1 = []
        for source in sources:
            databases1.append(self.session.GetDatabases(source, utf8_strings=True))
        # reverse the list of sources and get databases again
        sources.reverse()
        databases2 = []
        for source in sources:
            databases2.append(self.session.GetDatabases(source, utf8_strings=True))
        # sort two arrays
        databases1.sort()
        databases2.sort()
        self.failUnlessEqual(databases1, databases2)

    def testGetDatabasesUpdateConfigTemp(self):
        """ test the config is temporary updated and in effect for GetDatabases in the current session. """
        self.setupConfig()
        databases1 = self.session.GetDatabases("calendar", utf8_strings=True)
        tempConfig = {"source/temp" : { "backend" : "calendar"}}
        self.session.SetConfig(True, True, tempConfig, utf8_strings=True)
        databases2 = self.session.GetDatabases("temp", utf8_strings=True)
        self.failUnlessEqual(databases2, databases1)

    def testGetReportsNoConfig(self):
        """ Test nothing is gotten when the given server doesn't exist. Also covers boundaries """
        reports = self.session.GetReports(0, 0, utf8_strings=True)
        self.failUnlessEqual(reports, [])
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        self.failUnlessEqual(reports, [])
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.failUnlessEqual(reports, [])
        reports = self.session.GetReports(0xFFFFFFFF, 0xFFFFFFFF, utf8_strings=True)
        self.failUnlessEqual(reports, [])

    def testGetReportsNoReports(self):
        """ Test when the given server has no reports. Also covers boundaries """
        self.setupConfig()
        reports = self.session.GetReports(0, 0, utf8_strings=True)
        self.failUnlessEqual(reports, [])
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        self.failUnlessEqual(reports, [])
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.failUnlessEqual(reports, [])
        reports = self.session.GetReports(0xFFFFFFFF, 0xFFFFFFFF, utf8_strings=True)
        self.failUnlessEqual(reports, [])

    def testGetReportsByRef(self):
        """ Test the reports are gotten correctly from reference files. Also covers boundaries """
        """ This could be extractly compared since the reference files are known """
        self.setupFiles('reports')
        report0 = { "peer" : "dummy-test",
                    "start" : "1258520955",
                    "end" : "1258520964",
                    "status" : "200",
                    "source-addressbook-mode" : "slow",
                    "source-addressbook-first" : "true",
                    "source-addressbook-resume" : "false",
                    "source-addressbook-status" : "0",
                    "source-addressbook-backup-before" : "0",
                    "source-addressbook-backup-after" : "0",
                    "source-addressbook-stat-local-any-sent" : "9168",
                    "source-addressbook-stat-remote-added-total" : "71",
                    "source-addressbook-stat-remote-updated-total" : "100",
                    "source-addressbook-stat-local-updated-total" : "632",
                    "source-addressbook-stat-remote-any-reject" : "100",
                    "source-addressbook-stat-remote-any-conflict_duplicated" : "5293487",
                    "source-addressbook-stat-remote-any-conflict_client_won" : "33",
                    "source-addressbook-stat-local-any-received" : "2",
                    "source-addressbook-stat-local-removed-total" : "4",
                    "source-addressbook-stat-remote-any-conflict_server_won" : "38",
                    "source-addressbook-stat-local-any-reject" : "77",
                    "source-addressbook-stat-local-added-total" : "84",
                    "source-addressbook-stat-remote-removed-total" : "66",
                    "source-calendar-mode" : "slow",
                    "source-calendar-first" : "true",
                    "source-calendar-resume" : "false",
                    "source-calendar-status" : "0",
                    "source-calendar-backup-before" : "17",
                    "source-calendar-backup-after" : "17",
                    "source-calendar-stat-local-any-sent" : "8619",
                    "source-calendar-stat-remote-added-total": "17",
                    "source-calendar-stat-remote-updated-total" : "10",
                    "source-calendar-stat-local-updated-total" : "6",
                    "source-calendar-stat-remote-any-reject" : "1",
                    "source-calendar-stat-remote-any-conflict_duplicated" : "5",
                    "source-calendar-stat-remote-any-conflict_client_won" : "3",
                    "source-calendar-stat-local-any-received" : "24",
                    "source-calendar-stat-local-removed-total" : "54",
                    "source-calendar-stat-remote-any-conflict_server_won" : "38",
                    "source-calendar-stat-local-any-reject" : "7",
                    "source-calendar-stat-local-added-total" : "42",
                    "source-calendar-stat-remote-removed-total" : "6",
                    "source-memo-mode" : "slow",
                    "source-memo-first" : "true",
                    "source-memo-resume" : "false",
                    "source-memo-status" : "0",
                    "source-memo-backup-before" : "3",
                    "source-memo-backup-after" : "4",
                    "source-memo-stat-local-any-sent" : "8123",
                    "source-memo-stat-remote-added-total" : "15",
                    "source-memo-stat-remote-updated-total" : "6",
                    "source-memo-stat-local-updated-total" : "8",
                    "source-memo-stat-remote-any-reject" : "16",
                    "source-memo-stat-remote-any-conflict_duplicated" : "27",
                    "source-memo-stat-remote-any-conflict_client_won" : "2",
                    "source-memo-stat-local-any-received" : "3",
                    "source-memo-stat-local-removed-total" : "4",
                    "source-memo-stat-remote-any-conflict_server_won" : "8",
                    "source-memo-stat-local-any-reject" : "40",
                    "source-memo-stat-local-added-total" : "34",
                    "source-memo-stat-remote-removed-total" : "5",
                    "source-todo-mode" : "slow",
                    "source-todo-first" : "true",
                    "source-todo-resume" : "false",
                    "source-todo-status" : "0",
                    "source-todo-backup-before" : "2",
                    "source-todo-backup-after" : "2",
                    "source-todo-stat-local-any-sent" : "619",
                    "source-todo-stat-remote-added-total" : "71",
                    "source-todo-stat-remote-updated-total" : "1",
                    "source-todo-stat-local-updated-total" : "9",
                    "source-todo-stat-remote-any-reject" : "10",
                    "source-todo-stat-remote-any-conflict_duplicated" : "15",
                    "source-todo-stat-remote-any-conflict_client_won" : "7",
                    "source-todo-stat-local-any-received" : "2",
                    "source-todo-stat-local-removed-total" : "4",
                    "source-todo-stat-remote-any-conflict_server_won" : "8",
                    "source-todo-stat-local-any-reject" : "3",
                    "source-todo-stat-local-added-total" : "24",
                    "source-todo-stat-remote-removed-total" : "80" }
        reports = self.session.GetReports(0, 0, utf8_strings=True)
        self.failUnlessEqual(reports, [])
        # get only one report
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        self.assertTrue(len(reports) == 1)
        del reports[0]["dir"]

        self.failUnlessEqual(reports[0], report0)
        """ the number of reference sessions is totally 5. Check the returned count
        when parameter is bigger than 5 """
        reports = self.session.GetReports(0, 0xFFFFFFFF, utf8_strings=True)
        self.assertTrue(len(reports) == 5)
        # start from 2, this could check integer overflow
        reports2 = self.session.GetReports(2, 0xFFFFFFFF, utf8_strings=True)
        self.assertTrue(len(reports2) == 3)
        # the first element of reports2 should be the same as the third element of reports
        self.failUnlessEqual(reports[2], reports2[0])
        # indexed from 5, nothing could be gotten
        reports = self.session.GetReports(5, 0xFFFFFFFF, utf8_strings=True)
        self.failUnlessEqual(reports, [])

    def testRestoreByRef(self):
        # test when the data before or after a given session is restored
        self.setupFiles('restore')
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        dir = reports[0]["dir"]
        sessionpath, session = self.createSession("dummy-test", False)
        #TODO: check restore result, how?
        #restore data before this session
        self.session.Restore(dir, True, [], utf8_strings=True)
        loop.run()
        self.session.Detach()

        # check recorded events in DBusUtil.events, first filter them
        statuses = []
        progresses = []
        for item in DBusUtil.events:
            if item[0] == "status":
                statuses.append(item[1])
            elif item[0] == "progress":
                progresses.append(item[1])

        lastStatus = ""
        lastSources = {}
        statusPairs = {"": 0, "idle": 1, "running" : 2, "done" : 3}
        for status, error, sources in statuses:
            self.failIf(status == lastStatus and lastSources == sources)
            # no error
            self.failUnlessEqual(error, 0)
            for sourcename, value in sources.items():
                # no error
                self.failUnlessEqual(value[2], 0)
                # keep order: source status must also be unchanged or the next status
                if lastSources.has_key(sourcename):
                    lastValue = lastSources[sourcename]
                    self.failUnless(statusPairs[value[1]] >= statusPairs[lastValue[1]])

            lastStatus = status
            lastSources = sources

        # check increasing progress percentage
        lastPercent = 0
        for percent, sources in progresses:
            self.failIf(percent < lastPercent)
            lastPercent = percent

        session.SetConfig(False, False, self.config, utf8_strings=True)
        #restore data after this session
        session.Restore(dir, False, ["addressbook", "calendar"], utf8_strings=True)
        loop.run()

    def testSecondRestore(self):
        # test the right error is thrown when session is not active
        self.setupFiles('restore')
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        reports = self.session.GetReports(0, 1, utf8_strings=True)
        dir = reports[0]["dir"]
        sessionpath, session = self.createSession("dummy-test", False)
        try:
            session.Restore(dir, False, [], utf8_strings=True)
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                    "org.syncevolution.InvalidCall: session is not active, call not allowed at this time")
        else:
            self.fail("no exception thrown")

        self.session.Detach()
        session.SetConfig(False, False, self.config, utf8_strings=True)
        session.Restore(dir, False, [], utf8_strings=True)
        loop.run()

    @timeout(300)
    def testInteractivePassword(self):
        """ test the info request is correctly working for password """
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        self.lastState = "unknown"
        # define callback for InfoRequest signals and send corresponds response
        # to dbus server
        def infoRequest(id, session, state, handler, type, params):
            if state == "request":
                self.failUnlessEqual(self.lastState, "unknown")
                self.lastState = "request"
                self.server.InfoResponse(id, "working", {}, utf8_strings=True)
            elif state == "waiting":
                self.failUnlessEqual(self.lastState, "request")
                self.lastState = "waiting"
                self.server.InfoResponse(id, "response", {"password" : "123456"}, utf8_strings=True)
            elif state == "done":
                self.failUnlessEqual(self.lastState, "waiting")
                self.lastState = "done"
            else:
                self.fail("state should not be '" + state + "'")

        signal = bus.add_signal_receiver(infoRequest,
                                         'InfoRequest',
                                         'org.syncevolution.Server',
                                         'org.syncevolution',
                                         None,
                                         byte_arrays=True,
                                         utf8_strings=True)

        # dbus server will be blocked by gnome-keyring-ask dialog, so we kill it, and then 
        # it can't get the password from gnome keyring and send info request for password
        def callback():
            kill = subprocess.Popen("sh -c 'killall -9 gnome-keyring-ask >/dev/null 2>&1'", shell=True)
            kill.communicate()
            return True

        timeout_handler = Timeout.addTimeout(1, callback)

        # try to sync and invoke password request
        self.session.Sync("", {})
        loop.run()
        Timeout.removeTimeout(timeout_handler)
        self.failUnlessEqual(self.lastState, "done")

class TestSessionAPIsReal(unittest.TestCase, DBusUtil):
    """ This class is used to test those unit tests of session APIs, depending on doing sync.
        Thus we need a real server configuration to confirm sync could be run successfully.
        Typically we need make sure that at least one sync has been done before testing our
        desired unit tests. Note that it also covers session.Sync API itself """
    """ All unit tests in this class have a dependency on a real sync config named 'dbus_unittest',
        which should works correctly. """

    def setUp(self):
        self.setUpServer()
        self.setUpSession(configName)
        self.operation = "" 

    def run(self, result):
        self.runTest(result, own_xdg=False)

    def setupConfig(self):
        """ Apply for user settings. Used internally. """
        configProps = { }
        # check whether 'dbus_unittest' is configured.
        try:
            configProps = self.session.GetConfig(False, utf8_strings=True)
        except dbus.DBusException, ex:
            self.fail(str(ex) + 
                      ". To test this case, please first set up a correct config named 'dbus_unittest'.")
        updateProps = self.getDatabases(configProps)
        # temporarily set evolutionsource and don't change them
        self.session.SetConfig(True, True, updateProps, utf8_strings=True)

    def doSync(self):
        self.setupConfig()
        self.setUpListeners(self.sessionpath)
        self.session.Sync("slow", {})
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])

    def progressChanged(self, *args):
        # subclass specifies its own callback for ProgressChanged signals
        percentage = args[0]
        # make sure sync is really running
        if percentage > 20:
            if self.operation == "abort":
                self.session.Abort()
            if self.operation == "suspend":
                self.session.Suspend()

    @timeout(300)
    def testSync(self):
        '''run a real sync with default server and test status list and progress number'''
        ''' check events list is correct for StatusChanged and ProgressChanged '''
        # do sync
        self.doSync()
        self.checkSync()
    
    @timeout(300)
    def testSyncStatusAbort(self):
        ''' test status is set correctly when the session is aborted '''
        self.operation = "abort"
        self.doSync()
        hasAbortingStatus = False
        for item in DBusUtil.events:
            if item[0] == "status" and item[1][0] == "aborting":
                hasAbortingStatus = True
                break
        self.failUnlessEqual(hasAbortingStatus, True)

    @timeout(300)
    def testSyncStatusSuspend(self):
        ''' test status is set correctly when the session is suspended '''
        self.operation = "suspend"
        self.doSync()
        hasSuspendingStatus = False
        for item in DBusUtil.events:
            if item[0] == "status" and "suspending" in item[1][0] :
                hasSuspendingStatus = True
                break
        self.failUnlessEqual(hasSuspendingStatus, True)

    @timeout(300)
    def testSyncSecondSession(self):
        '''ask for a second session that becomes ready after a real sync'''
        sessionpath2, session2 = self.createSession("", False)
        status, error, sources = session2.GetStatus(utf8_strings=True)
        self.failUnlessEqual(status, "queueing")
        self.testSync()
        # now wait for second session becoming ready
        loop.run()
        status, error, sources = session2.GetStatus(utf8_strings=True)
        self.failUnlessEqual(status, "idle")
        self.failUnlessEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done",
                                                    "session " + sessionpath2 + " ready"])
        session2.Detach()

class TestDBusSyncError(unittest.TestCase, DBusUtil):
    def setUp(self):
        self.setUpServer()
        self.setUpSession(configName)

    def run(self, result):
        self.runTest(result, own_xdg=True)

    def testSyncNoConfig(self):
        """Executes a real sync with no corresponding config."""
        self.setUpListeners(self.sessionpath)
        self.session.Sync("", {})
        loop.run()
        # TODO: check recorded events in DBusUtil.events
        status, error, sources = self.session.GetStatus(utf8_strings=True)
        self.failUnlessEqual(status, "done")
        self.failUnlessEqual(error, 10500)

class TestConnection(unittest.TestCase, DBusUtil):
    """Tests Server.Connect(). Tests depend on getting one Abort signal to terminate."""

    """a real message sent to our own server, DevInf stripped, username/password foo/bar"""
    message1 = '''<?xml version="1.0" encoding="UTF-8"?><SyncML xmlns='SYNCML:SYNCML1.2'><SyncHdr><VerDTD>1.2</VerDTD><VerProto>SyncML/1.2</VerProto><SessionID>255</SessionID><MsgID>1</MsgID><Target><LocURI>http://127.0.0.1:9000/syncevolution</LocURI></Target><Source><LocURI>sc-api-nat</LocURI><LocName>test</LocName></Source><Cred><Meta><Format xmlns='syncml:metinf'>b64</Format><Type xmlns='syncml:metinf'>syncml:auth-md5</Type></Meta><Data>kHzMn3RWFGWSKeBpXicppQ==</Data></Cred><Meta><MaxMsgSize xmlns='syncml:metinf'>20000</MaxMsgSize><MaxObjSize xmlns='syncml:metinf'>4000000</MaxObjSize></Meta></SyncHdr><SyncBody><Alert><CmdID>1</CmdID><Data>200</Data><Item><Target><LocURI>addressbook</LocURI></Target><Source><LocURI>./addressbook</LocURI></Source><Meta><Anchor xmlns='syncml:metinf'><Last>20091105T092757Z</Last><Next>20091105T092831Z</Next></Anchor><MaxObjSize xmlns='syncml:metinf'>4000000</MaxObjSize></Meta></Item></Alert><Final/></SyncBody></SyncML>'''

    def setUp(self):
        self.setUpServer()
        self.setUpListeners(None)
        # default config
        self.config = { 
                         "" : { "remoteDeviceId" : "sc-api-nat",
                                "password" : "test",
                                "username" : "test",
                                "PeerIsClient" : "1",
                                "RetryInterval" : "1",
                                "RetryDuration" : "10"
                              },
                         "source/addressbook" : { "sync" : "two-way",
                                                  "backend" : "addressbook",
                                                  "uri" : "card"
                                                },
                         "source/calendar"    : { "sync" : "two-way",
                                                  "backend" : "calendar",
                                                  "uri" : "cal"
                                                },
                         "source/todo"        : { "sync" : "two-way",
                                                  "backend" : "todo",
                                                  "uri" : "task"
                                                },
                         "source/memo"        : { "sync" : "two-way",
                                                  "backend" : "memo",
                                                  "uri" : "text"
                                                }
                       }

        #create default or user settings evolutionsource
        updateProps = self.getDatabases(self.config)
        for key, dict in updateProps.items():
            self.config[key]["database"] = dict["database"]

    def setupConfig(self, name="dummy-test", deviceId="sc-api-nat"):
        self.setUpSession(name)
        self.config[""]["remoteDeviceId"] = deviceId
        self.session.SetConfig(False, False, self.config, utf8_strings=True)
        self.session.Detach()

    def run(self, result):
        self.runTest(result, own_xdg=True)

    def getConnection(self, must_authenticate=False):
        conpath = self.server.Connect({'description': 'test-dbus.py',
                                       'transport': 'dummy'},
                                      must_authenticate,
                                      "")
        self.setUpConnectionListeners(conpath)
        connection = dbus.Interface(bus.get_object('org.syncevolution',
                                                   conpath),
                                    'org.syncevolution.Connection')
        return (conpath, connection)

    def testConnect(self):
        """get connection and close it"""
        conpath, connection = self.getConnection()
        connection.Close(False, 'good bye')
        loop.run()
        self.failUnlessEqual(DBusUtil.events, [('abort',)])

    def testInvalidConnect(self):
        """get connection, send invalid initial message"""
        self.setupConfig()
        conpath, connection = self.getConnection()
        try:
            connection.Process('1234', 'invalid message type')
        except dbus.DBusException, ex:
            self.failUnlessEqual(str(ex),
                                 "org.syncevolution.Exception: message type 'invalid message type' not supported for starting a sync")
        else:
            self.fail("no exception thrown")
        loop.run()
        # 'idle' status doesn't be checked
        self.failUnless(('abort',) in DBusUtil.events)

    def testStartSync(self):
        """send a valid initial SyncML message"""
        self.setupConfig()
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        # TODO: check events
        self.failIfEqual(DBusUtil.reply, None)
        self.failUnlessEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # credentials should have been accepted because must_authenticate=False
        # in Connect(); 508 = "refresh required" is normal
        self.failUnless('<Status><CmdID>2</CmdID><MsgRef>1</MsgRef><CmdRef>1</CmdRef><Cmd>Alert</Cmd><TargetRef>addressbook</TargetRef><SourceRef>./addressbook</SourceRef><Data>508</Data>' in DBusUtil.reply[0])
        self.failIf('<Chal>' in DBusUtil.reply[0])
        self.failUnlessEqual(DBusUtil.reply[3], False)
        self.failIfEqual(DBusUtil.reply[4], '')
        connection.Close(False, 'good bye')
        loop.run()
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])
        # start another session for the server (ensures that the previous one is done),
        # then check the server side report
        DBusUtil.quit_events = []
        self.setUpSession("dummy-test")
        sessions = self.session.GetReports(0, 100)
        self.failUnlessEqual(len(sessions), 1)
        # transport failure, only addressbook active and later aborted
        self.failUnlessEqual(sessions[0]["status"], "20043")
        self.failUnlessEqual(sessions[0]["error"], "D-Bus peer has disconnected")
        self.failUnlessEqual(sessions[0]["source-addressbook-status"], "20017")
        self.failUnlessEqual(sessions[0]["source-calendar-status"], "0")
        self.failUnlessEqual(sessions[0]["source-todo-status"], "0")
        self.failUnlessEqual(sessions[0]["source-memo-status"], "0")


    def testCredentialsWrong(self):
        """send invalid credentials"""
        self.setupConfig()
        conpath, connection = self.getConnection(must_authenticate=True)
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        # TODO: check events
        self.failIfEqual(DBusUtil.reply, None)
        self.failUnlessEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # credentials should have been rejected because of wrong Nonce
        self.failUnless('<Chal>' in DBusUtil.reply[0])
        self.failUnlessEqual(DBusUtil.reply[3], False)
        self.failIfEqual(DBusUtil.reply[4], '')
        connection.Close(False, 'good bye')
        # when the login fails, the server also ends the session
        loop.run()
        loop.run()
        loop.run()
        DBusUtil.quit_events.sort()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "connection " + conpath + " got final reply",
                                                    "session done"])

    def testCredentialsRight(self):
        """send correct credentials"""
        self.setupConfig()
        conpath, connection = self.getConnection(must_authenticate=True)
        plain_auth = TestConnection.message1.replace("<Type xmlns='syncml:metinf'>syncml:auth-md5</Type></Meta><Data>kHzMn3RWFGWSKeBpXicppQ==</Data>",
                                                     "<Type xmlns='syncml:metinf'>syncml:auth-basic</Type></Meta><Data>dGVzdDp0ZXN0</Data>")
        connection.Process(plain_auth, 'application/vnd.syncml+xml')
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        self.failIfEqual(DBusUtil.reply, None)
        self.failUnlessEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # credentials should have been accepted because with basic auth,
        # credentials can be replayed; 508 = "refresh required" is normal
        self.failUnless('<Status><CmdID>2</CmdID><MsgRef>1</MsgRef><CmdRef>1</CmdRef><Cmd>Alert</Cmd><TargetRef>addressbook</TargetRef><SourceRef>./addressbook</SourceRef><Data>508</Data>' in DBusUtil.reply[0])
        self.failUnlessEqual(DBusUtil.reply[3], False)
        self.failIfEqual(DBusUtil.reply[4], '')
        connection.Close(False, 'good bye')
        loop.run()
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])

    def testStartSyncTwice(self):
        """send the same SyncML message twice, starting two sessions"""
        self.setupConfig()
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        # TODO: check events
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        self.failIfEqual(DBusUtil.reply, None)
        self.failUnlessEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        self.failUnlessEqual(DBusUtil.reply[3], False)
        self.failIfEqual(DBusUtil.reply[4], '')
        DBusUtil.reply = None
        DBusUtil.quit_events = []

        # Now start another session with the same client *without*
        # closing the first one. The server should detect this
        # and forcefully close the first one.
        conpath2, connection2 = self.getConnection()
        connection2.Process(TestConnection.message1, 'application/vnd.syncml+xml')

        # reasons for leaving the loop, in random order:
        # - abort of first connection
        # - first session done
        # - reply for second one
        loop.run()
        loop.run()
        loop.run()
        DBusUtil.quit_events.sort()
        expected = [ "connection " + conpath + " aborted",
                     "session done",
                     "connection " + conpath2 + " got reply" ]
        expected.sort()
        self.failUnlessEqual(DBusUtil.quit_events, expected)
        self.failIfEqual(DBusUtil.reply, None)
        self.failUnlessEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        self.failUnlessEqual(DBusUtil.reply[3], False)
        self.failIfEqual(DBusUtil.reply[4], '')
        DBusUtil.quit_events = []

        # now quit for good
        connection2.Close(False, 'good bye')
        loop.run()
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath2 + " aborted",
                                                    "session done"])

    def testKillInactive(self):
        """block server with client A, then let client B connect twice"""
        #set up 2 configs
        self.setupConfig()
        self.setupConfig("dummy", "sc-pim-ppc")
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        # TODO: check events
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        self.failIfEqual(DBusUtil.reply, None)
        self.failUnlessEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        self.failUnlessEqual(DBusUtil.reply[3], False)
        self.failIfEqual(DBusUtil.reply[4], '')
        DBusUtil.reply = None
        DBusUtil.quit_events = []

        # Now start two more sessions with the second client *without*
        # closing the first one. The server should remove only the
        # first connection of client B.
        message1_clientB = TestConnection.message1.replace("sc-api-nat", "sc-pim-ppc")
        conpath2, connection2 = self.getConnection()
        connection2.Process(message1_clientB, 'application/vnd.syncml+xml')
        conpath3, connection3 = self.getConnection()
        connection3.Process(message1_clientB, 'application/vnd.syncml+xml')
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, [ "connection " + conpath2 + " aborted" ])
        DBusUtil.quit_events = []

        # now quit for good
        connection3.Close(False, 'good bye client B')
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, [ "connection " + conpath3 + " aborted" ])
        DBusUtil.quit_events = []
        connection.Close(False, 'good bye client A')
        loop.run()
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])

    @timeout(20)
    def testTimeoutSync(self):
        """start a sync, then wait for server to detect that we stopped replying
        
        The server-side configuration for sc-api-nat must contain a retryDuration=10
        because this test itself will time out with a failure after 20 seconds."""
        self.setupConfig()
        conpath, connection = self.getConnection()
        connection.Process(TestConnection.message1, 'application/vnd.syncml+xml')
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " got reply"])
        DBusUtil.quit_events = []
        # TODO: check events
        self.failIfEqual(DBusUtil.reply, None)
        self.failUnlessEqual(DBusUtil.reply[1], 'application/vnd.syncml+xml')
        # wait for connection reset and "session done" due to timeout
        loop.run()
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["connection " + conpath + " aborted",
                                                    "session done"])

class TestMultipleConfigs(unittest.TestCase, DBusUtil):
    """ sharing of properties between configs

    Creates and tests the configs 'foo', 'bar', 'foo@other_context',
    '@default' and checks that 'defaultPeer' (global), 'syncURL' (per
    peer), 'database' (per source), 'uri' (per source and peer)
    are shared correctly.

    Runs with a the server ready, without session."""

    def setUp(self):
        self.setUpServer()

    def run(self, result):
        self.runTest(result)

    def setupEmpty(self):
        """Creates empty configs 'foo', 'bar', 'foo@other_context'.
        Updating non-existant configs is an error. Use this
        function before trying to update one of these configs."""
        self.setUpSession("foo")
        self.session.SetConfig(False, False, {"" : {}})
        self.session.Detach()
        self.setUpSession("bar")
        self.session.SetConfig(False, False, {"": {}})
        self.session.Detach()
        self.setUpSession("foo@other_CONTEXT")
        self.session.SetConfig(False, False, {"": {}})
        self.session.Detach()

    def setupConfigs(self):
        """Creates polulated configs 'foo', 'bar', 'foo@other_context'."""
        self.setupEmpty()

        # update normal view on "foo"
        self.setUpSession("foo")
        self.session.SetConfig(True, False,
                               { "" : { "defaultPeer" : "foobar_peer",
                                        "deviceId" : "shared-device-identifier",
                                        "syncURL": "http://scheduleworld" },
                                 "source/calendar" : { "uri" : "cal3" },
                                 "source/addressbook" : { "database": "Personal",
                                                          "sync" : "two-way",
                                                          "uri": "card3" } },
                               utf8_strings=True)
        self.session.Detach()

        # "bar" shares properties with "foo"
        self.setUpSession("bar")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["deviceId"], "shared-device-identifier")
        self.failUnlessEqual(config["source/addressbook"]["database"], "Personal")
        self.session.SetConfig(True, False,
                               { "" : { "syncURL": "http://funambol" },
                                 "source/calendar" : { "uri" : "cal" },
                                 "source/addressbook" : { "database": "Work",
                                                          "sync" : "refresh-from-client",
                                                          "uri": "card" } },
                               utf8_strings=True)
        self.session.Detach()

    def testSharing(self):
        """set up configs and tests reading them"""
        self.setupConfigs()

        # check how view "foo" has been modified
        self.setUpSession("Foo@deFAULT")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["syncURL"], "http://scheduleworld")
        self.failUnlessEqual(config["source/addressbook"]["database"], "Work")
        self.failUnlessEqual(config["source/addressbook"]["uri"], "card3")
        self.session.Detach()

        # different ways of addressing this context
        self.setUpSession("")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnless("source/addressbook" in config)
        self.failIf("uri" in config["source/addressbook"])
        self.session.Detach()

        self.setUpSession("@DEFAULT")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["deviceId"], "shared-device-identifier")
        self.failUnless("source/addressbook" in config)
        self.failIf("uri" in config["source/addressbook"])
        self.session.Detach()

        # different context
        self.setUpSession("@other_context")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failIf("source/addressbook" in config)        
        self.session.Detach()

    def testSharedTemplate(self):
        """templates must contain shared properties"""
        self.setupConfigs()

        config = self.server.GetConfig("scheduleworld", True, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["deviceId"], "shared-device-identifier")
        self.failUnlessEqual(config["source/addressbook"]["database"], "Work")

    def testSharedType(self):
        """'type' consists of per-peer and shared properties"""
        self.setupConfigs()

        # writing for peer modifies properties in "foo" and context
        self.setUpSession("Foo@deFAULT")
        config = self.session.GetConfig(False, utf8_strings=True)
        config["source/addressbook"]["syncFormat"] = "text/vcard"
        config["source/addressbook"]["backend"] = "file"
        config["source/addressbook"]["databaseFormat"] = "text/x-vcard"
        self.session.SetConfig(True, False,
                               config,
                               utf8_strings=True)
        config = self.server.GetConfig("Foo", False, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["syncFormat"], "text/vcard")
        config = self.server.GetConfig("@default", False, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["backend"], "file")
        self.failUnlessEqual(config["source/addressbook"]["databaseFormat"], "text/x-vcard")
        self.session.Detach()

    def testSharedTypeOther(self):
        """shared backend properties must be preserved when adding peers"""
        # writing peer modifies properties in "foo" and creates context "@other"
        self.setUpSession("Foo@other")
        config = self.server.GetConfig("ScheduleWorld@other", True, utf8_strings=True)
        config["source/addressbook"]["backend"] = "file"
        config["source/addressbook"]["databaseFormat"] = "text/x-vcard"
        self.session.SetConfig(False, False,
                               config,
                               utf8_strings=True)
        config = self.server.GetConfig("Foo", False, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["backend"], "file")
        config = self.server.GetConfig("@other", False, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["databaseFormat"], "text/x-vcard")
        self.session.Detach()

        # adding second client must preserve type
        self.setUpSession("bar@other")
        config = self.server.GetConfig("Funambol@other", True, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["backend"], "file")
        self.session.SetConfig(False, False,
                               config,
                               utf8_strings=True)
        config = self.server.GetConfig("bar", False, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["backend"], "file")
        self.failUnlessEqual(config["source/addressbook"].get("syncFormat"), None)
        config = self.server.GetConfig("@other", False, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["databaseFormat"], "text/x-vcard")

    def testOtherContext(self):
        """write into independent context"""
        self.setupConfigs()

        # write independent "foo@other_context" config
        self.setUpSession("foo@other_context")
        config = self.session.GetConfig(False, utf8_strings=True)
        config[""]["syncURL"] = "http://scheduleworld2"
        config["source/addressbook"] = { "database": "Play",
                                         "uri": "card30" }
        self.session.SetConfig(True, False,
                               config,
                               utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["syncURL"], "http://scheduleworld2")
        self.failUnlessEqual(config["source/addressbook"]["database"], "Play")
        self.failUnlessEqual(config["source/addressbook"]["uri"], "card30")
        self.session.Detach()

        # "foo" modified?
        self.setUpSession("foo")
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["syncURL"], "http://scheduleworld")
        self.failUnlessEqual(config["source/addressbook"]["database"], "Work")
        self.failUnlessEqual(config["source/addressbook"]["uri"], "card3")
        self.session.Detach()

    def testSourceRemovalLocal(self):
        '''remove "addressbook" source in "foo"'''
        self.setupConfigs()
        self.setUpSession("foo")
        config = self.session.GetConfig(False, utf8_strings=True)
        del config["source/addressbook"]
        self.session.SetConfig(False, False, config, utf8_strings=True)
        self.session.Detach()

        # "addressbook" still exists in "foo" but only with default values
        config = self.server.GetConfig("foo", False, utf8_strings=True)
        self.failIf("uri" in config["source/addressbook"])
        self.failIf("sync" in config["source/addressbook"])

        # "addressbook" unchanged in "bar"
        config = self.server.GetConfig("bar", False, utf8_strings=True)
        self.failUnlessEqual(config["source/addressbook"]["uri"], "card")
        self.failUnlessEqual(config["source/addressbook"]["sync"], "refresh-from-client")

    def testSourceRemovalGlobal(self):
        '''remove "addressbook" everywhere'''
        self.setupConfigs()
        self.setUpSession("")
        config = self.session.GetConfig(False, utf8_strings=True)
        del config["source/addressbook"]
        self.session.SetConfig(False, False, config, utf8_strings=True)
        self.session.Detach()

        # "addressbook" gone in "foo" and "bar"
        config = self.server.GetConfig("foo", False, utf8_strings=True)
        self.failIf("source/addressbook" in config)
        config = self.server.GetConfig("bar", False, utf8_strings=True)
        self.failIf("source/addressbook" in config)

    def testRemovePeer(self):
        '''check listing of peers while removing "bar"'''
        self.setupConfigs()
        self.testOtherContext()
        self.setUpSession("bar")
        peers = self.session.GetConfigs(False, utf8_strings=True)
        self.failUnlessEqual(peers,
                             [ "bar", "foo", "foo@other_context" ])
        peers2 = self.server.GetConfigs(False, utf8_strings=True)
        self.failUnlessEqual(peers, peers2)
        # remove "bar"
        self.session.SetConfig(False, False, {}, utf8_strings=True)
        peers = self.server.GetConfigs(False, utf8_strings=True)
        self.failUnlessEqual(peers,
                             [ "foo", "foo@other_context" ])
        self.session.Detach()

        # other configs should not have been affected
        config = self.server.GetConfig("foo", False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["syncURL"], "http://scheduleworld")
        self.failUnlessEqual(config["source/calendar"]["uri"], "cal3")
        config = self.server.GetConfig("foo@other_context", False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["syncURL"], "http://scheduleworld2")
        self.failUnlessEqual(config["source/addressbook"]["database"], "Play")
        self.failUnlessEqual(config["source/addressbook"]["uri"], "card30")

    def testRemoveContext(self):
        '''remove complete config'''
        self.setupConfigs()
        self.setUpSession("")
        self.session.SetConfig(False, False, {}, utf8_strings=True)
        config = self.session.GetConfig(False, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        peers = self.server.GetConfigs(False, utf8_strings=True)
        self.failUnlessEqual(peers, ['foo@other_context'])
        self.session.Detach()

    def testTemplates(self):
        '''templates reuse common properties'''
        self.setupConfigs()

        # deviceID must be shared and thus be reused in templates
        self.setUpSession("")
        config = self.session.GetConfig(False, utf8_strings=True)
        config[""]["DEVICEID"] = "shared-device-identifier"
        self.session.SetConfig(True, False, config, utf8_strings=True)
        config = self.server.GetConfig("", False, utf8_strings=True)
        self.failUnlessEqual(config[""]["deviceId"], "shared-device-identifier")

        # get template for default context
        config = self.server.GetConfig("scheduleworld", True, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failUnlessEqual(config[""]["deviceId"], "shared-device-identifier")

        # now for @other_context - different device ID!
        config = self.server.GetConfig("scheduleworld@other_context", True, utf8_strings=True)
        self.failUnlessEqual(config[""]["defaultPeer"], "foobar_peer")
        self.failIfEqual(config[""]["deviceId"], "shared-device-identifier")

class TestLocalSync(unittest.TestCase, DBusUtil):
    """Tests involving local sync."""

    def setUp(self):
        self.setUpServer()
        # create file<->file configs
        self.setUpSession("source-config@client")
        self.session.SetConfig(False, False,
                               {"" : { "loglevel": "4" },
                                "source/addressbook": { "sync": "two-way",
                                                        "backend": "file",
                                                        "databaseFormat": "text/vcard",
                                                        "database": "file://" + xdg_root + "/client" } })
        self.session.Detach()
        self.setUpSession("server")
        self.session.SetConfig(False, False,
                               {"" : { "loglevel": "4",
                                       "syncURL": "local://@client",
                                       "RetryDuration": self.getTestProperty("resendDuration", "60"),
                                       "peerIsClient": "1" },
                                "source/addressbook": { "sync": "two-way",
                                                        "uri": "addressbook",
                                                        "backend": "file",
                                                        "databaseFormat": "text/vcard",
                                                        "database": "file://" + xdg_root + "/server" } })

    def testSync(self):
        '''run a simple slow sync between local dirs'''
        os.makedirs(xdg_root + "/server")
        output = open(xdg_root + "/server/0", "w")
        output.write('''BEGIN:VCARD
VERSION:3.0
FN:John Doe
N:Doe;John
END:VCARD''')
        output.close()
        self.setUpListeners(self.sessionpath)
        self.session.Sync("slow", {})
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        self.checkSync()
        input = open(xdg_root + "/server/0", "r")
        self.failUnless("FN:John Doe" in input.read())

    @timeout(10)
    @property("resendDuration", "3")
    @property("ENV", "SYNCEVOLUTION_LOCAL_CHILD_DELAY=5")
    def testTimeout(self):
        '''master must detect a hanging child'''
        self.setUpListeners(self.sessionpath)
        self.session.Sync("slow", {})
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        report = self.checkSync(20017, 20043) # sources aborted, transport failure
        self.failUnlessEqual(report["error"], "timeout, retry period exceeded")

    @timeout(10)
    @property("ENV", "SYNCEVOLUTION_LOCAL_CHILD_DELAY=5")
    def testConcurrency(self):
        '''D-Bus server must remain responsive while sync runs'''
        self.setUpListeners(self.sessionpath)
        self.session.Sync("slow", {})
        time.sleep(2)
        status, error, sources = self.session.GetStatus(utf8_strings=True)
        self.failUnlessEqual(status, "running")
        self.failUnlessEqual(error, 0)
        self.session.Abort()
        loop.run()
        self.failUnlessEqual(DBusUtil.quit_events, ["session " + self.sessionpath + " done"])
        report = self.checkSync(20017, 20017) # aborted
        self.failIf("error" in report) # ... but without error message
        self.failUnlessEqual(report["source-addressbook-status"], "0") # unknown status for source (aborted early)

    def run(self, result):
        self.runTest(result)

class TestFileNotify(unittest.TestCase, DBusUtil):
    """syncevo-dbus-server must stop if one of its files mapped into
    memory (executable, libraries) change. Furthermore it must restart
    if automatic syncs are enabled. This class simulates such file changes
    by starting the server, identifying the location of the main executable,
    and renaming it back and forth."""

    def setUp(self):
        self.setUpServer()
        self.serverexe = self.serverExecutable()

    def tearDown(self):
        if os.path.isfile(self.serverexe + ".bak"):
            os.rename(self.serverexe + ".bak", self.serverexe)

    def run(self, result):
        self.runTest(result)

    def modifyServerFile(self):
        """rename server executable to trigger shutdown"""
        os.rename(self.serverexe, self.serverexe + ".bak")
        os.rename(self.serverexe + ".bak", self.serverexe)        

    @timeout(100)
    def testShutdown(self):
        """update server binary for 30 seconds, check that it shuts down at most 15 seconds after last mod"""
        self.failUnless(self.isServerRunning())
        i = 0
        # Server must not shut down immediately, more changes might follow.
        # Simulate that.
        while i < 6:
            self.modifyServerFile()
            time.sleep(5)
            i = i + 1
        self.failUnless(self.isServerRunning())
        time.sleep(10)
        self.failIf(self.isServerRunning())

    @timeout(30)
    def testSession(self):
        """create session, shut down directly after closing it"""
        self.failUnless(self.isServerRunning())
        self.setUpSession("")
        self.modifyServerFile()
        time.sleep(15)
        self.failUnless(self.isServerRunning())
        self.session.Detach()
        # should shut down almost immediately
        time.sleep(1)
        self.failIf(self.isServerRunning())

    @timeout(30)
    def testSession2(self):
        """create session, shut down after quiesence period after closing it"""
        self.failUnless(self.isServerRunning())
        self.setUpSession("")
        self.modifyServerFile()
        self.failUnless(self.isServerRunning())
        self.session.Detach()
        time.sleep(8)
        self.failUnless(self.isServerRunning())
        time.sleep(4)
        self.failIf(self.isServerRunning())

    @timeout(60)
    def testRestart(self):
        """set up auto sync, then check that server restarts"""
        self.failUnless(self.isServerRunning())
        self.setUpSession("memotoo")
        config = self.session.GetConfig(True, utf8_strings=True)
        config[""]["autoSync"] = "1"
        self.session.SetConfig(False, False, config)
        self.failUnless(self.isServerRunning())
        self.session.Detach()
        self.modifyServerFile()
        bus_name = self.server.bus_name
        # give server time to restart
        time.sleep(15)
        self.setUpServer()
        self.failIfEqual(bus_name, self.server.bus_name)
        # serverExecutable() will fail if the service wasn't properly
        # with execve() because then the old process is dead.
        self.failUnlessEqual(self.serverexe, self.serverExecutable())

if __name__ == '__main__':
    unittest.main()
