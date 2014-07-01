#!/usr/bin/python -u

"""
The general idea is that tests to run are defined as a list of
actions. Each action has a unique name and can depend on other
actions to have run successfully before.

Most work is executed in directories defined and owned by these
actions. The framework only manages one directory which represents
the result of each action:
- an overview file which lists the result of each action
- for each action a directory with stderr/out and additional files
  that the action can put there
"""

import os, sys, popen2, traceback, re, time, smtplib, optparse, stat, shutil, StringIO, MimeWriter
import shlex
import subprocess
import fnmatch
import copy
import errno
import signal
import stat
import exceptions

def log(format, *args):
    now = time.time()
    print 'runtests.py-%d' % os.getpid(), time.asctime(time.gmtime(now)), 'UTC', '(+ %.1fs / %.1fs)' % (now - log.latest, now - log.start), format % args
    log.latest = now
log.start = time.time()
log.latest = log.start

try:
    import gzip
    havegzip = True
except:
    havegzip = False

def cd(path):
    """Enter directories, creating them if necessary."""
    if not os.access(path, os.F_OK):
        os.makedirs(path)
    os.chdir(path)
    log('changing into directory %s (= %s)', path, os.getcwd())

def abspath(path):
    """Absolute path after expanding vars and user."""
    return os.path.abspath(os.path.expanduser(os.path.expandvars(path)))

def findInPaths(name, dirs):
    """find existing item  in one of the directories, return None if
    no directories give, absolute path to existing item or (as fallbac)
    last dir + name"""
    fullname = None
    for dir in dirs:
        fullname = os.path.join(abspath(dir), name)
        if os.access(fullname, os.F_OK):
            break
    return fullname

def del_dir(path):
    # Preserve XDG dirs, if we were set up like that by caller.
    # These dirs might already contain some relevant data.
    xdgdirs = list(os.environ.get(x, None) for x in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"))
    if path in xdgdirs:
        return
    if not os.access(path, os.F_OK):
        return
    for file in os.listdir(path):
        file_or_dir = os.path.join(path,file)
        # ensure directory is writable
        os.chmod(path, os.stat(path)[stat.ST_MODE] | stat.S_IRWXU)
        if os.path.isdir(file_or_dir) and not os.path.islink(file_or_dir):
            del_dir(file_or_dir) #it's a directory recursive call to function again
        else:
            os.remove(file_or_dir) #it's a file, delete it
    # We might have skipped deleting something, allow that.
    try:
        os.rmdir(path)
    except OSError, ex:
        if ex.errno != errno.ENOTEMPTY:
            raise

def copyLog(filename, dirname, htaccess, lineFilter=None):
    """Make a gzipped copy (if possible) with the original time stamps and find the most severe problem in it.
    That line is then added as description in a .htaccess AddDescription.
    For directories just copy the whole directory tree.
    """
    info = os.stat(filename)
    outname = os.path.join(dirname, os.path.basename(filename))

    if os.path.isdir(filename):
        # copy whole directory, without any further processing at the moment
        shutil.copytree(filename, outname, symlinks=True)
        return

    # .out files are typically small nowadays, so don't compress
    if False:
        outname = outname + ".gz"
        out = gzip.open(outname, "wb")
    else:
        out = file(outname, "w")
    error = None
    for line in file(filename, "r").readlines():
        if not error and line.find("ERROR") >= 0:
            error = line
        if lineFilter:
            line = lineFilter(line)
        out.write(line)
    out.close()
    os.utime(outname, (info[stat.ST_ATIME], info[stat.ST_MTIME]))
    if error:
        error = error.strip().replace("\"", "'").replace("<", "&lt;").replace(">","&gt;")
        htaccess.write("AddDescription \"%s\" %s\n" %
                       (error,
                        os.path.basename(filename)))
    return error

def TryKill(pid, signal):
    try:
        os.kill(pid, signal)
    except OSError, ex:
        # might have quit in the meantime, deal with the race
        # condition
        if ex.errno != 3:
            raise ex

def ShutdownSubprocess(popen, timeout):
    start = time.time()
    if popen.poll() == None:
        TryKill(popen.pid, signal.SIGTERM)
    while popen.poll() == None and start + timeout >= time.time():
        time.sleep(0.01)
    if popen.poll() == None:
        TryKill(popen.pid, signal.SIGKILL)
        while popen.poll() == None and start + timeout + 1 >= time.time():
            time.sleep(0.01)
        return False
    return True

class Jobserver:
    '''Allocates the given number of job slots from the "make -j"
    jobserver, then runs the command and finally returns the slots.
    See http://mad-scientist.net/make/jobserver.html'''
    def __init__(self):
        self.havejobserver = False
        self.allocated = 0

        # MAKEFLAGS= --jobserver-fds=3,4 -j
        flags = os.environ.get('MAKEFLAGS', '')
        m = re.search(r'--jobserver-fds=(\d+),(\d+)', flags)
        if m:
            self.receiveslots = int(m.group(1))
            self.returnslots = int(m.group(2))
            self.blocked = {}
            self.havejobserver = True
            log('using jobserver')
        else:
            log('not using jobserver')

    def active(self):
        return self.havejobserver

    def alloc(self, numjobs = 1):
        if not self.havejobserver:
            return
        n = 0
        self._block()
        try:
            while n < numjobs:
                os.read(self.receiveslots, 1)
                n += 1
            self.allocated += n
            n = 0
        except:
            os.write(self.returnslots, ' ' * n)
            raise
        finally:
            self._unblock()

    def free(self, numjobs = 1):
        if not self.havejobserver:
            return
        try:
            self.allocated -= numjobs
            os.write(self.returnslots, ' ' * numjobs)
        finally:
            self._unblock()

    def _block(self):
        '''Block signals if not already done.'''
        if not self.blocked:
            for sig in [ signal.SIGINT, signal.SIGTERM ]:
                self.blocked[sig] = signal.signal(sig, signal.SIG_IGN)

    def _unblock(self):
        '''Unblock signals if blocked and we currently own no slots.'''
        if self.blocked and not self.allocated:
            for sig, handler in self.blocked.items():
                signal.signal(sig, handler)
            self.blocked = {}

jobserver = Jobserver()

# must be set before instantiating some of the following classes
context = None

class Action:
    """Base class for all actions to be performed."""

    DONE = "0 DONE"
    WARNINGS = "1 WARNINGS"
    FAILED = "2 FAILED"
    TODO = "3 TODO"
    SKIPPED = "4 SKIPPED"
    RUNNING = "5 RUNNING"
    COMPLETED = (DONE, WARNINGS)

    def __init__(self, name):
        self.name = name
        self.status = self.TODO
        self.summary = ""
        self.dependencies = []
        self.isserver = False;
        # Assume that the action does not need its own HOME directory.
        self.needhome = False
        # Child PID of forked process executing the action while it is
        # running.
        self.worker_pid = None

    def execute(self):
        """Runs action. Throws an exeception if anything fails.
        Will be called by tryexecution() with stderr/stdout redirected into a file
        and the current directory set to an empty temporary directory.
        """
        raise Exception("not implemented")

    def nop(self):
         pass

    def tryexecution(self, step, logs):
        """wrapper around execute which handles exceptions, directories and stdout"""
        log('*** starting action %s', self.name)
        sys.stderr.flush()
        sys.stdout.flush()
        child = None
        res = 0
        try:
            child = os.fork()
            if child == 0:
                # We are the child executing the action.
                try:
                    subdirname = "%d-%s" % (step, self.name)
                    cd(subdirname)
                    if logs:
                        # Append, in case that we run multiple times for the same platform.
                        # The second run will typically have fake libsynthesis/syncevolution/compile
                        # runs which must not overwrite previous results. The new operations must
                        # be added at the end of main output.txt, too.
                        fd = os.open("output.txt", os.O_WRONLY|os.O_CREAT|os.O_APPEND)
                        os.dup2(fd, 1)
                        os.dup2(fd, 2)
                        sys.stdout = os.fdopen(fd, "w", 0) # unbuffered output!
                        sys.stderr = sys.stdout
                    if self.needhome and context.home_template:
                        # Clone home directory template?
                        home = os.path.join(context.tmpdir, 'home', self.name)
                        mapping = [('.cache', 'cache', 'XDG_CACHE_HOME'),
                                   ('.config', 'config', 'XDG_CONFIG_HOME'),
                                   ('.local/share', 'data', 'XDG_DATA_HOME')]
                        if not os.path.isdir(home):\
                            # Files that we need to handle ourselves.
                            manual = []
                            # Ignore special files like sockets (for example,
                            # .cache/keyring-5sj9Qz/control).
                            def ignore(path, entries):
                                exclude = []
                                for entry in entries:
                                    mode = os.lstat(os.path.join(path, entry)).st_mode
                                    if entry in ('akonadi.db',
                                                 'akonadiserverrc'):
                                        manual.append((path, entry))
                                        exclude.append(entry)
                                    # Copy only regular files. Ignore process id files and socket-<hostname> symlinks created
                                    # inside the home by a concurrent Akonadi instance.
                                    # Some files need special processing (see below).
                                    elif not (stat.S_ISDIR(mode) or stat.S_ISREG(mode) or stat.S_ISLNK(mode)) \
                                            or entry.endswith('.pid') \
                                            or entry.startswith('socket-'):
                                        exclude.append(entry)
                                return exclude
                            shutil.copytree(context.home_template, home,
                                            symlinks=True,
                                            ignore=ignore)

                            for path, entry in manual:
                                source = os.path.join(path, entry)
                                sourceDump = source + '.dump'
                                target = os.path.join(home, os.path.relpath(path, context.home_template), entry)
                                if entry == 'akonadi.db':
                                    # Replace XDG_DATA_HOME paths inside the sqlite3 db.
                                    # This runs *outside* of the chroot. It relies on
                                    # compatibility between the sqlite3 inside and outside the chroots.
                                    #
                                    # Occasionally in parallel testing, 'sqlite3 .dump' produced
                                    # incomplete output. Perhaps caused by parallel writes?
                                    # To work around that, a static dump is used instead if found.
                                    if os.path.isfile(sourceDump):
                                        db = open(sourceDump).read()
                                    else:
                                        db = subprocess.check_output(['sqlite3', source, '.dump'])
                                    db = db.replace(os.path.expanduser('~/.local/share/'),
                                                    os.path.join(context.stripSchrootDir(home), 'data', ''))
                                    sqlite = subprocess.Popen(['sqlite3', target],
                                                              stdin=subprocess.PIPE)
                                    sqlite.communicate(db)
                                    if sqlite.returncode:
                                        raise Exception("sqlite3 returned %d for the following input:\n%s" % (sqlite.returncode, db))
                                    db = subprocess.check_output(['sqlite3', target, '.dump'])
                                    log('target %s:\n%s', target, db)
                                elif entry == 'akonadiserverrc':
                                    # Replace hard-coded path to XDG dirs.
                                    content = open(source).read()
                                    for old, new, name in mapping:
                                        content = content.replace(os.path.expanduser('~/%s/' % old),
                                                                  os.path.join(context.stripSchrootDir(home), new, ''))
                                    rc = open(target, 'w')
                                    rc.write(content)
                                    rc.close()
                                    log('target %s:\n%s', target, content)
                        os.environ['HOME'] = context.stripSchrootDir(home)
                        for old, new, name in mapping:
                            newdir = os.path.join(home, new)
                            olddir = os.path.join(home, old)
                            if not os.path.isdir(olddir):
                                os.makedirs(olddir)
                            # Use simpler directory layout to comply with testpim.py expectations.
                            print 'old', olddir, 'new', newdir
                            os.rename(olddir, newdir)
                            # Keep the old names as symlinks, just in case.
                            os.symlink(newdir, olddir)
                            # Now use it via XDG env var *without* the schrootdir.
                            os.environ[name] = context.stripSchrootDir(newdir)
                    log('=== starting %s ===', self.name)
                    self.execute()
                except:
                    traceback.print_exc()
                    # We can't just exit() here because that ends up raising an exception
                    # which would get caught in the outer try/except.
                    res = 1
            else:
                # Parent.
                self.worker_pid = child
                self.status = Action.RUNNING
                # Can we really parallelize?
                if self.needhome and not context.home_template:
                    self.wait_for_completion()

        except Exception, inst:
            # fork() error handling in parent.
            traceback.print_exc()
            self.status = Action.FAILED
            self.summary = str(inst)

        if child == 0:
            # Child must quit.
            exit(res)
        else:
            # Parent must return.
            return self.status

    def wait_for_completion(self):
        log('*** waiting for %s (pid %d)', self.name, self.worker_pid)
        pid, exitcode = os.waitpid(self.worker_pid, 0)
        log('*** %s: %d', self.name, exitcode)
        if exitcode == 0:
            self.status = Action.DONE
        else:
            self.status = Action.FAILED
            self.summary = 'return code %d: failed' % exitcode

class Context:
    """Provides services required by actions and handles running them."""

    def __init__(self, tmpdir, resultdir, uri, workdir, mailtitle, sender, recipients, mailhost, enabled, skip, nologs, setupcmd, make, sanitychecks, lastresultdir, datadir):
        # preserve normal stdout because stdout/stderr will be redirected
        self.out = os.fdopen(os.dup(1), "w", 0) # unbuffered
        self.todo = []
        self.actions = {}
        self.tmpdir = abspath(tmpdir)
        self.resultdir = abspath(resultdir)
        self.uri = uri
        self.workdir = abspath(workdir)
        self.summary = []
        self.mailtitle = mailtitle
        self.sender = sender
        self.recipients = recipients
        self.mailhost = mailhost
        self.enabled = enabled
        self.skip = skip
        self.nologs = nologs
        self.setupcmd = setupcmd
        self.make = make
        self.sanitychecks = sanitychecks
        self.lastresultdir = lastresultdir
        self.datadir = datadir
        self.schrootdir = None

    def stripSchrootDir(self, path):
        if self.schrootdir and path.startswith(self.schrootdir + '/'):
            return path[len(self.schrootdir):]
        else:
            return path

    def findTestFile(self, name):
        """find item in SyncEvolution test directory, first using the
        generated source of the current test, then the bootstrapping code"""
        return findInPaths(name, (os.path.join(sync.basedir, "test"), self.datadir))

    def runCommand(self, cmdstr, dumpCommands=False, runAsIs=False, resources=[], jobs=1):
        """Log and run the given command, throwing an exception if it fails."""
        cmd = shlex.split(cmdstr)
        if "valgrindcheck.sh" in cmdstr:
            cmd.insert(0, "VALGRIND_LOG=%s" % os.getenv("VALGRIND_LOG", ""))
            cmd.insert(0, "VALGRIND_ARGS=%s" % os.getenv("VALGRIND_ARGS", ""))
            cmd.insert(0, "VALGRIND_LEAK_CHECK_ONLY_FIRST=%s" % os.getenv("VALGRIND_LEAK_CHECK_ONLY_FIRST", ""))
            cmd.insert(0, "VALGRIND_LEAK_CHECK_SKIP=%s" % os.getenv("VALGRIND_LEAK_CHECK_SKIP", ""))

        # move "sudo" or "env" command invocation in front of
        # all the leading env variable assignments: necessary
        # because sudo ignores them otherwise
        command = 0
        isenv = re.compile(r'[a-zA-Z0-9_]*=.*')
        while isenv.match(cmd[command]):
           command = command + 1
        if cmd[command] in ("env", "sudo"):
            cmd.insert(0, cmd[command])
            del cmd[command + 1]
        elif isenv.match(cmd[0]):
            # We did not insert env or sudo before the initial
            # variable assignment. Don't rely on the shell to
            # handle that (breaks for 'foo="x" "y"'), instead
            # use env.
            cmd.insert(0, 'env')

        if not runAsIs:
            cmdstr = " ".join(map(lambda x: (' ' in x or '(' in x or '\\' in x or x == '') and ("'" in x and '"%s"' or "'%s'") % x or x, cmd))
        if dumpCommands:
            cmdstr = "set -x; " + cmdstr

        cwd = os.getcwd()
        # Most commands involving schroot need to run with paths as seen inside the chroot.
        # Detect that in a hackish way by checking for "schroot" and then adapting
        # paths with search/replace. Exception is resultchecker.py, which runs outside
        # the chroot, but gets passed "schroot" as parameter.
        if not runAsIs and 'schroot ' in cmdstr and options.schrootdir and not 'resultchecker.py' in cmdstr:
            if cwd.startswith(options.schrootdir):
                relcwd = cwd[len(options.schrootdir):]
                cmdstr = cmdstr.replace('schroot ', 'schroot -d %s ' % relcwd)
            cmdstr = cmdstr.replace(options.schrootdir + '/', '/')
        if jobs or resources:
            helper = self.findTestFile("resources.py")
            cmdstr = helper + \
                (jobs and (' -j %d' % jobs) or '') + \
                ''.join([' -r ' + resource for resource in resources]) + \
                ' -- ' + \
                cmdstr
        relevantenv = [
            "LD_LIBRARY_PATH",
            "PATH",
            "HOME",
            "XDG_CONFIG_HOME",
            "XDG_DATA_HOME",
            "XDG_CACHE_HOME",
            ]
        log('*** ( cd %s; export %s; unset %s; %s )',
            cwd,
            " ".join(["'%s=%s'" % (x, os.getenv(x)) for x in relevantenv if  os.getenv(x, None) is not None]),
            " ".join([x for x in relevantenv if os.getenv(x, None) is None]),
            cmdstr)
        sys.stdout.flush()
        result = os.system(cmdstr)
        if result != 0:
            raise Exception("%s: failed (return code %d)" % (cmd, result>>8))

    def add(self, action):
        """Add an action for later execution. Order is important, fifo..."""
        self.todo.append(action)
        self.actions[action.name] = action

    def required(self, actionname):
        """Returns true if the action is required by one which is enabled."""
        if actionname in self.enabled:
            return True
        for action in self.todo:
            if actionname in action.dependencies and self.required(action.name):
                return True
        return False

    def execute(self):
        cd(self.resultdir)
        # Append instead of overwriting, as for other output.txt files, too.
        s = open("output.txt", "a+")
        status = Action.DONE

        step = 0
        run_servers = []
        started = []

        def check_action(action, global_status):
            if action.status == Action.FAILED:
                result = ': %s' % action.summary
            elif action.status == Action.WARNINGS:
                result = ' done, but check the warnings'
            else:
                result = ' successful'
            log('*** action %s completed, status%s', action.name, result)
            if action.status > global_status:
                global_status = action.status
            self.summary.append('%s%s' % (action.name, result))
            return global_status

        while len(self.todo) > 0:
            try:
                step = step + 1

                # get action
                action = self.todo.pop(0)

                # check whether it actually needs to be executed
                if self.enabled and \
                       not action.name in self.enabled and \
                       not self.required(action.name):
                    # disabled
                    action.status = Action.SKIPPED
                    self.summary.append("%s skipped: disabled in configuration" % (action.name))
                elif action.name in self.skip:
                    # assume that it was done earlier
                    action.status = Action.SKIPPED
                    self.summary.append("%s assumed to be done: requested by configuration" % (action.name))
                else:
                    # check dependencies
                    log('*** checking dependencies %s of %s', action.dependencies, action.name)
                    for depend in action.dependencies:
                        while self.actions[depend].status == Action.RUNNING:
                            self.actions[depend].wait_for_completion()
                            status = check_action(self.actions[depend], status)
                        if not self.actions[depend].status in Action.COMPLETED:
                            action.status = Action.SKIPPED
                            self.summary.append("%s skipped: required %s has not been executed" % (action.name, depend))
                            break
                    if action.status != Action.SKIPPED:
                        # execute it
                        if action.isserver:
                            run_servers.append(action.name);
                        action.tryexecution(step, not self.nologs)
                        started.append(action)
            except Exception, inst:
                traceback.print_exc()
                self.summary.append("%s failed: %s" % (action.name, inst))

        # Now wait for each running action.
        for action in started:
            if action.status == Action.RUNNING:
                action.wait_for_completion()
            status = check_action(action, status)

        # append all parameters to summary
        self.summary.append("")
        self.summary.extend(sys.argv)

        # update summary
        s.write("%s\n" % ("\n".join(self.summary)))
        s.close()

        # copy information about sources
        for source in self.actions.keys():
            action = self.actions[source]
            basedir = getattr(action, 'basedir', None)
            if basedir and os.path.isdir(basedir):
                for file in os.listdir(os.path.join(basedir, "..")):
                    if fnmatch.fnmatch(file, source + '[.-]*'):
                        shutil.copyfile(os.path.join(basedir, "..", file),
                                        os.path.join(self.resultdir, file))

        # run testresult checker
        testdir = compile.testdir
        backenddir = os.path.join(compile.installdir, "usr/lib/syncevolution/backends")
        # resultchecker doesn't need valgrind, remove it if present
        shell = options.simpleshell
        if not shell:
            shell = options.shell
        shell = re.sub(r'\S*valgrind\S*', '', shell)
        # When using schroot, run it in /tmp, because the host's directory might
        # not exist in the chroot.
        shell = shell.replace('schroot ', 'schroot -d /tmp ', 1)
        prefix = re.sub(r'\S*valgrind\S*', '', options.testprefix)
        uri = self.uri or ("file:///" + self.resultdir)
        resultchecker = self.findTestFile("resultchecker.py")
        compare = self.findTestFile("compare.xsl")
        generateHTML = self.findTestFile("generate-html.xsl")
        commands = []

        # produce nightly.xml from plain text log files
        backenddir = context.stripSchrootDir(backenddir)
        testdir = context.stripSchrootDir(testdir)
        commands.append(resultchecker + " " +self.resultdir+" "+"\""+",".join(run_servers)+"\""+" "+uri +" "+testdir + " \"" + shell + " " + testprefix +" \""+" \"" +backenddir + "\"")
        previousxml = os.path.join(self.lastresultdir, "nightly.xml")

        if os.path.exists(previousxml):
            # compare current nightly.xml against previous file
            commands.append("xsltproc -o " + self.resultdir + "/cmp_result.xml --stringparam cmp_file " + previousxml + " " + compare + " " + self.resultdir + "/nightly.xml")

        # produce HTML with URLs relative to current directory of the nightly.html
        commands.append("xsltproc -o " + self.resultdir + "/nightly.html --stringparam url . --stringparam cmp_result_file " + self.resultdir + "/cmp_result.xml " + generateHTML + " "+ self.resultdir+"/nightly.xml")

        self.runCommand(" && ".join(commands))

        # report result by email
        server, body, writer = self.startEmail()
        if server:
            msg=''
            try:
                msg = open(self.resultdir + "/nightly.html").read()
            except IOError:
                msg = '''<html><body><h1>Error: No HTML report generated!</h1></body></html>\n'''
            # insert absolute URL into hrefs so that links can be opened directly in
            # the mail reader
            msg = re.sub(r'href="([a-zA-Z0-9./])',
                         'href="' + uri + r'/\1',
                         msg)
            writer.startbody("text/html;charset=ISO-8859-1").write(msg)
            self.finishEmail(server, body)
        else:
            log('%s\n', '\n'.join(self.summary))

        if status in Action.COMPLETED:
            sys.exit(0)
        else:
            sys.exit(1)

    def startEmail(self):
        if self.recipients:
            server = smtplib.SMTP(self.mailhost)
            body = StringIO.StringIO()
            writer = MimeWriter.MimeWriter (body)
            writer.addheader("From", self.sender)
            for recipient in self.recipients:
            	writer.addheader("To", recipient)
            writer.addheader("Subject", self.mailtitle + ": " + os.path.basename(self.resultdir))
            writer.addheader("MIME-Version", "1.0")
            writer.flushheaders()
            return (server, body, writer)
        else:
            return (None, None, None)

    def finishEmail(self, server, body):
        failed = server.sendmail(self.sender, self.recipients, body.getvalue())
        if failed:
            log('could not send to: %s', failed)
            sys.exit(1)

class CVSCheckout(Action):
    """Does a CVS checkout (if directory does not exist yet) or an update (if it does)."""
    
    def __init__(self, name, workdir, runner, cvsroot, module, revision):
        """workdir defines the directory to do the checkout in,
        cvsroot the server, module the path to the files,
        revision the tag to checkout"""
        Action.__init__(self,name)
        self.workdir = workdir
        self.runner = runner
        self.cvsroot = cvsroot
        self.module = module
        self.revision = revision
        self.basedir = os.path.join(abspath(workdir), module)

    def execute(self):
        cd(self.workdir)
        if os.access(self.module, os.F_OK):
            cd(self.module)
            context.runCommand("cvs update -d -r %s"  % (self.revision))
        elif self.revision == "HEAD":
            context.runCommand("cvs -d %s checkout %s" % (self.cvsroot, self.module))
            cd(self.module)
        else:
            context.runCommand("cvs -d %s checkout -r %s %s" % (self.cvsroot, self.revision, self.module))
            cd(self.module)
        if os.access("autogen.sh", os.F_OK):
            context.runCommand("%s ./autogen.sh" % (self.runner))

class SVNCheckout(Action):
    """Does a Subversion checkout (if directory does not exist yet) or a switch (if it does)."""
    
    def __init__(self, name, workdir, runner, url, module):
        """workdir defines the directory to do the checkout in,
        URL the server and path inside repository,
        module the path to the files in the checked out copy"""
        Action.__init__(self,name)
        self.workdir = workdir
        self.runner = runner
        self.url = url
        self.module = module
        self.basedir = os.path.join(abspath(workdir), module)

    def execute(self):
        cd(self.workdir)
        if os.access(self.module, os.F_OK):
            cmd = "switch"
        else:
            cmd = "checkout"
        context.runCommand("svn %s %s %s"  % (cmd, self.url, self.module))
        cd(self.module)
        if os.access("autogen.sh", os.F_OK):
            context.runCommand("%s ./autogen.sh" % (self.runner))

class GitCheckoutBase:
    """Just sets some common properties for all Git checkout classes: workdir, basedir"""

    def __init__(self, name, workdir):
        self.workdir = workdir
        self.basedir = os.path.join(abspath(workdir), name)

class GitCheckout(GitCheckoutBase, Action):
    """Does a git clone (if directory does not exist yet) or a fetch+checkout (if it does)."""

    def __init__(self, name, workdir, runner, url, revision):
        """workdir defines the directory to do the checkout in with 'name' as name of the sub directory,
        URL the server and repository,
        revision the desired branch or tag"""
        Action.__init__(self, name)
        GitCheckoutBase.__init__(self, name)
        self.runner = runner
        self.url = url
        self.revision = revision

    def execute(self):
        if os.access(self.basedir, os.F_OK):
            cmd = "cd %s && git fetch" % (self.basedir)
        else:
            cmd = "git clone %s %s && chmod -R g+w %s && cd %s && git config core.sharedRepository group " % (self.url, self.basedir, self.basedir, self.basedir)
        context.runCommand(cmd)
        context.runCommand("set -x; cd %(dir)s && git show-ref &&"
                           "((git tag -l | grep -w -q %(rev)s) && git checkout %(rev)s ||"
                           "((git branch -l | grep -w -q %(rev)s) && git checkout %(rev)s || git checkout -b %(rev)s origin/%(rev)s) && git merge origin/%(rev)s)" %
                           {"dir": self.basedir,
                            "rev": self.revision},
                           runAsIs=True)
        cd(self.basedir)
        if os.access("autogen.sh", os.F_OK):
            context.runCommand("%s ./autogen.sh" % (self.runner))

class GitCopy(GitCheckoutBase, Action):
    """Copy existing git repository and update it to the requested
    branch, with local changes stashed before updating and restored
    again afterwards. Automatically merges all branches with <branch>/
    as prefix, skips those which do not apply cleanly."""

    def __init__(self, name, workdir, runner, sourcedir, revision):
        """workdir defines the directory to create/update the repo in with 'name' as name of the sub directory,
        sourcedir a directory which must contain such a repo already,
        revision the desired branch or tag"""
        Action.__init__(self, name)
        GitCheckoutBase.__init__(self, name, workdir)
        self.runner = runner
        self.sourcedir = sourcedir
        self.revision = revision
        self.patchlog = os.path.join(abspath(workdir), name + "-source.log")

        self.__getitem__ = lambda x: getattr(self, x)

    def execute(self):
        if not os.access(self.basedir, os.F_OK):
            context.runCommand("(mkdir -p %s && cp -a -l %s/%s %s) || ( rm -rf %s && false )" %
                               (self.workdir, self.sourcedir, self.name, self.workdir, self.basedir))
        cd(self.basedir)
        cmd = " && ".join([
                'rm -f %(patchlog)s',
                'echo "save local changes with stash under a fixed name <rev>-nightly"',
                'rev=$(git stash create)',
                'git branch -f %(revision)s-nightly ${rev:-HEAD}',
                'echo "check out branch as "nightly" and integrate all proposed patches (= <revision>/... branches)"',
                # switch to detached head, to allow removal of branches
                'git checkout -q $( git show-ref --head --hash | head -1 )',
                'if git branch | grep -q -w "^..%(revision)s$"; then git branch -D %(revision)s; fi',
                'if git branch | grep -q -w "^..nightly$"; then git branch -D nightly; fi',
                # fetch
                'echo "remove stale merge branches and fetch anew"',
                'git branch -r -D $( git branch -r | grep -e "/for-%(revision)s/" ) ',
                'git branch -D $( git branch | grep -e "^  for-%(revision)s/" ) ',
                'git fetch',
                'git fetch --tags',
                # pick tag or remote branch
                'if git tag | grep -q -w %(revision)s; then base=%(revision)s; git checkout -f -b nightly %(revision)s; ' \
                    'else base=origin/%(revision)s; git checkout -f -b nightly origin/%(revision)s; fi',
                # integrate remote branches first, followed by local ones;
                # the hope is that local branches apply cleanly on top of the remote ones
                'for patch in $( (git branch -r --no-merged origin/%(revision)s; git branch --no-merged origin/%(revision)s) | sed -e "s/^..//" | grep -e "^for-%(revision)s/" -e "/for-%(revision)s/" ); do ' \
                    'if git merge $patch; then echo >>%(patchlog)s $patch: okay; ' \
                    'else echo >>%(patchlog)s $patch: failed to apply; git reset --hard; fi; done',
                'echo "restore <rev>-nightly and create permanent branch <rev>-nightly-before-<date>-<time> if that fails or new tree is different"',
                # only apply stash when really a stash
                'if ( git log -n 1 --oneline %(revision)s-nightly | grep -q " WIP on" && ! git stash apply %(revision)s-nightly ) || ! git diff --quiet %(revision)s-nightly..nightly; then ' \
                    'git branch %(revision)s-nightly-before-$(date +%%Y-%%m-%%d-%%H-%%M) %(revision)s-nightly; '
                    'fi',
                'echo "document local patches"',
                'rm -f ../%(name)s-*.patch',
                'git format-patch -o .. $base..nightly',
                '(cd ..; for i in [0-9]*.patch; do [ ! -f "$i" ] || mv $i %(name)s-$i; done)',
                'git describe --tags --always nightly | sed -e "s/\(.*\)-\([0-9][0-9]*\)-g\(.*\)/\\1 + \\2 commit(s) = \\3/" >>%(patchlog)s',
                '( git status | grep -q "working directory clean" && echo "working directory clean" || ( echo "working directory dirty" && ( echo From: nightly testing ; echo Subject: [PATCH 1/1] uncommitted changes ; echo ; git status; echo; git diff HEAD ) >../%(name)s-1000-unstaged.patch ) ) >>%(patchlog)s'
                ]) % self

        context.runCommand(cmd, dumpCommands=True, runAsIs=True)
        if os.access("autogen.sh", os.F_OK):
            context.runCommand("%s ./autogen.sh" % (self.runner))

class AutotoolsBuild(Action):
    def __init__(self, name, src, configargs, runner, dependencies):
        """Runs configure from the src directory with the given arguments.
        runner is a prefix for the configure command and can be used to setup the
        environment."""
        Action.__init__(self, name)
        self.src = src
        self.configargs = configargs
        self.runner = runner
        self.dependencies = dependencies
        self.installdir = os.path.join(context.tmpdir, "install")
        self.builddir = os.path.join(context.tmpdir, "build")
        self.testdir = os.path.join(self.builddir, "src")

    def execute(self):
        log('removing builddir: %s', self.builddir)
        del_dir(self.builddir)
        cd(self.builddir)
        context.runCommand("%s %s/configure %s" % (self.runner, self.src, self.configargs))
        # Before invoking make recursively, the parent must obtain
        # one job token. make then may allocate more.
        context.runCommand("%s %s install DESTDIR=%s" % (self.runner, context.make, self.installdir))


class SyncEvolutionTest(Action):
    def __init__(self, name, build, serverlogs, runner, tests, sources, testenv="", lineFilter=None, testPrefix="", serverName="", testBinary="./client-test"):
        """Execute TestEvolution for all (empty tests) or the
        selected tests."""
        Action.__init__(self, name)
        self.isserver = True
        self.build = build
        self.testdir = build.testdir
        self.serverlogs = serverlogs
        self.runner = runner
        self.tests = tests
        self.sources = sources
        self.testenv = testenv
        if build.name:
            self.dependencies.append(build.name)
        self.lineFilter = lineFilter
        self.testPrefix = testPrefix
        self.serverName = serverName
        if not self.serverName:
            self.serverName = name
        self.testBinary = testBinary
        self.alarmSeconds = 1200
        self.needhome = True

    def execute(self):
        resdir = os.getcwd()
        log('result dir: %s, /proc/self/cwd -> %s', resdir, os.readlink('/proc/self/cwd'))
        if resdir == '/':
            time.sleep(5)
            resdir = os.getcwd()
            log('result dir: %s, /proc/self/cwd -> %s', resdir, os.readlink('/proc/self/cwd'))
        # Run inside a new directory which links to all files in the build dir.
        # That way different actions are independent of each other while still
        # sharing the same test binaries and files.
        actiondir = os.path.join(context.tmpdir, 'tests', self.name)
        if not os.path.isdir(actiondir):
            os.makedirs(actiondir)
        # The symlinks must be usable inside a chroot, so
        # remove the chroot prefix that is only visible here
        # outside the chroot. For copying the original file,
        # we must remember the file name outside of the chroot.
        hosttargetdir = self.testdir
        targetdir = context.stripSchrootDir(hosttargetdir)
        links = {}
        for entry in os.listdir(self.testdir):
            if not entry.startswith('.'):
                target = os.path.join(targetdir, entry)
                name = os.path.join(actiondir, entry)
                os.symlink(target, name)
                links[entry] = os.path.join(hosttargetdir, entry)
        cd(actiondir)
        try:
            # use installed backends if available
            backenddir = os.path.join(self.build.installdir, "usr/lib/syncevolution/backends")
            if not os.access(backenddir, os.F_OK):
                # fallback: relative to client-test inside the current directory
                backenddir = "backends"

            # same with configs and templates, except that they use the source as fallback
            confdir = os.path.join(self.build.installdir, "usr/share/syncevolution/xml")
            if not os.access(confdir, os.F_OK):
                confdir = os.path.join(sync.basedir, "src/syncevo/configs")

            templatedir = os.path.join(self.build.installdir, "usr/share/syncevolution/templates")
            if not os.access(templatedir, os.F_OK):
                templatedir = os.path.join(sync.basedir, "src/templates")

            datadir = os.path.join(self.build.installdir, "usr/share/syncevolution")
            if not os.access(datadir, os.F_OK):
                # fallback works for bluetooth_products.ini but will fail for other files
                datadir = os.path.join(sync.basedir, "src/dbus/server")

            if self.build.installed:
                # No need for special env variables.
                installenv = ""
            else:
                installenv = \
                    "SYNCEVOLUTION_DATA_DIR=%s "\
                    "SYNCEVOLUTION_TEMPLATE_DIR=%s " \
                    "SYNCEVOLUTION_XML_CONFIG_DIR=%s " \
                    "SYNCEVOLUTION_BACKEND_DIR=%s " \
                    % ( datadir, templatedir, confdir, backenddir )

                # Translations have no fallback, they must be installed. Leave unset
                # if not found.
                localedir = os.path.join(self.build.installdir, "usr/share/locale")
                if os.access(localedir, os.F_OK):
                    installenv = installenv + \
                        ("SYNCEVOLUTION_LOCALE_DIR=%s " % localedir)

            cmd = "%s %s %s %s %s ./syncevolution" % (self.testenv, installenv, self.runner, context.setupcmd, self.name)
            context.runCommand(cmd, resources=[self.name])

            # proxy must be set in test config! Necessary because not all tests work with the env proxy (local CalDAV, for example).
            options = { "server": self.serverName,
                        "sources": ",".join(self.sources),
                        "alarm": self.alarmSeconds,
                        "env": self.testenv,
                        "installenv": installenv,
                        "log": self.serverlogs,
                        "evoprefix": context.databasePrefix,
                        "runner": self.runner,
                        "testbinary": self.testBinary,
                        "testprefix": self.testPrefix }
            basecmd = "http_proxy= " \
                      "CLIENT_TEST_SERVER=%(server)s " \
                      "CLIENT_TEST_SOURCES=%(sources)s " \
                      "SYNC_EVOLUTION_EVO_CALENDAR_DELAY=1 " \
                      "CLIENT_TEST_ALARM=%(alarm)d " \
                      "%(env)s %(installenv)s " \
                      "CLIENT_TEST_LOG=%(log)s " \
                      "CLIENT_TEST_EVOLUTION_PREFIX=%(evoprefix)s " \
                      "%(runner)s " \
                      % options
            additional = []
            for var, value in (('LD_LIBRARY_PATH', 'build-synthesis/src/.libs:.libs:syncevo/.libs:gdbus/.libs:gdbusxx/.libs:'),
                               ('PATH', 'backends/webdav:.:\\$PATH:')):
                if ' ' + var + '=' in basecmd:
                    # Prepend to existing assignment, instead of overwriting it
                    # as we would when appending another "env" invocation.
                    basecmd = basecmd.replace(' ' + var + '=', ' ' + var + '=' + value)
                else:
                    additional.append(var + '=' + value)
            if additional:
                basecmd = basecmd + 'env ' + ' '.join(additional)
            basecmd = basecmd +  (" %(testprefix)s %(testbinary)s" % options)
            enabled = context.enabled.get(self.name)
            if not enabled:
                enabled = self.tests
            enabled = re.split("[ ,]", enabled.strip()) 
            if enabled:
                tests = []
                for test in enabled:
                    if test == "Client::Sync" and context.sanitychecks:
                        # Replace with one simpler, faster testItems test, but be careful to
                        # pick an enabled source and the right mode (XML vs. WBXML).
                        # The first listed source and WBXML should be safe.
                        tests.append("Client::Sync::%s::testItems" % self.sources[0])
                    else:
                        tests.append(test)
                context.runCommand("%s %s" % (basecmd, " ".join(tests)),
                                   resources=[self.name])
            else:
                context.runCommand(basecmd,
                                   resources=[self.name])
        finally:
            tocopy = re.compile(r'.*\.log|.*\.client.[AB]|.*\.(cpp|h|c)\.html|.*\.log\.html')
            toconvert = re.compile(r'Client_.*\.log')
            htaccess = file(os.path.join(resdir, ".htaccess"), "a")
            for f in os.listdir(actiondir):
                if tocopy.match(f):
                    error = copyLog(f in links and links[f] or f, resdir, htaccess, self.lineFilter)
                    if toconvert.match(f):
                        # also convert client-test log files to HTML
                        tohtml = os.path.join(resdir, f + ".html")
                        synclog2html = os.path.join(self.build.installdir, "usr", "bin", "synclog2html")
                        if not os.access(synclog2html, os.F_OK):
                            synclog2html = os.path.join(sync.basedir, "src", "synclog2html")
                        os.system("%s %s >%s" % (synclog2html, f, tohtml))
                        basehtml = f + ".html"
                        if os.path.exists(basehtml):
                            os.unlink(basehtml)
                        os.symlink(tohtml, basehtml)
                        if error:
                            htaccess.write('AddDescription "%s" %s\n' % (error, basehtml))


###################################################################
# Configuration part
###################################################################

parser = optparse.OptionParser()
parser.add_option("-e", "--enable",
                  action="append", type="string", dest="enabled", default=[],
                  help="use this to enable specific actions instead of executing all of them (can be used multiple times and accepts enable=test1,test2 test3,... test lists)")
parser.add_option("-n", "--no-logs",
                  action="store_true", dest="nologs",
                  help="print to stdout/stderr directly instead of redirecting into log files")
parser.add_option("-l", "--list",
                  action="store_true", dest="list",
                  help="list all available actions")
parser.add_option("-s", "--skip",
                  action="append", type="string", dest="skip", default=[],
                  help="instead of executing this action assume that it completed earlier (can be used multiple times)")
parser.add_option("", "--tmp",
                  type="string", dest="tmpdir", default="",
                  help="temporary directory for intermediate files")
parser.add_option("", "--home-template", default=None,
                  help="Copied entirely to set up temporary home directories while running tests in parallel. Leaving this empty disables parallel testing.")
parser.add_option("", "--workdir",
                  type="string", dest="workdir", default=None,
                  help="directory for files which might be reused between runs")
parser.add_option("", "--database-prefix",
                  type="string", dest="databasePrefix", default="Test_",
                  help="defines database names (<prefix>_<type>_1/2), must exist")
parser.add_option("", "--resultdir",
                  type="string", dest="resultdir", default="",
                  help="directory for log files and results")
parser.add_option("", "--lastresultdir",
                  type="string", dest="lastresultdir", default="",
                  help="directory for last day's log files and results")
parser.add_option("", "--datadir",
                  type="string", dest="datadir", default=os.path.dirname(os.path.abspath(os.path.expanduser(os.path.expandvars(sys.argv[0])))),
                  help="directory for files used by report generation")
parser.add_option("", "--resulturi",
                  type="string", dest="uri", default=None,
                  help="URI that corresponds to --resultdir, if given this is used in mails instead of --resultdir")
parser.add_option("", "--shell",
                  type="string", dest="shell", default="",
                  help="a prefix which is put in front of a command to execute it (can be used for e.g. run_garnome)")
parser.add_option("", "--simple-shell",
                  type="string", dest="simpleshell", default="",
                  help="shell to use for result checking (just the environment, no daemons)")
parser.add_option("", "--schrootdir",
                  type="string", dest="schrootdir", default="",
                  help="the path to the root of the chroot when using schroot in --shell; --resultdir already includes the path")
parser.add_option("", "--test-prefix",
                  type="string", dest="testprefix", default="",
                  help="a prefix which is put in front of client-test (e.g. valgrind)")
parser.add_option("", "--sourcedir",
                  type="string", dest="sourcedir", default=None,
                  help="directory which contains 'syncevolution' and 'libsynthesis' code repositories; if given, those repositories will be used as starting point for testing instead of checking out directly")
parser.add_option("", "--cppcheck",
                  action="store_true", dest="cppcheck", default=False,
                  help="enable running of cppcheck on all source checkouts; only active with --no-sourcedir-copy")
parser.add_option("", "--no-sourcedir-copy",
                  action="store_true", dest="nosourcedircopy", default=False,
                  help="instead of copying the content of --sourcedir and integrating patches automatically, use the content directly")
parser.add_option("", "--sourcedir-copy",
                  action="store_false", dest="nosourcedircopy",
                  help="reverts a previous --no-sourcedir-copy")
parser.add_option("", "--syncevo-tag",
                  type="string", dest="syncevotag", default="master",
                  help="the tag of SyncEvolution (e.g. syncevolution-0.7, default is 'master'")
parser.add_option("", "--synthesis-tag",
                  type="string", dest="synthesistag", default="master",
                  help="the tag of the synthesis library (default = master in the moblin.org repo)")
parser.add_option("", "--activesyncd-tag",
                  type="string", dest="activesyncdtag", default="master",
                  help="the tag of the activesyncd (default = master)")
parser.add_option("", "--configure",
                  type="string", dest="configure", default="",
                  help="additional parameters for configure")
parser.add_option("", "--openembedded",
                  type="string", dest="oedir",
                  help="the build directory of the OpenEmbedded cross-compile environment")
parser.add_option("", "--host",
                  type="string", dest="host",
                  help="platform identifier like x86_64-linux; if this and --openembedded is set, then cross-compilation is tested")
parser.add_option("", "--bin-suffix",
                  type="string", dest="binsuffix", default="",
                  help="string to append to name of binary .tar.gz distribution archive (default empty = no binary distribution built)")
parser.add_option("", "--package-suffix",
                  type="string", dest="packagesuffix", default="",
                  help="string to insert into package name (default empty = no binary distribution built)")

parser.add_option("", "--synthesis",
                  type="string", dest="synthesisdir", default="",
                  help="directory with Synthesis installation")
parser.add_option("", "--funambol",
                  type="string", dest="funamboldir", default="/scratch/Funambol",
                  help="directory with Funambol installation")
parser.add_option("", "--from",
                  type="string", dest="sender",
                  help="sender of email if recipients are also specified")
parser.add_option("", "--to",
                  action="append", type="string", dest="recipients",
                  help="recipient of result email (option can be given multiple times)")
parser.add_option("", "--mailhost",
                  type="string", dest="mailhost", default="localhost",
                  help="SMTP mail server to be used for outgoing mail")
parser.add_option("", "--subject",
                  type="string", dest="subject", default="SyncML Tests " + time.strftime("%Y-%m-%d %H-%M"),
                  help="subject of result email (default is \"SyncML Tests <date> <time>\"")
parser.add_option("", "--evosvn",
                  action="append", type="string", dest="evosvn", default=[],
                  help="<name>=<path>: compiles Evolution from source under a short name, using Paul Smith's Makefile and config as found in <path>")
parser.add_option("", "--prebuilt",
                  action="store", type="string", dest="prebuilt", default=None,
                  help="a directory where SyncEvolution was build before: enables testing using those binaries (can be used once, instead of compiling)")
parser.add_option("", "--setup-command",
                  type="string", dest="setupcmd",
                  help="invoked with <test name> <args to start syncevolution>, should setup local account for the test")
parser.add_option("", "--make-command",
                  type="string", dest="makecmd", default="nice make",
                  help="command to use instead of plain make, for example 'make -j'")
parser.add_option("", "--sanity-checks",
                  action="store_true", dest="sanitychecks", default=False,
                  help="run limited number of sanity checks instead of full set")

(options, args) = parser.parse_args()
if options.recipients and not options.sender:
    log('sending email also requires sender argument')
    sys.exit(1)

# accept --enable foo[=args]
enabled = {}
for option in options.enabled:
    l = option.split("=", 1)
    if len(l) == 2:
        enabled[l[0]] = l[1]
    else:
        enabled[option] = None

context = Context(options.tmpdir, options.resultdir, options.uri, options.workdir,
                  options.subject, options.sender, options.recipients, options.mailhost,
                  enabled, options.skip, options.nologs, options.setupcmd,
                  options.makecmd, options.sanitychecks, options.lastresultdir, options.datadir)
context.databasePrefix = options.databasePrefix
context.home_template = options.home_template
context.schrootdir = options.schrootdir

class EvoSvn(Action):
    """Builds Evolution from SVN using Paul Smith's Evolution Makefile."""
    
    def __init__(self, name, workdir, resultdir, makedir, makeoptions):
        """workdir defines the directory to do the build in,
        makedir is the directory which contains the Makefile and its local.mk,
        makeoptions contain additional parameters for make (like BRANCH=2.20 PREFIX=/tmp/runtests/evo)."""
        Action.__init__(self,name)
        self.workdir = workdir
	self.resultdir = resultdir
        self.makedir = makedir
        self.makeoptions = makeoptions

    def execute(self):
        cd(self.workdir)
        shutil.copy2(os.path.join(self.makedir, "Makefile"), ".")
        shutil.copy2(os.path.join(self.makedir, "local.mk"), ".")
        if os.access(self.resultdir, os.F_OK):
            shutil.rmtree(self.resultdir)
        os.system("rm -f .stamp/*.install")
	localmk = open("local.mk", "a")
	localmk.write("PREFIX := %s\n" % self.resultdir)
	localmk.close()
        if os.access(".stamp", os.F_OK):
            context.runCommand("make check-changelog")
        context.runCommand("%s %s" % (context.make, self.makeoptions))

for evosvn in options.evosvn:
    name, path = evosvn.split("=")
    evosvn = EvoSvn("evolution" + name,
                    os.path.join(options.tmpdir, "evolution%s-build" % name),
		    os.path.join(options.tmpdir, "evolution%s-result" % name),
                    path,
                    "SUDO=true")
    context.add(evosvn)

class SyncEvolutionCheckout(GitCheckout):
    def __init__(self, name, revision):
        """checkout SyncEvolution"""
        GitCheckout.__init__(self,
                             name, context.workdir,
                             # parameter to autogen.sh in SyncEvolution: also
                             # check for clean Synthesis source
                             "SYNTHESISSRC=../libsynthesis %s" % options.shell,
                             "git@gitorious.org:meego-middleware/syncevolution.git",
                             revision)

class SynthesisCheckout(GitCheckout):
    def __init__(self, name, revision):
        """checkout libsynthesis"""
        GitCheckout.__init__(self,
                             name, context.workdir, options.shell,
                             "git@gitorious.org:meego-middleware/libsynthesis.git",
                             revision)

class ActiveSyncDCheckout(GitCheckout):
    def __init__(self, name, revision):
        """checkout activesyncd"""
        GitCheckout.__init__(self,
                             name, context.workdir, options.shell,
                             "git://git.gnome.org/evolution-activesync",
                             revision)

class SyncEvolutionBuild(AutotoolsBuild):
    def execute(self):
        AutotoolsBuild.execute(self)
        # LDFLAGS=-no-install is needed to ensure that the resulting
        # client-test is a normal, usable executable. Otherwise we
        # can have the following situation:
        # - A wrapper script is created on the reference platform.
        # - It is never executed there, which means that it won't
        #   produce the final .libs/lt-client-test executable
        #   (done on demand by libtool wrapper).
        # - The wrapper script is invokved for the first time
        #   on some other platform, it tries to link, but fails
        #   because libs are different.
        context.runCommand("%s %s src/client-test CXXFLAGS='-O0 -g' ADDITIONAL_LDFLAGS=-no-install" % (self.runner, context.make))

class NopAction(Action):
    def __init__(self, name):
        Action.__init__(self, name)
        self.status = Action.DONE
        self.execute = self.nop
        self.numjobs = 0

class NopSource(GitCheckoutBase, NopAction):
    def __init__(self, name, sourcedir):
        NopAction.__init__(self, name)
        GitCheckoutBase.__init__(self, name, sourcedir)

class CppcheckSource(GitCheckoutBase, Action):
    def __init__(self, name, sourcedir, cppcheckflags):
        Action.__init__(self, name)
        GitCheckoutBase.__init__(self, name, sourcedir)
        self.cppcheckflags = cppcheckflags
        # During normal, parallel testing we want to parallelize
        # by running other things besides cppcheck, because that
        # makes better use of the CPUs. Allocating a large number
        # of jobs for cppcheck blocks using them for a certain
        # period until enough CPUs are free. This can be overriden
        # with an env variable.
        self.numjobs = int(os.environ.get("RUNTESTS_CPPCHECK_JOBS", "4"))
        self.sources = self.basedir

    def execute(self):
        context.runCommand("%s %s --force -j%d %s %s" % (options.shell,
                                                         os.path.join(sync.basedir,
                                                                      "test",
                                                                      "cppcheck-wrapper.sh"),
                                                         self.numjobs,
                                                         self.cppcheckflags,
                                                         self.sources),
                           jobs=self.numjobs)

if options.sourcedir:
    if options.nosourcedircopy:
        if options.cppcheck:
            # Checking libsynthesis must avoid define combinations
            # which are invalid.  We cannot exclude invalid define
            # combinations specifically, so we have to limit the set
            # of combinations by setting or unsetting single defines.
            # We focus on the Linux port here.
            libsynthesis = CppcheckSource("libsynthesis", options.sourcedir,
                                          " ".join([ "-i %s/%s" % (options.sourcedir, x) for x in
                                                     [
                                                       # No files need to be excluded at the moment.
                                                       ]
                                                     ] +
                                                   [ "-USYSYNC_TOOL",
                                                     "-U__EPOC_OS__",
                                                     "-U__MC68K__",
                                                     "-U__MWERKS__",
                                                     "-U__PALM_OS__",
                                                     "-D__GNUC__",
                                                     "-D__cplusplus",
                                                     "-UEXPIRES_AFTER_DAYS",
                                                     "-USYSER_REGISTRATION",
                                                     "-UEXPIRES_AFTER_DATE",
                                                     "-UODBC_SUPPORT", # obsolete
                                                     "-DSQLITE_SUPPORT", # enabled on Linux
                                                     "-DLINUX",
                                                     "-DNOWSM",
                                                     "-DENGINEINTERFACE_SUPPORT",
                                                     "-UDIRECT_APPBASE_GLOBALACCESS",
                                                     "-DUSE_SML_EVALUATION",
                                                     "-DDESKTOP_CLIENT",
                                                     "-DCOPY_SEND",
                                                     "-DCOPY_RECEIVE",
                                                     "-DSYSYNC_CLIENT",
                                                     "-DSYSYNC_SERVER",
                                                     "-DENGINE_LIBRARY",
                                                     "-DCHANGEDETECTION_AVAILABLE",
                                                     "-UHARDCODED_TYPE_SUPPORT",
                                                     "-UHARD_CODED_SERVER_URI",
                                                     "-UAUTOSYNC_SUPPORT",
                                                     "-UBINFILE_ALWAYS_ACTIVE",
                                                     "-DOBJECT_FILTERING",
                                                     "-DCLIENTFEATURES_2008",
                                                     "-DENHANCED_PROFILES_2004", # binfileimplds.h:395: error: #error "non-enhanced profiles and profile version <6 no longer supported!"
                                                     "-UMEMORY_PROFILING", # linux/profiling.cpp:26: error: #error "No memory  profiling for linux yet"
                                                     "-UTIME_PROFILING", # linux/profiling.cpp:19: error: #error "No time profiling for linux yet"
                                                     "-UNUMERIC_LOCALIDS",

                                                     # http://sourceforge.net/apps/trac/cppcheck/ticket/5316:
                                                     # Happens with cppcheck 1.61: Analysis failed. If the code is valid then please report this failure.
                                                     "--suppress=cppcheckError:*/localengineds.cpp",
                                                     # We use inline suppressions for some errors.
                                                     '--inline-suppr',
                                                     ]))
            # Be more specific about which sources we check. We are not interested in
            # pcre and expat, for example.
            libsynthesis.sources = " ".join("%s/src/%s" % (libsynthesis.sources, x) for x in
                                            "sysync DB_interfaces sysync_SDK/Sources Transport_interfaces/engine platform_adapters".split())
        else:
            libsynthesis = NopSource("libsynthesis", options.sourcedir)
    else:
        libsynthesis = GitCopy("libsynthesis",
                               options.workdir,
                               options.shell,
                               options.sourcedir,
                               options.synthesistag)
else:
    libsynthesis = SynthesisCheckout("libsynthesis", options.synthesistag)
context.add(libsynthesis)

if options.sourcedir:
    if options.nosourcedircopy:
        if options.cppcheck:
            activesyncd = CppcheckSource("activesyncd", options.sourcedir,
                                         # Several (all?) of the GObject priv pointer accesses
                                         # trigger a 'Possible null pointer dereference: priv'
                                         # error. We could add inline suppressions, but that's
                                         # a bit intrusive, so let's be more lenient for activesyncd
                                         # and ignore the error altogether.
                                         "--suppress=nullPointer")
        else:
            activesyncd = NopSource("activesyncd", options.sourcedir)
    else:
        activesyncd = GitCopy("activesyncd",
                              options.workdir,
                              options.shell,
                              options.sourcedir,
                              options.activesyncdtag)
else:
    activesyncd = ActiveSyncDCheckout("activesyncd", options.activesyncdtag)
context.add(activesyncd)

if options.sourcedir:
    if options.nosourcedircopy:
        if options.cppcheck:
            sync = CppcheckSource("syncevolution", options.sourcedir,
                                  "--enable=warning,performance,portability --inline-suppr")
        else:
            sync = NopSource("syncevolution", options.sourcedir)
    else:
        sync = GitCopy("syncevolution",
                       options.workdir,
                       "SYNTHESISSRC=%s %s" % (libsynthesis.basedir, options.shell),
                       options.sourcedir,
                       options.syncevotag)
else:
    sync = SyncEvolutionCheckout("syncevolution", options.syncevotag)
context.add(sync)
source = []
if options.synthesistag:
    source.append("--with-synthesis-src=%s" % libsynthesis.basedir)
if options.activesyncdtag:
    source.append("--with-activesyncd-src=%s" % activesyncd.basedir)

class InstallPackage(Action):
    def __init__(self, name, package, runner):
        """Runs configure from the src directory with the given arguments.
        runner is a prefix for the configure command and can be used to setup the
        environment."""
        Action.__init__(self, name)
        self.package = package
        self.runner = runner

    def execute(self):
        # Assume .deb file(s) here.
        if self.package == '':
            raise Exception('No prebuilt packages available. Compilation failed?')
        context.runCommand("%s env PATH=/sbin:/usr/sbin:$PATH fakeroot dpkg -i %s" % (self.runner, self.package))

# determine where binaries come from:
# either compile anew or prebuilt
if options.prebuilt != None:
    if os.path.isdir(options.prebuilt):
        # Use build directory. Relies on bind mounting in chroots such
        # that all platforms see the same file system (paths and
        # content).
        compile = NopAction("compile")
        # For running tests.
        compile.testdir = os.path.join(options.prebuilt, "src")
        # For "make testclean".
        compile.builddir = options.prebuilt
        # For runtime paths.
        compile.installdir = os.path.join(options.prebuilt, "../install")
        compile.installed = False
    else:
        # Use dist package(s). Copy them first into our own work directory,
        # in case that runtest.py has access to it outside of a chroot but not
        # the dpkg inside it.
        pkgs = []
        for pkg in options.prebuilt.split():
            shutil.copy(pkg, context.workdir)
            pkgs.append(os.path.join(context.workdir, os.path.basename(pkg)))
        compile = InstallPackage("compile", ' '.join(pkgs), options.shell)
        compile.testdir = os.path.join(options.schrootdir, "usr", "lib", "syncevolution", "test")
        compile.builddir = compile.testdir
        compile.installdir = options.schrootdir
        compile.installed = True
else:
    if enabled.get("compile", None) == "no-tests":
        # Regular build.
        build = AutotoolsBuild
    else:
        # Also build client-test.
        build = SyncEvolutionBuild
    compile = build("compile",
                    sync.basedir,
                    "%s %s" % (options.configure, " ".join(source)),
                    options.shell,
                    [ libsynthesis.name, sync.name ])
    compile.installed = False

context.add(compile)

class SyncEvolutionCross(AutotoolsBuild):
    def __init__(self, syncevosrc, synthesissrc, host, oedir, dependencies):
        """cross-compile SyncEvolution using a certain OpenEmbedded build dir:
        host is the platform identifier (e.g. x86_64-linux),
        oedir must contain the 'tmp/cross' and 'tmp/staging/<host>' directories"""
        if synthesissrc:
            synthesis_source = "--with-funambol-src=%s" % synthesissrc
        else:
            synthesis_source = ""
        AutotoolsBuild.__init__(self, "cross-compile", syncevosrc, \
                                "--host=%s %s CPPFLAGS=-I%s/tmp/staging/%s/include/ LDFLAGS='-Wl,-rpath-link=%s/tmp/staging/%s/lib/ -Wl,--allow-shlib-undefined'" % \
                                ( host, synthesis_source, oedir, host, oedir, host ), \
                                "PKG_CONFIG_PATH=%s/tmp/staging/%s/share/pkgconfig PATH=%s/tmp/cross/bin:$PATH" % \
                                ( oedir, host, oedir ),
                                dependencies)
        self.builddir = os.path.join(context.tmpdir, host)
        self.testdir = os.path.join(self.builddir, "src")

    def execute(self):
        AutotoolsBuild.execute(self)

if options.oedir and options.host:
    cross = SyncEvolutionCross(sync.basedir, libsynthesis.basedir, options.host, options.oedir, [ libsynthesis.name, sync.name, compile.name ])
    context.add(cross)

class SyncEvolutionDist(AutotoolsBuild):
    def __init__(self, name, binsuffix, packagesuffix, binrunner, dependencies):
        """Builds a normal and a binary distribution archive in a directory where
        SyncEvolution was configured and compiled before.
        """
        AutotoolsBuild.__init__(self, name, "", "", binrunner, dependencies)
        self.binsuffix = binsuffix
        self.packagesuffix = packagesuffix
        
    def execute(self):
        cd(self.builddir)
        if self.packagesuffix:
            context.runCommand("%s %s BINSUFFIX=%s deb rpm" % (self.runner, context.make, self.packagesuffix))
	    put, get = os.popen4("%s dpkg-architecture -qDEB_HOST_ARCH" % (self.runner))
	    for arch in get.readlines():
	           if "i386" in arch:
		   	context.runCommand("%s %s BINSUFFIX=%s PKGARCH=lpia deb" % (self.runner, context.make, self.packagesuffix))
			break
        if self.binsuffix:
            context.runCommand("%s %s BINSUFFIX=%s distbin" % (self.runner, context.make, self.binsuffix))

dist = SyncEvolutionDist("dist",
                         options.binsuffix,
                         options.packagesuffix,
                         options.shell,
                         [ compile.name ])
context.add(dist)

class SyncEvolutionDistcheck(AutotoolsBuild):
    def __init__(self, name, binrunner, dependencies):
        """Does 'distcheck' in a directory where SyncEvolution was configured and compiled before."""
        AutotoolsBuild.__init__(self, name, "", "", binrunner, dependencies)

    def execute(self):
        cd(self.builddir)
        if enabled["distcheck"] == None:
            context.runCommand("%s %s distcheck" % (self.runner, context.make))
            context.runCommand("%s %s DISTCHECK_CONFIGURE_FLAGS=--enable-gui distcheck" % (self.runner, context.make))
            context.runCommand("%s %s 'DISTCHECK_CONFIGURE_FLAGS=--disable-ecal --disable-ebook' distcheck" % (self.runner, context.make))
        else:
            context.runCommand("%s %s 'DISTCHECK_CONFIGURE_FLAGS=%s' distcheck" % (self.runner, context.make, enabled["dist"]))

distcheck = SyncEvolutionDistcheck("distcheck",
                                   options.shell,
                                   [ compile.name ])
context.add(distcheck)

# Special case "evolution": this used to be a catch-all for all
# Client::Source and unit tests in the "SyncEvolution" test group.
# In practice it was always run with specific sources enabled.
#
# Now runtests.py has separate test runs for all of these but continues
# to use --enable evolution=... This is done by mapping the enabled["evolution"]
# value into the new categories (kde, eds, file, unittests).
# The advantage is parallel testing and some separation between running incompatible
# sources in the same process.
#
# Akonadi is known to crash randomly when used after EDS in the same
# process (from Client::Source::kde_contact::testOpen):
#
# [DEBUG 00:00:00] ClientTest.cpp:1004: starting source->open()
# [ERROR 00:20:00] stderr: syncevolution(787)/libakonadi Akonadi::SessionPrivate::socketError: Socket error occurred: "QLocalSocket::connectToServer: Invalid name"
# [DEVELOPER 00:20:00] stderr: QDBusConnection: session D-Bus connection created before QCoreApplication. Application may misbehave.
# [DEVELOPER 00:20:00] stderr: kres-migrator: cannot connect to X server
# [DEVELOPER 00:20:00] stderr: QDBusConnection: session D-Bus connection created before QCoreApplication. Application may misbehave.
# [DEVELOPER 00:20:00] stderr: kres-migrator: cannot connect to X server
# [DEVELOPER 00:20:00] stderr: Qt has caught an exception thrown from an event handler. Throwing
# [DEVELOPER 00:20:00] stderr: exceptions from an event handler is not supported in Qt. You must
# [DEVELOPER 00:20:00] stderr: reimplement QApplication::notify() and catch all exceptions there.

localtests = []

test = SyncEvolutionTest("eds", compile,
                         "", options.shell,
                         "Client::Source::eds_contact Client::Source::eds_event Client::Source::eds_task Client::Source::eds_memo ",
                         [],
                         "CLIENT_TEST_FAILURES="
                         " "
                         "CLIENT_TEST_SKIP="
                         " "
                         ,
                         testPrefix=options.testprefix)
localtests.append(test)
context.add(test)

test = SyncEvolutionTest("kde", compile,
                         "", options.shell,
                         "Client::Source::kde_contact Client::Source::kde_event Client::Source::kde_task Client::Source::kde_memo",
                         [],
                         "CLIENT_TEST_FAILURES="
                         # testReadItem404 works with some Akonadi versions (Ubuntu Lucid),
                         # but not all (Debian Testing). The other tests always fail,
                         # the code needs to be fixed.
                         "Client::Source::kde_.*::testReadItem404,"
                         "Client::Source::kde_.*::testDelete404,"
                         "Client::Source::kde_.*::testLinkedItems.*404,"
                         "Client::Source::kde_.*::testImport.*,"
                         "Client::Source::kde_.*::testRemoveProperties,"
                         " "
                         "CLIENT_TEST_SKIP="
                         " "
                         ,
                         testPrefix=options.testprefix)
localtests.append(test)
context.add(test)

test = SyncEvolutionTest("file", compile,
                         "", options.shell,
                         "Client::Source::file_contact Client::Source::file_event Client::Source::file_task Client::Source::file_memo",
                         [],
                         "CLIENT_TEST_FAILURES="
                         " "
                         "CLIENT_TEST_SKIP="
                         "Client::Source::file_event::LinkedItemsDefault::testLinkedItemsInsertBothUpdateChildNoIDs,"
                         "Client::Source::file_event::LinkedItemsDefault::testLinkedItemsUpdateChildNoIDs,"
                         "Client::Source::file_event::LinkedItemsWithVALARM::testLinkedItemsInsertBothUpdateChildNoIDs,"
                         "Client::Source::file_event::LinkedItemsWithVALARM::testLinkedItemsUpdateChildNoIDs,"
                         "Client::Source::file_event::LinkedItemsAllDay::testLinkedItemsInsertBothUpdateChildNoIDs,"
                         "Client::Source::file_event::LinkedItemsAllDay::testLinkedItemsUpdateChildNoIDs,"
                         "Client::Source::file_event::LinkedItemsNoTZ::testLinkedItemsInsertBothUpdateChildNoIDs,"
                         "Client::Source::file_event::LinkedItemsNoTZ::testLinkedItemsUpdateChildNoIDs"
                         " "
                         ,
                         testPrefix=options.testprefix)
localtests.append(test)
context.add(test)

test = SyncEvolutionTest("unittests", compile,
                         "", options.shell,
                         "SyncEvolution",
                         [],
                         "CLIENT_TEST_FAILURES="
                         " "
                         "CLIENT_TEST_SKIP="
                         " "
                         ,
                         testPrefix=options.testprefix)
localtests.append(test)
context.add(test)

# Implement the mapping from "evolution" to the new test names.
if enabled.has_key("evolution"):
    if enabled["evolution"] is None:
        # Everything is enabled.
        for test in localtests:
            enable[test.name] = None
    else:
        # Specific tests are enabled.
        evolution = enabled["evolution"].split(",")
        localtestsEnabled = {}
        for e in evolution:
            if e:
                for localtest in localtests:
                    # Match "Client:source::eds_contact::testImport" against
                    # "Client::source::eds_contact Client::source::eds_event ...".
                    for defTest in localtest.tests.split():
                        if defTest.startswith(e):
                            localtestsEnabled.setdefault(localtest.name, []).append(e)
                            break
        for name, e in localtestsEnabled.iteritems():
            enabled[name] = ','.join(e)

# test-dbus.py itself doesn't need to run under valgrind, remove it...
shell = re.sub(r'\S*valgrind\S*', '', options.shell)
testprefix = re.sub(r'\S*valgrind\S*', '', options.testprefix)
dbustest = SyncEvolutionTest("dbus", compile,
                             "", shell,
                             "",
                             [],
                             # ... but syncevo-dbus-server started by test-dbus.py should use valgrind
                             testenv="TEST_DBUS_PREFIX='%s'" % options.testprefix,
                             testPrefix=testprefix,
                             testBinary=os.path.join(sync.basedir,
                                                     "test",
                                                     "test-dbus.py -v"))
context.add(dbustest)
pimtest = SyncEvolutionTest("pim", compile,
                            "", shell,
                            "",
                            [],
                            # ... but syncevo-dbus-server started by test-dbus.py should use valgrind
                            testenv="TEST_DBUS_PREFIX='%s'" % options.testprefix,
                            testPrefix=testprefix,
                            testBinary=os.path.join(sync.basedir,
                                                    "src",
                                                    "dbus",
                                                    "server",
                                                    "pim",
                                                    "testpim.py -v"))
context.add(pimtest)

test = SyncEvolutionTest("googlecalendar", compile,
                         "", options.shell,
                         "Client::Sync::eds_event::testItems Client::Source::google_caldav",
                         [ "google_caldav", "eds_event" ],
                         "CLIENT_TEST_WEBDAV='google caldav testcases=testcases/google_event.ics' "
                         "CLIENT_TEST_NUM_ITEMS=10 " # don't stress server
                         "CLIENT_TEST_SIMPLE_UID=1 " # server gets confused by UID with special characters
                         "CLIENT_TEST_UNIQUE_UID=2 " # server keeps backups and complains with 409 about not increasing SEQUENCE number even after deleting old data
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         "CLIENT_TEST_FAILURES="
                         # http://code.google.com/p/google-caldav-issues/issues/detail?id=61 "cannot remove detached recurrence"
                         "Client::Source::google_caldav::LinkedItemsDefault::testLinkedItemsRemoveNormal,"
                         "Client::Source::google_caldav::LinkedItemsNoTZ::testLinkedItemsRemoveNormal,"
                         "Client::Source::google_caldav::LinkedItemsWithVALARM::testLinkedItemsRemoveNormal,"
                         "Client::Source::google_caldav::LinkedItemsAllDayGoogle::testLinkedItemsRemoveNormal,"
                         ,
                         testPrefix=options.testprefix)
context.add(test)

test = SyncEvolutionTest("googlecontacts", compile,
                         "", options.shell,
                         "Client::Sync::eds_contact::testItems "
                         "Client::Sync::eds_contact::testDownload "
                         "Client::Sync::eds_contact::testUpload "
                         "Client::Sync::eds_contact::testUpdateLocalWins "
                         "Client::Sync::eds_contact::testUpdateRemoteWins "
                         "Client::Source::google_carddav",
                         [ "google_carddav", "eds_contact" ],
                         "CLIENT_TEST_WEBDAV='google carddav' "
                         "CLIENT_TEST_NUM_ITEMS=10 " # don't stress server
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         "CLIENT_TEST_FAILURES="
                         ,
                         testPrefix=options.testprefix)
context.add(test)

test = SyncEvolutionTest("owndrive", compile,
                         "", options.shell,
                         "Client::Sync::eds_contact::testItems Client::Sync::eds_event::testItems Client::Source::owndrive_caldav Client::Source::owndrive_carddav",
                         [ "owndrive_caldav", "owndrive_carddav", "eds_event", "eds_contact" ],
                         "CLIENT_TEST_WEBDAV='owndrive caldav carddav' "
                         "CLIENT_TEST_NUM_ITEMS=10 " # don't stress server
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         ,
                         testPrefix=options.testprefix)
context.add(test)

test = SyncEvolutionTest("yahoo", compile,
                         "", options.shell,
                         "Client::Sync::eds_contact::testItems Client::Sync::eds_event::testItems Client::Source::yahoo_caldav Client::Source::yahoo_carddav",
                         [ "yahoo_caldav", "yahoo_carddav", "eds_event", "eds_contact" ],
                         "CLIENT_TEST_WEBDAV='yahoo caldav carddav carddav/testcases=testcases/yahoo_contact.vcf' "
                         "CLIENT_TEST_NUM_ITEMS=10 " # don't stress server
                         "CLIENT_TEST_SIMPLE_UID=1 " # server gets confused by UID with special characters
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         ,
                         testPrefix=options.testprefix)
context.add(test)

test = SyncEvolutionTest("oracle", compile,
                         "", options.shell,
                         "Client::Sync::eds_contact::testItems Client::Sync::eds_event::testItems Client::Source::oracle_caldav Client::Source::oracle_carddav",
                         [ "oracle_caldav", "oracle_carddav", "eds_event", "eds_contact" ],
                         "CLIENT_TEST_WEBDAV='oracle caldav carddav' "
                         "CLIENT_TEST_NUM_ITEMS=10 " # don't stress server
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         ,
                         testPrefix=options.testprefix)
context.add(test)

test = SyncEvolutionTest("egroupware-dav", compile,
                         "", options.shell,
                         "Client::Sync::eds_contact::testItems Client::Sync::eds_event::testItems Client::Source::egroupware-dav_caldav Client::Source::egroupware-dav_carddav",
                         [ "egroupware-dav_caldav", "egroupware-dav_carddav", "eds_event", "eds_contact" ],
                         "CLIENT_TEST_WEBDAV='egroupware-dav caldav carddav' "
                         "CLIENT_TEST_NUM_ITEMS=10 " # don't stress server
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         ,
                         testPrefix=options.testprefix)
context.add(test)

test = SyncEvolutionTest("davical", compile,
                         "", options.shell,
                         "Client::Sync::eds_contact Client::Sync::eds_event Client::Sync::eds_task Client::Source::davical_caldav Client::Source::davical_caldavtodo Client::Source::davical_carddav",
                         [ "davical_caldav", "davical_caldavtodo", "davical_carddav", "eds_event", "eds_task", "eds_contact" ],
                         "CLIENT_TEST_WEBDAV='davical caldav caldavtodo carddav' "
                         "CLIENT_TEST_NUM_ITEMS=10 " # don't stress server
                         "CLIENT_TEST_SIMPLE_UID=1 " # server gets confused by UID with special characters
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         ,
                         testPrefix=options.testprefix)
context.add(test)

test = SyncEvolutionTest("apple", compile,
                         "", options.shell,
                         "Client::Sync::eds_event Client::Sync::eds_task Client::Sync::eds_contact Client::Source::apple_caldav Client::Source::apple_caldavtodo Client::Source::apple_carddav",
                         [ "apple_caldav", "apple_caldavtodo", "apple_carddav", "eds_event", "eds_task", "eds_contact" ],
                         "CLIENT_TEST_WEBDAV='apple caldav caldavtodo carddav' "
                         "CLIENT_TEST_NUM_ITEMS=100 " # test is local, so we can afford a higher number;
                         # used to be 250, but with valgrind that led to runtimes of over 40 minutes in testManyItems (too long!)
                         "CLIENT_TEST_FAILURES="
                         # After introducing POST, a misbehavior (?) of the
                         # server started breaking the test:
                         # - POST returns a certain etag "foo" in send.client.A
                         # - the server seems to reorder properties, leading to etag "bar"
                         # - in check.client.A, because of "foo" != "bar", the item gets
                         #   downloaded and updated in a sync where no such update is
                         #   expected.
                         #
                         # Related to https://bugs.freedesktop.org/show_bug.cgi?id=63882 "WebDAV: re-import uploaded item".
                         # However, it is uncertain whether the server really
                         # behaves correctly, because the client cannot detect
                         # that the item is still getting modified by the server.
                         "Client::Sync::eds_contact::testOneWayFromLocal,"
                         "Client::Sync::eds_contact::testOneWayFromClient,"
                         "Client::Sync::eds_task::testOneWayFromLocal,"
                         "Client::Sync::eds_task::testOneWayFromClient,"
                         " "
                         # Apple Calendar Server 5.2 (and earlier?)
                         # implement timezones by reference and does
                         # not return VTIMEZONE definitions (see
                         # "Apple Calendar Server 5.2 + timezone by
                         # reference" on the caldeveloper mailing
                         # list). Ignore timezone related test failures.
                         "CLIENT_TEST_NO_TIMEZONES=1 "
                         "CLIENT_TEST_MODE=server " # for Client::Sync
                         ,
                         testPrefix=options.testprefix)
# but even with a local server does the test run a long time
test.alarmSeconds = 2400
context.add(test)

class ActiveSyncTest(SyncEvolutionTest):
    def __init__(self, name, sources = [ "eas_event", "eas_contact", "eds_event", "eds_contact" ],
                 env = "",
                 knownFailures = []):
        tests = []
        if "eds_event" in sources:
            tests.append("Client::Sync::eds_event")
        if "eds_contact" in sources:
            tests.append("Client::Sync::eds_contact")
        if "eas_event" in sources:
            tests.append("Client::Source::eas_event")
        if "eas_contact" in sources:
            tests.append("Client::Source::eas_contact")

        # Find activesyncd. It doesn't exist anywhere yet, but will be
        # created during compile. We have to predict the location here.
        if compile.installed:
            self.activesyncd = os.path.join(compile.installdir, "usr", "libexec", "activesyncd")
        else:
            self.activesyncd =  os.path.join(compile.builddir, "src", "backends", "activesync", "activesyncd", "install", "libexec", "activesyncd")

        SyncEvolutionTest.__init__(self, name,
                                   compile,
                                   "", options.shell,
                                   tests,
                                   sources,
                                   env +
                                   "CLIENT_TEST_NUM_ITEMS=10 "
                                   "CLIENT_TEST_MODE=server " # for Client::Sync
                                   "EAS_SOUP_LOGGER=1 "
                                   "EAS_DEBUG=5 "
                                   "EAS_DEBUG_DETACHED_RECURRENCES=1 "

                                   "CLIENT_TEST_FAILURES=" +
                                   ",".join(knownFailures +
                                            # time zone mismatch between client and server,
                                            # still need to investigate
                                            [ ".*::LinkedItemsWeekly::testSubsetStart11Skip[0-3]",
                                              ".*::LinkedItemsWeekly::testSubsetStart22Skip[1-3]",
                                              ".*::LinkedItemsWeekly::testSubsetStart33Skip[1-3]",
                                              ".*::LinkedItemsWeekly::testSubsetStart44.*" ] +
                                            # The disables the synccompare simplifications for
                                            # BDAY and friends, and therefore fails.
                                            [ ".*::testExtensions" ]
                                            ) +
                                   " "

                                   "CLIENT_TEST_SKIP="
                                   # See "[SyncEvolution] one-way sync + sync tokens not updated":
                                   # one-way sync keeps using old (and obsolete) sync keys,
                                   # thus running into unexpected slow syncs with ActiveSync.
                                   "Client::Sync::.*::testOneWayFromClient,"
                                   "Client::Sync::.*::testOneWayFromLocal,"
                                   " "

                                   "CLIENT_TEST_LOG=activesyncd.log "
                                   ,
                                   testPrefix=" ".join(("env EAS_DEBUG_FILE=activesyncd.log",
                                                        os.path.join(sync.basedir, "test", "wrappercheck.sh"),
                                                        options.testprefix,
                                                        self.activesyncd,
                                                        "--",
                                                        options.testprefix)))

    def executeWithActiveSync(self):
        '''start and stop activesyncd before/after running the test'''
        args = []
        if options.testprefix:
            args.append(options.testprefix)
        args.append(self.activesyncd)
        env = copy.deepcopy(os.environ)
        env['EAS_SOUP_LOGGER'] = '1'
        env['EAS_DEBUG'] = '5'
        env['EAS_DEBUG_DETACHED_RECURRENCES'] = '1'
        activesyncd = subprocess.Popen(args,
                                       env=env)
        try:
            SyncEvolutionTest.execute(self)
        finally:
            if not ShutdownSubprocess(activesyncd, 5):
                raise Exception("activesyncd had to be killed with SIGKILL")
            returncode = activesyncd.poll()
            if returncode != None:
                if returncode != 0:
                    raise Exception("activesyncd returned %d" % returncode)
            else:
                raise Exception("activesyncd did not return")

test = ActiveSyncTest("exchange")
context.add(test)

test = ActiveSyncTest("googleeas",
                      ["eds_contact", "eas_contact"],
                      env="CLIENT_TEST_DELAY=10 CLIENT_TEST_SOURCE_DELAY=10 ",
                      knownFailures=[
                          # Google does not support the Fetch operation, leading
                          # to an unhandled generic error.
                          ".*::testReadItem404",
                          # Remove of PHOTO not supported by Google (?),
                          # works with Exchange.
                          "Client::Source::eas_contact::testRemoveProperties",
                          ])
context.add(test)

syncevoPrefix=" ".join([os.path.join(sync.basedir, "test", "wrappercheck.sh")] +
                       # redirect output of command run under valgrind (when
                       # using valgrind) or of the whole command (otherwise)
                       # to syncevohttp.log
                       ( 'valgrindcheck' in options.testprefix and \
                             [ "VALGRIND_CMD_LOG=syncevohttp.log" ] or \
                             [ "--daemon-log", "syncevohttp.log" ] ) +
                       [ options.testprefix,
                         os.path.join(compile.installdir, "usr", "libexec", "syncevo-dbus-server"),
                         '--verbosity=3', # Full information about daemon operation.
                         '--dbus-verbosity=1', # Only errors from syncevo-dbus-server and syncing.
                         '--stdout', '--no-syslog', # Write into same syncevohttp.log as syncevo-http-server.
                         '--duration=unlimited', # Never shut down, even if client is inactive for a while.
                         "--",
                         os.path.join(sync.basedir, "test", "wrappercheck.sh"),
                         # also redirect additional syncevo-http-server
                         # output into the same file
                         "--daemon-log", "syncevohttp.log",
                         "--wait-for-daemon-output", "syncevo-http:.listening.on.port.<httpport>",
                         os.path.join(compile.installdir, "usr", "bin", "syncevo-http-server"),
                         "--debug",
                         "http://127.0.0.1:<httpport>/syncevolution",
                         "--",
                         options.testprefix])

# The test uses EDS on the clients and a server config with file
# backends - normal tests.
test = SyncEvolutionTest("edsfile",
                         compile,
                         "", options.shell,
                         "Client::Sync::eds_event Client::Sync::eds_contact Client::Sync::eds_event_eds_contact",
                         [ "eds_event", "eds_contact" ],
                         "CLIENT_TEST_NUM_ITEMS=100 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # Slow, and running many syncs still fails when using
                         # valgrind. Tested separately below in "edsxfile".
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         # server cannot detect pairs based on UID/RECURRENCE-ID
                         "CLIENT_TEST_ADD_BOTH_SIDES_SERVER_IS_DUMB=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9900'))
context.add(test)

# The test uses EDS on the client and server server side.
test = SyncEvolutionTest("edseds",
                         compile,
                         "", options.shell,
                         "Client::Sync::eds_event Client::Sync::eds_contact Client::Sync::eds_event_eds_contact",
                         [ "eds_event", "eds_contact" ],
                         "CLIENT_TEST_NUM_ITEMS=100 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # Slow, and running many syncs still fails when using
                         # valgrind. Tested separately below in "edsxfile".
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9901'))
context.add(test)

# The test uses EDS on the clients and a server config with file
# backends - suspend/retry/resend tests.
test = SyncEvolutionTest("edsxfile",
                         compile,
                         "", options.shell,
                         "Client::Sync::eds_contact::Retry Client::Sync::eds_contact::Resend Client::Sync::eds_contact::Suspend",
                         [ "eds_contact" ],
                         "CLIENT_TEST_NUM_ITEMS=100 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         "CLIENT_TEST_RETRY=t "
                         "CLIENT_TEST_RESEND=t "
                         "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         # server cannot detect pairs based on UID/RECURRENCE-ID
                         "CLIENT_TEST_ADD_BOTH_SIDES_SERVER_IS_DUMB=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9902'))
# a lot of syncs per test
test.alarmSeconds = 6000
context.add(test)

# This one uses CalDAV/CardDAV in DAViCal and the same server config
# with file backends as edsfile.
test = SyncEvolutionTest("davfile",
                         compile,
                         "", options.shell,
                         "Client::Sync::davical_caldav Client::Sync::davical_caldavtodo Client::Sync::davical_carddav Client::Sync::davical_caldav_davical_caldavtodo_davical_carddav",
                         [ "davical_caldav", "davical_caldavtodo", "davical_carddav" ],
                         "CLIENT_TEST_SIMPLE_UID=1 " # DAViCal server gets confused by UID with special characters
                         "CLIENT_TEST_WEBDAV='davical caldav caldavtodo carddav' "
                         "CLIENT_TEST_NUM_ITEMS=10 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # could be enabled, but reporting result is currently missing (BMC #1009)
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         # server cannot detect pairs based on UID/RECURRENCE-ID
                         "CLIENT_TEST_ADD_BOTH_SIDES_SERVER_IS_DUMB=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9903'))
context.add(test)

# EDS on client side, DAV on server.
test = SyncEvolutionTest("edsdav",
                         compile,
                         "", options.shell,
                         "Client::Sync::eds_event Client::Sync::eds_contact Client::Sync::eds_event_eds_contact",
                         [ "eds_event", "eds_contact" ],
                         "CLIENT_TEST_SIMPLE_UID=1 " # DAViCal server gets confused by UID with special characters
                         "CLIENT_TEST_NUM_ITEMS=10 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # could be enabled, but reporting result is currently missing (BMC #1009)
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9904'))
context.add(test)

# The test uses plain files on clients and a server config with EDS
# backend. This can be used to send test items to SyncEvoltion which have
# not gone through the EDS import step first.
test = SyncEvolutionTest("fileeds",
                         compile,
                         "", options.shell,
                         "Client::Sync::file_event Client::Sync::file_contact",
                         [ "file_event", "file_contact" ],
                         "CLIENT_TEST_NUM_ITEMS=100 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # Slow, and running many syncs still fails when using
                         # valgrind. Tested separately below in "edsxfile".
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9905'))
context.add(test)

# The test uses plain files on clients and server. This allows checking of
# content without having transformations inside EDS involved at all.
test = SyncEvolutionTest("filefile",
                         compile,
                         "", options.shell,
                         "Client::Sync::file_event Client::Sync::file_contact",
                         [ "file_event", "file_contact" ],
                         "CLIENT_TEST_NUM_ITEMS=100 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # Slow, and running many syncs still fails when using
                         # valgrind. Tested separately below in "edsxfile".
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         # server cannot detect pairs based on UID/RECURRENCE-ID
                         "CLIENT_TEST_ADD_BOTH_SIDES_SERVER_IS_DUMB=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9906'))
context.add(test)

# The test uses Akonadi on the client and server server side.
test = SyncEvolutionTest("kdekde",
                         compile,
                         "", options.shell,
                         "Client::Sync::kde_event Client::Sync::kde_contact",
                         [ "kde_event", "kde_contact" ],
                         "CLIENT_TEST_NUM_ITEMS=100 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # Slow, and running many syncs still fails when using
                         # valgrind. Tested separately below in "edsxfile".
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "
                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9907'))
context.add(test)

# The test uses files on the client and KDE on the server server side.
test = SyncEvolutionTest("filekde",
                         compile,
                         "", options.shell,
                         "Client::Sync::file_event Client::Sync::file_contact",
                         [ "file_event", "file_contact" ],
                         "CLIENT_TEST_NUM_ITEMS=100 "
                         "CLIENT_TEST_LOG=syncevohttp.log "
                         # Slow, and running many syncs still fails when using
                         # valgrind. Tested separately below in "edsxfile".
                         # "CLIENT_TEST_RETRY=t "
                         # "CLIENT_TEST_RESEND=t "
                         # "CLIENT_TEST_SUSPEND=t "
                         # server supports refresh-from-client, use it for
                         # more efficient test setup
                         "CLIENT_TEST_DELETE_REFRESH=1 "
                         # server supports multiple cycles inside the same session
                         "CLIENT_TEST_PEER_CAN_RESTART=1 "

                         "CLIENT_TEST_FAILURES="
                         # Different vcard flavor, need different test data (just as
                         # in testImport).
                         "Client::Sync::file_contact::testItems,"
                         " "

                         "CLIENT_TEST_SKIP="
                         ,
                         testPrefix=syncevoPrefix.replace('<httpport>', '9908'))
context.add(test)

scheduleworldtest = SyncEvolutionTest("scheduleworld", compile,
                                      "", options.shell,
                                      "Client::Sync",
                                      [ "eds_contact",
                                        "eds_event",
                                        "eds_task",
                                        "eds_memo" ],
                                      "CLIENT_TEST_NUM_ITEMS=10 "
                                      "CLIENT_TEST_FAILURES="
                                      "Client::Sync::eds_memo::testManyItems,"
                                      "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::testManyItems,"
                                      "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::testManyItems CLIENT_TEST_SKIP=Client::Sync::eds_event::Retry,"
                                      "Client::Sync::eds_event::Suspend,"
                                      "Client::Sync::eds_event::Resend,"
                                      "Client::Sync::eds_contact::Retry,"
                                      "Client::Sync::eds_contact::Suspend,"
                                      "Client::Sync::eds_contact::Resend,"
                                      "Client::Sync::eds_task::Retry,"
                                      "Client::Sync::eds_task::Suspend,"
                                      "Client::Sync::eds_task::Resend,"
                                      "Client::Sync::eds_memo::Retry,"
                                      "Client::Sync::eds_memo::Suspend,"
                                      "Client::Sync::eds_memo::Resend,"
                                      "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::Retry,"
                                      "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::Suspend,"
                                      "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::Resend,"
                                      "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::Retry,"
                                      "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::Suspend,"
                                      "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::Resend "
                                      "CLIENT_TEST_DELAY=5 "
                                      "CLIENT_TEST_RESEND_TIMEOUT=5 "
                                      "CLIENT_TEST_INTERRUPT_AT=1",
                                      testPrefix=options.testprefix)
context.add(scheduleworldtest)

egroupwaretest = SyncEvolutionTest("egroupware", compile,
                                   "", options.shell,
                                   "Client::Sync::eds_contact "
                                   "Client::Sync::eds_event::testCopy "
                                   "Client::Sync::eds_event::testUpdate "
                                   "Client::Sync::eds_event::testDelete "
                                   "Client::Sync::eds_contact_eds_event::testCopy "
                                   "Client::Sync::eds_contact_eds_event::testUpdate "
                                   "Client::Sync::eds_contact_eds_event::testDelete "
                                   "Client::Sync::eds_event_eds_contact::testCopy "
                                   "Client::Sync::eds_event_eds_contact::testUpdate "
                                   "Client::Sync::eds_event_eds_contact::testDelete ",
                                   [ "eds_contact",
                                     "eds_event" ],
                                   # ContactSync::testRefreshFromServerSync,ContactSync::testRefreshFromClientSync,ContactSync::testDeleteAllRefresh,ContactSync::testRefreshSemantic,ContactSync::testRefreshStatus - refresh-from-client not supported by server
                                   # ContactSync::testOneWayFromClient - not supported by server?
                                   # ContactSync::testItems - loses a lot of information
                                   # ContactSync::testComplexUpdate - only one phone number preserved
                                   # ContactSync::testMaxMsg,ContactSync::testLargeObject,ContactSync::testLargeObjectBin - server fails to parse extra info?
                                   # ContactSync::testTwinning - duplicates contacts
                                   # CalendarSync::testCopy,CalendarSync::testUpdate - shifts time?
                                   "CLIENT_TEST_FAILURES="
                                   "ContactSync::testRefreshFromServerSync,"
                                   "ContactSync::testRefreshFromClientSync,"
                                   "ContactSync::testDeleteAllRefresh,"
                                   "ContactSync::testRefreshSemantic,"
                                   "ContactSync::testRefreshStatus,"
                                   "ContactSync::testOneWayFromClient,"
                                   "ContactSync::testAddUpdate,"
                                   "ContactSync::testItems,"
                                   "ContactSync::testComplexUpdate,"
                                   "ContactSync::testTwinning,"
                                   "ContactSync::testMaxMsg,"
                                   "ContactSync::testLargeObject,"
                                   "ContactSync::testLargeObjectBin,"
                                   "CalendarSync::testCopy,"
                                   "CalendarSync::testUpdate",
                                   lambda x: x.replace('oasis.ethz.ch','<host hidden>').\
                                             replace('cG9obHk6cWQyYTVtZ1gzZk5GQQ==','xxx'),
                                   testPrefix=options.testprefix)
context.add(egroupwaretest)

class SynthesisTest(SyncEvolutionTest):
    def __init__(self, name, build, synthesisdir, runner, testPrefix):
        SyncEvolutionTest.__init__(self, name, build, "", # os.path.join(synthesisdir, "logs")
                                   runner,
                                   "Client::Sync",
                                   [ "eds_contact",
                                     "eds_memo" ],
                                   "CLIENT_TEST_SKIP="
                                   "Client::Sync::eds_event::Retry,"
                                   "Client::Sync::eds_event::Suspend,"
                                   "Client::Sync::eds_event::Resend,"
                                   "Client::Sync::eds_contact::Retry,"
                                   "Client::Sync::eds_contact::Suspend,"
                                   "Client::Sync::eds_contact::Resend,"
                                   "Client::Sync::eds_task::Retry,"
                                   "Client::Sync::eds_task::Suspend,"
                                   "Client::Sync::eds_task::Resend,"
                                   "Client::Sync::eds_memo::Retry,"
                                   "Client::Sync::eds_memo::Suspend,"
                                   "Client::Sync::eds_memo::Resend,"
                                   "Client::Sync::eds_contact_eds_memo::Retry,"
                                   "Client::Sync::eds_contact_eds_memo::Suspend,"
                                   "Client::Sync::eds_contact_eds_memo::Resend "
                                   "CLIENT_TEST_NUM_ITEMS=20 "
                                   "CLIENT_TEST_DELAY=2 "
                                   "CLIENT_TEST_RESEND_TIMEOUT=5",
                                   serverName="synthesis",
                                   testPrefix=testPrefix)
        self.synthesisdir = synthesisdir
        # self.dependencies.append(evolutiontest.name)

    def execute(self):
        if self.synthesisdir:
            context.runCommand("synthesis start \"%s\"" % (self.synthesisdir))
        time.sleep(5)
        try:
            SyncEvolutionTest.execute(self)
        finally:
            if self.synthesisdir:
                context.runCommand("synthesis stop \"%s\"" % (self.synthesisdir))

synthesis = SynthesisTest("synthesis", compile,
                          options.synthesisdir,
                          options.shell,
                          options.testprefix)
context.add(synthesis)

class FunambolTest(SyncEvolutionTest):
    def __init__(self, name, build, funamboldir, runner, testPrefix):
        if funamboldir:
            serverlogs = os.path.join(funamboldir, "ds-server", "logs", "funambol_ds.log")
        else:
            serverlogs = ""
        SyncEvolutionTest.__init__(self, name, build, serverlogs,
                                   runner,
                                   "Client::Sync",
                                   [ "eds_contact",
                                     "eds_event",
                                     "eds_task",
                                     "eds_memo" ],
                                   "CLIENT_TEST_SKIP="
                                   # server duplicates items in add<->add conflict because it
                                   # does not check UID
                                   "Client::Sync::eds_event::testAddBothSides,"
                                   "Client::Sync::eds_event::testAddBothSidesRefresh,"
                                   "Client::Sync::eds_task::testAddBothSides,"
                                   "Client::Sync::eds_task::testAddBothSidesRefresh,"
                                   # Avoid all tests which do a slow sync, to avoid 417 throttling.
                                   "Client::Sync::.*::(testDeleteAllRefresh|testSlowRestart|testTwinning|testSlowSync|testManyItems|testManyDeletes|testSlowSyncSemantic),"
                                   # test cannot pass because we don't have CtCap info about
                                   # the Funambol server
                                   "Client::Sync::eds_contact::testExtensions,"
                                   " "
                                   "CLIENT_TEST_XML=1 "
                                   "CLIENT_TEST_MAX_ITEMSIZE=2048 "
                                   "CLIENT_TEST_DELAY=10 "
                                   # Using refresh-from-client is important, Funambol
                                   # throttles slow syncs.
                                   "CLIENT_TEST_DELETE_REFRESH=1 "
                                   "CLIENT_TEST_FAILURES="
                                   "Client::Sync::eds_contact::testTwinning,"
                                   "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::testTwinning,"
                                   "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::testTwinning "
                                   "CLIENT_TEST_RESEND_TIMEOUT=5 "
                                   "CLIENT_TEST_INTERRUPT_AT=1",
                                   lineFilter=lambda x: x.replace('dogfood.funambol.com','<host hidden>'),
                                   serverName="funambol",
                                   testPrefix=testPrefix)
        self.funamboldir = funamboldir
        # self.dependencies.append(evolutiontest.name)

    def execute(self):
        if self.funamboldir:
            context.runCommand("%s/tools/bin/funambol.sh start" % (self.funamboldir))
        time.sleep(5)
        try:
            SyncEvolutionTest.execute(self)
        finally:
            if self.funamboldir:
                context.runCommand("%s/tools/bin/funambol.sh stop" % (self.funamboldir))

funambol = FunambolTest("funambol", compile,
                        options.funamboldir,
                        options.shell,
                        options.testprefix)
context.add(funambol)

zybtest = SyncEvolutionTest("zyb", compile,
                            "", options.shell,
                            "Client::Sync",
                            [ "eds_contact" ],
                            "CLIENT_TEST_NUM_ITEMS=10 "
                            "CLIENT_TEST_SKIP="
                            "Client::Sync::eds_contact::Retry,"
                            "Client::Sync::eds_contact::Suspend,"
                            "Client::Sync::eds_contact::Resend "
                            "CLIENT_TEST_DELAY=5 ",
                            testPrefix=options.testprefix)
context.add(zybtest)

googletest = SyncEvolutionTest("google", compile,
                               "", options.shell,
                               "Client::Sync",
                               [ "eds_contact" ],
                               "CLIENT_TEST_NUM_ITEMS=10 "
                               "CLIENT_TEST_XML=0 "
                               "CLIENT_TEST_MAX_ITEMSIZE=2048 "
                               "CLIENT_TEST_SKIP="
                               "Client::Sync::eds_contact::Retry,"
                               "Client::Sync::eds_contact::Suspend,"
                               "Client::Sync::eds_contact::Resend,"
                               # refresh-from-client not supported by Google
                               "Client::Sync::eds_contact::testRefreshFromClientSync,"
                               "Client::Sync::eds_contact::testRefreshFromClientSemantic,"
                               "Client::Sync::eds_contact::testRefreshStatus,"
                               "Client::Sync::eds_contact::testDeleteAllRefresh,"
                               "Client::Sync::eds_contact::testOneWayFromClient,"
                               "Client::Sync::eds_contact::testRefreshFromLocalSync,"
                               "Client::Sync::eds_contact::testOneWayFromLocal,"
                               # only WBXML supported by Google
                               "Client::Sync::eds_contact::testItemsXML "
                               "CLIENT_TEST_DELAY=5 ",
                               testPrefix=options.testprefix)
context.add(googletest)

mobicaltest = SyncEvolutionTest("mobical", compile,
                                "", options.shell,
                                "Client::Sync",
                                [ "eds_contact",
                                  "eds_event",
                                  "eds_task" ],
                                # "eds_memo" - no longer works, 400 "Bad Request"

                                # all-day detection in vCalendar 1.0
                                # only works if client and server
                                # agree on the time zone (otherwise the start/end times
                                # do not align with midnight); the nightly test account
                                # happens to use Europe/Berlin
                                "TZ=Europe/Berlin "
                                "CLIENT_TEST_NOCHECK_SYNCMODE=1 "
                                "CLIENT_TEST_MAX_ITEMSIZE=2048 "
                                "CLIENT_TEST_SKIP="
                                # server duplicates items in add<->add conflict because it
                                # does not check UID
                                "Client::Sync::eds_event::testAddBothSides,"
                                "Client::Sync::eds_event::testAddBothSidesRefresh,"
                                "Client::Sync::eds_task::testAddBothSides,"
                                "Client::Sync::eds_task::testAddBothSidesRefresh,"
                                "Client::Sync::.*::testRefreshFromClientSync,"
                                "Client::Sync::.*::testSlowSyncSemantic,"
                                "Client::Sync::.*::testRefreshStatus,"
                                "Client::Sync::.*::testDelete,"
                                "Client::Sync::.*::testItemsXML,"
                                "Client::Sync::.*::testOneWayFromServer,"
                                "Client::Sync::.*::testOneWayFromClient,"
                                "Client::Sync::.*::testRefreshFromLocalSync,"
                                "Client::Sync::.*::testOneWayFromLocal,"
                                "Client::Sync::.*::testOneWayFromRemote,"
                                "Client::Sync::.*::Retry,"
                                "Client::Sync::.*::Suspend,"
                                "Client::Sync::.*::Resend "
                                "CLIENT_TEST_DELAY=5 "
                                "CLIENT_TEST_RESEND_TIMEOUT=5 "
                                "CLIENT_TEST_INTERRUPT_AT=1",
                                testPrefix=options.testprefix)
context.add(mobicaltest)

memotootest = SyncEvolutionTest("memotoo", compile,
                                "", options.shell,
                                "Client::Sync",
                                [ "eds_contact",
                                  "eds_event",
                                  "eds_task",
                                  "eds_memo" ],
                                "CLIENT_TEST_NOCHECK_SYNCMODE=1 "
                                "CLIENT_TEST_NUM_ITEMS=10 "
                                "CLIENT_TEST_FAILURES="
                                # Server merges conflicting two items, but drops the
                                # X-AIM from the first one.
                                "Client::Sync::.*eds_contact.*::testMerge,"
                                " "
                                "CLIENT_TEST_SKIP="
                                # server duplicates items in add<->add conflict because it
                                # does not check UID
                                "Client::Sync::eds_event::testAddBothSides,"
                                "Client::Sync::eds_event::testAddBothSidesRefresh,"
                                "Client::Sync::eds_task::testAddBothSides,"
                                "Client::Sync::eds_task::testAddBothSidesRefresh,"
                                "Client::Sync::eds_contact::Retry,"
                                "Client::Sync::eds_contact::Suspend,"
                                # "Client::Sync::eds_contact::testRefreshFromClientSync,"
                                # "Client::Sync::eds_contact::testRefreshFromClientSemantic,"
                                # "Client::Sync::eds_contact::testDeleteAllRefresh,"
                                # "Client::Sync::eds_contact::testOneWayFromServer,"
                                "Client::Sync::eds_event::testRefreshFromClientSync,"
                                "Client::Sync::eds_event::testRefreshFromClientSemantic,"
                                "Client::Sync::eds_event::testOneWayFromServer,"
                                "Client::Sync::eds_event::testDeleteAllRefresh,"
                                "Client::Sync::eds_event::Retry,"
                                "Client::Sync::eds_event::Suspend,"
                                "Client::Sync::eds_task::testRefreshFromClientSync,"
                                "Client::Sync::eds_task::testRefreshFromClientSemantic,"
                                "Client::Sync::eds_task::testDeleteAllRefresh,"
                                "Client::Sync::eds_task::testOneWayFromServer,"
                                "Client::Sync::eds_task::Retry,"
                                "Client::Sync::eds_task::Suspend,"
                                "Client::Sync::eds_memo::testRefreshFromClientSync,"
                                "Client::Sync::eds_memo::testRefreshFromClientSemantic,"
                                "Client::Sync::eds_memo::testDeleteAllRefresh,"
                                "Client::Sync::eds_memo::testOneWayFromServer,"
                                "Client::Sync::eds_memo::Retry,"
                                "Client::Sync::eds_memo::Suspend,"
                                "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::testRefreshFromClientSync,"
                                "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::testRefreshFromClientSemantic,"
                                "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::testDeleteAllRefresh,"
                                "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::testOneWayFromServer,"
                                "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::Retry,"
                                "Client::Sync::eds_contact_eds_event_eds_task_eds_memo::Suspend,"
                                "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::testRefreshFromClientSync,"
                                "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::testRefreshFromClientSemantic,"
                                "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::testOneWayFromServer,"
                                "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::testDeleteAllRefresh,"
                                "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::Retry,"
                                "Client::Sync::eds_event_eds_task_eds_memo_eds_contact::Suspend "
                                "CLIENT_TEST_DELAY=10 "
                                "CLIENT_TEST_RESEND_TIMEOUT=5 "
                                "CLIENT_TEST_INTERRUPT_AT=1",
                                testPrefix=options.testprefix)
context.add(memotootest)

ovitest = SyncEvolutionTest("ovi", compile,
                                "", options.shell,
                                "Client::Sync",
                                [ "eds_contact",
                                  "calendar+todo" ],
                                "CLIENT_TEST_DELETE_REFRESH=1 "
                                "CLIENT_TEST_NUM_ITEMS=50 "
                                "CLIENT_TEST_MAX_ITEMSIZE=512 "
                                "CLIENT_TEST_SKIP="
                                "Client::Sync::eds_contact::Retry,"
                                "Client::Sync::eds_contact::Suspend,"
                                "Client::Sync::eds_contact::testOneWayFromClient,"
                                "Client::Sync::eds_contact::testOneWayFromServer,"
                                "Client::Sync::eds_contact::testSlowSyncSemantic,"
                                "Client::Sync::eds_contact::testComplexRefreshFromServerSemantic,"
                                "Client::Sync::eds_contact::testDelete,"
                                "Client::Sync::eds_contact::testDeleteAllSync,"
                                "Client::Sync::eds_contact::testManyDeletes,"
                                "Client::Sync::calendar+todo::Retry,"
                                "Client::Sync::calendar+todo::Suspend,"
                                "Client::Sync::calendar+todo::testOneWayFromClient,"
                                "Client::Sync::calendar+todo::testOneWayFromServer,"
                                "Client::Sync::calendar+todo::testSlowSyncSemantic,"
                                "Client::Sync::calendar+todo::testComplexRefreshFromServerSemantic,"
                                "Client::Sync::calendar+todo::testDelete,"
                                "Client::Sync::calendar+todo::testDeleteAllSync,"
                                "Client::Sync::calendar+todo::testManyDeletes,"
                                "Client::Sync::calendar+todo::testDeleteAllRefresh,"
                                "Client::Sync::calendar+todo::testItemsXML,"
                                "Client::Sync::calendar+todo::testMaxMsg,"
                                "Client::Sync::calendar+todo::testLargeObject,"
                                "Client::Sync::calendar+todo_eds_contact::Retry,"
                                "Client::Sync::calendar+todo_eds_contact::Suspend,"
                                "Client::Sync::calendar+todo_eds_contact::testOneWayFromClient,"
                                "Client::Sync::calendar+todo_eds_contact::testOneWayFromServer,"
                                "Client::Sync::calendar+todo_eds_contact::testSlowSyncSemantic,"
                                "Client::Sync::calendar+todo_eds_contact::testComplexRefreshFromServerSemantic,"
                                "Client::Sync::calendar+todo_eds_contact::testDelete,"
                                "Client::Sync::calendar+todo_eds_contact::testDeleteAllSync,"
                                "Client::Sync::calendar+todo_eds_contact::testManyDeletes,"
                                "Client::Sync::calendar+todo::Retry,"
                                "Client::Sync::eds_contact_calendar+todo::Suspend,"
                                "Client::Sync::eds_contact_calendar+todo::testOneWayFromClient,"
                                "Client::Sync::eds_contact_calendar+todo::testOneWayFromServer,"
                                "Client::Sync::eds_contact_calendar+todo::testSlowSyncSemantic,"
                                "Client::Sync::eds_contact_calendar+todo::testComplexRefreshFromServerSemantic,"
                                "Client::Sync::eds_contact_calendar+todo::testDelete,"
                                "Client::Sync::eds_contact_calendar+todo::testDeleteAllSync,"
                                "Client::Sync::eds_contact_calendar+todo::testManyDeletes,"
                                "CLIENT_TEST_DELAY=5 "
                                "CLIENT_TEST_RESEND_TIMEOUT=5 "
                                "CLIENT_TEST_INTERRUPT_AT=1",
                                serverName="Ovi",
                                testPrefix=options.testprefix)
context.add(ovitest)

if options.list:
    for action in context.todo:
        print action.name
else:
    pid = os.getpid()
    log('Ready to run. I have PID %d.', pid)
    try:
        context.execute()
    except exceptions.SystemExit:
        raise
    except:
        # Something went wrong. Send emergency email if an email is
        # expected and we are the parent process.
        if pid == os.getpid():
            server, body, writer = context.startEmail()
            if server:
                writer.startbody("text/html;charset=ISO-8859-1").write('<html><body><pre>%s</pre></body></html>' %
                                                                       traceback.format_exc())
                context.finishEmail(server, body)
        raise
