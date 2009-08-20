#!/usr/bin/python


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

def abspath(path):
    """Absolute path after expanding vars and user."""
    return os.path.abspath(os.path.expanduser(os.path.expandvars(path)))

def del_dir(path):
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
    os.rmdir(path)


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
        htaccess.write("AddDescription \"%s\" %s\n" %
                       (error.strip().replace("\"", "'").replace("<", "&lt;").replace(">","&gt;"),
                        os.path.basename(filename)))

class Action:
    """Base class for all actions to be performed."""

    DONE = "0 DONE"
    WARNINGS = "1 WARNINGS"
    FAILED = "2 FAILED"
    TODO = "3 TODO"
    SKIPPED = "4 SKIPPED"
    COMPLETED = (DONE, WARNINGS)

    def __init__(self, name):
        self.name = name
        self.status = self.TODO
        self.summary = ""
        self.dependencies = []
        self.isserver = False;

    def execute(self):
        """Runs action. Throws an exeception if anything fails.
        Will be called by tryexecution() with stderr/stdout redirected into a file
        and the current directory set to an empty temporary directory.
        """
        raise Exception("not implemented")

    def tryexecution(self, step, logs):
        """wrapper around execute which handles exceptions, directories and stdout"""
        if logs:
            fd = -1
            oldstdout = os.dup(1)
            oldstderr = os.dup(2)
            oldout = sys.stdout
            olderr = sys.stderr
        cwd = os.getcwd()
        try:
            subdirname = "%d-%s" % (step, self.name)
            del_dir(subdirname)
            sys.stderr.flush()
            sys.stdout.flush()
            cd(subdirname)
            if logs:
                fd = os.open("output.txt", os.O_WRONLY|os.O_CREAT|os.O_TRUNC)
                os.dup2(fd, 1)
                os.dup2(fd, 2)
                sys.stdout = os.fdopen(fd, "w")
                sys.stderr = sys.stdout
            print "=== starting %s ===" % (self.name)
            self.execute()
            self.status = Action.DONE
            self.summary = "okay"
        except Exception, inst:
            traceback.print_exc()
            self.status = Action.FAILED
            self.summary = str(inst)

        print "\n=== %s: %s ===" % (self.name, self.status)
        sys.stdout.flush()
        os.chdir(cwd)
        if logs:
            if fd >= 0:
                os.close(fd)
                os.dup2(oldstdout, 1)
                os.dup2(oldstderr, 2)
                sys.stderr = olderr
                sys.stdout = oldout
                os.close(oldstdout)
                os.close(oldstderr)
        return self.status

class Context:
    """Provides services required by actions and handles running them."""

    def __init__(self, tmpdir, resultdir, uri, workdir, mailtitle, sender, recipients, mailhost, enabled, skip, nologs, setupcmd, make, sanitychecks):
        # preserve normal stdout because stdout/stderr will be redirected
        self.out = os.fdopen(os.dup(1), "w")
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

    def runCommand(self, cmd):
        """Log and run the given command, throwing an exception if it fails."""
        print "%s: %s" % (os.getcwd(), cmd)
        sys.stdout.flush()
        result = os.system(cmd)
        if result != 0:
            raise Exception("%s: failed (return code %d)" % (cmd, result))

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
        s = file("output.txt", "w+")
        status = Action.DONE

        step = 0
        run_servers=[];
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
                    for depend in action.dependencies:
                        if not self.actions[depend].status in Action.COMPLETED:
                            action.status = Action.SKIPPED
                            self.summary.append("%s skipped: required %s has not been executed" % (action.name, depend))
                            break

                if action.status == Action.SKIPPED:
                    continue

                # execute it
                action.tryexecution(step, not self.nologs)
                if action.status > status:
                    status = action.status
                if action.status == Action.FAILED:
                    self.summary.append("%s: %s" % (action.name, action.summary))
                elif action.status == Action.WARNINGS:
                    self.summary.append("%s done, but check the warnings" % action.name)
                else:
                    self.summary.append("%s successful" % action.name)
                if action.isserver:
                    run_servers.append(action.name);
            except Exception, inst:
                traceback.print_exc()
                self.summary.append("%s failed: %s" % (action.name, inst))

        # append all parameters to summary
        self.summary.append("")
        self.summary.extend(sys.argv)

        # update summary
        s.write("%s\n" % ("\n".join(self.summary)))
        s.close()

        # run testresult checker 
        #calculate the src dir where client-test can be located
        srcdir = os.path.join("build/src",self.tmpdir)
        os.system ("testresultchecker.py " +self.resultdir+" "+",".join(run_servers)+" "+self.uri +" "+srcdir);
        # transform to html
        os.system ("transform.sh "+self.resultdir +" "+ self.lastresultdir);
        # report result by email
        if self.recipients:
            server = smtplib.SMTP(self.mailhost)
            resulthtml = open ("nightly.html")
            line=resulthtml.readline()
            msg=''
            while(line!=''):
                    msg=msg+line
                        line=resulthtml.readline()
                        resultml.close()
            body = StringIO.StringIO()
            writer = MimeWriter.MimeWriter (body)
            writer.addheader("From", self.sender)
            writer.addheader("To", self.recipients)
            writer.addheader("Subject", self.mailtitle)
            writer.addheader("MIME-Version", "1.0")
            writer.flushheaders()
            writer.startbody("text/html;charset=ISO-8859-1").write(msg)

            failed = server.sendmail(self.sender, self.recipients, body.getvalue())
            if failed:
                print "could not send to: %s" % (failed)
                sys.exit(1)
        else:
            print "\n".join(self.summary), "\n"

        if status in Action.COMPLETED:
            sys.exit(0)
        else:
            sys.exit(1)

# must be set before instantiating some of the following classes
context = None
        
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
            os.chdir(self.module)
            context.runCommand("cvs update -d -r %s"  % (self.revision))
        elif self.revision == "HEAD":
            context.runCommand("cvs -d %s checkout %s" % (self.cvsroot, self.module))
            os.chdir(self.module)
        else:
            context.runCommand("cvs -d %s checkout -r %s %s" % (self.cvsroot, self.revision, self.module))
            os.chdir(self.module)
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
        os.chdir(self.module)
        if os.access("autogen.sh", os.F_OK):
            context.runCommand("%s ./autogen.sh" % (self.runner))

class GitCheckout(Action):
    """Does a git clone (if directory does not exist yet) or a fetch+checkout (if it does)."""
    
    def __init__(self, name, workdir, runner, url, revision):
        """workdir defines the directory to do the checkout in with 'name' as name of the sub directory,
        URL the server and repository,
        revision the desired branch or tag"""
        Action.__init__(self,name)
        self.workdir = workdir
        self.runner = runner
        self.url = url
        self.revision = revision
        self.basedir = os.path.join(abspath(workdir), name)

    def execute(self):
        if os.access(self.basedir, os.F_OK):
            cmd = "cd %s && git fetch %s && git checkout %s && git pull" % (self.basedir, self.url, self.revision)
        else:
            cmd = "git clone %s %s && cd %s && git checkout %s" % (self.url, self.basedir, self.basedir, self.revision)
        context.runCommand(cmd)
        os.chdir(self.basedir)
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

    def execute(self):
        del_dir(self.builddir)
        cd(self.builddir)
        context.runCommand("%s %s/configure %s" % (self.runner, self.src, self.configargs))
        context.runCommand("%s %s install DESTDIR=%s" % (self.runner, context.make, self.installdir))


class SyncEvolutionTest(Action):
    def __init__(self, name, build, serverlogs, runner, tests, sources, testenv="", lineFilter=None, testPrefix="", serverName=""):
        """Execute TestEvolution for all (empty tests) or the
        selected tests."""
        Action.__init__(self, name)
        self.isserver = True
        self.srcdir = os.path.join(build.builddir, "src")
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

    def execute(self):
        resdir = os.getcwd()
        os.chdir(self.srcdir)
        try:
            if context.setupcmd:
                context.runCommand("%s %s %s %s ./syncevolution" % (self.testenv, self.runner, context.setupcmd, self.name))
            basecmd = "CLIENT_TEST_SERVER=%s CLIENT_TEST_SOURCES=%s %s SYNC_EVOLUTION_EVO_CALENDAR_DELAY=1 CLIENT_TEST_ALARM=1200 CLIENT_TEST_LOG=%s CLIENT_TEST_EVOLUTION_PREFIX=file://%s/databases %s %s env LD_LIBRARY_PATH=build-synthesis/src/.libs ./client-test" % (self.serverName, ",".join(self.sources), self.testenv, self.serverlogs, context.workdir, self.runner, self.testPrefix);
            context.runCommand("%s testclean" % context.make)
            if self.tests:
                tests = []
                for test in self.tests:
                    if test == "Client::Sync" and context.sanitychecks:
                        # Replace with one simpler, faster testItems test, but be careful to
                        # pick an enabled source and the right mode (XML vs. WBXML).
                        # The first listed source and WBXML should be safe.
                        tests.append("Client::Sync::%s::testItems" % self.sources[0])
                    else:
                        tests.append(test)
                context.runCommand("%s %s" % (basecmd, " ".join(tests)))
            else:
                context.runCommand(basecmd)
        finally:
            tocopy = re.compile(r'.*\.log|.*\.client.[AB]')
            htaccess = file(os.path.join(resdir, ".htaccess"), "a")
            for f in os.listdir(self.srcdir):
                if tocopy.match(f):
                    copyLog(f, resdir, htaccess, self.lineFilter)



###################################################################
# Configuration part
###################################################################

parser = optparse.OptionParser()
parser.add_option("-e", "--enable",
                  action="append", type="string", dest="enabled",
                  help="use this to enable specific actions instead of executing all of them (can be used multiple times)")
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
parser.add_option("", "--workdir",
                  type="string", dest="workdir", default="",
                  help="directory for files which might be reused between runs")
parser.add_option("", "--resultdir",
                  type="string", dest="resultdir", default="",
                  help="directory for log files and results")
parser.add_option("", "--lastresultdir",
                  type="string", dest="lastresultdir", default="",
                  help="directory for last day's log files and results")
parser.add_option("", "--resulturi",
                  type="string", dest="uri", default=None,
                  help="URI that corresponds to --resultdir, if given this is used in mails instead of --resultdir")
parser.add_option("", "--shell",
                  type="string", dest="shell", default="",
                  help="a prefix which is put in front of a command to execute it (can be used for e.g. run_garnome)")
parser.add_option("", "--test-prefix",
                  type="string", dest="testprefix", default="",
                  help="a prefix which is put in front of client-test (e.g. valgrind)")
parser.add_option("", "--syncevo-tag",
                  type="string", dest="syncevotag", default="master",
                  help="the tag of SyncEvolution (e.g. syncevolution-0.7, default is 'master'")
parser.add_option("", "--synthesis-tag",
                  type="string", dest="synthesistag", default="master",
                  help="the tag of the synthesis library (default = master in the moblin.org repo)")
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
                  type="string", dest="subject", default="SyncML Tests " + time.strftime("%Y-%m-%d"),
                  help="subject of result email (default is \"SyncML Tests <date>\"")
parser.add_option("", "--evosvn",
                  action="append", type="string", dest="evosvn", default=[],
                  help="<name>=<path>: compiles Evolution from source under a short name, using Paul Smith's Makefile and config as found in <path>")
parser.add_option("", "--prebuilt",
                  action="append", type="string", dest="prebuilt", default=[],
                  help="a directory where SyncEvolution was build before: enables testing using those binaries (can be used multiple times)")
parser.add_option("", "--setup-command",
                  type="string", dest="setupcmd",
                  help="invoked with <test name> <args to start syncevolution>, should setup local account for the test")
parser.add_option("", "--make-command",
                  type="string", dest="makecmd", default="make",
                  help="command to use instead of plain make, for example 'make -j'")
parser.add_option("", "--sanity-checks",
                  action="store_true", dest="sanitychecks", default=False,
                  help="run limited number of sanity checks instead of full set")

(options, args) = parser.parse_args()
if options.recipients and not options.sender:
    print "sending email also requires sender argument"
    sys.exit(1)

context = Context(options.tmpdir, options.resultdir, options.uri, options.workdir,
                  options.subject, options.sender, options.recipients, options.mailhost,
                  options.enabled, options.skip, options.nologs, options.setupcmd,
                  options.makecmd, options.sanitychecks, options.lastresultdir)

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

for prebuilt in options.prebuilt:
    pre = Action("")
    pre.builddir = prebuilt
    if prebuilt:
        context.add(SyncEvolutionTest("evolution-prebuilt-" + os.path.basename(prebuilt), pre,
                                      "", options.shell,
                                      [ "Client::Source", "SyncEvolution" ],
                                      testPrefix=options.testprefix))

class SyncEvolutionCheckout(GitCheckout):
    def __init__(self, name, revision):
        """checkout SyncEvolution"""
        GitCheckout.__init__(self,
                             name, context.workdir, options.shell,
                             "git@git.moblin.org:syncevolution.git",
                             revision)

class SynthesisCheckout(GitCheckout):
    def __init__(self, name, revision):
        """checkout libsynthesis"""
        GitCheckout.__init__(self,
                             name, context.workdir, options.shell,
                             "git@git.moblin.org:libsynthesis.git",
                             revision)

class SyncEvolutionBuild(AutotoolsBuild):
    def execute(self):
        AutotoolsBuild.execute(self)
        os.chdir("src")
        context.runCommand("%s %s test CXXFLAGS=-O0" % (self.runner, context.make))

libsynthesis = SynthesisCheckout("libsynthesis", options.synthesistag)
context.add(libsynthesis)
sync = SyncEvolutionCheckout("syncevolution", options.syncevotag)
context.add(sync)
if options.synthesistag:
    synthesis_source = "--with-synthesis-src=%s" % libsynthesis.basedir
else:
    synthesis_source = ""
compile = SyncEvolutionBuild("compile",
                             sync.basedir,
                             "%s %s" % (options.configure, synthesis_source),
                             options.shell,
                             [ libsynthesis.name, sync.name ])
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
            context.runCommand("%s %s BINSUFFIX=%s deb" % (self.runner, context.make, self.packagesuffix))
	    put, get = os.popen4("dpkg-architecture -qDEB_HOST_ARCH")
	    for arch in get.readlines():
	           if "i386" in arch:
		   	context.runCommand("%s %s BINSUFFIX=%s PKGARCH=lpia deb" % (self.runner, context.make, self.packagesuffix))
			break
        if self.binsuffix:
            context.runCommand("%s %s BINSUFFIX=%s distbin" % (self.runner, context.make, self.binsuffix))
        context.runCommand("%s %s distcheck" % (self.runner, context.make))
        context.runCommand("%s %s DISTCHECK_CONFIGURE_FLAGS=--enable-gui distcheck" % (self.runner, context.make))

dist = SyncEvolutionDist("dist",
                         options.binsuffix,
                         options.packagesuffix,
                         options.shell,
                         [ compile.name ])
context.add(dist)

evolutiontest = SyncEvolutionTest("evolution", compile,
                                  "", options.shell,
                                  [ "Client::Source", "SyncEvolution" ],
                                  [],
                                  testPrefix=options.testprefix)
context.add(evolutiontest)

scheduleworldtest = SyncEvolutionTest("scheduleworld", compile,
                                      "", options.shell,
                                      [ "Client::Sync" ],
                                      [ "vcard30", "ical20", "itodo20", "text" ],
                                      "CLIENT_TEST_NUM_ITEMS=10 CLIENT_TEST_FAILURES=Client::Sync::text::testManyItems,Client::Sync::vcard30_ical20_itodo20_text::testManyItems,Client::Sync::text_itodo20_ical20_vcard30::testManyItems CLIENT_TEST_SKIP=Client::Sync::ical20::Retry,Client::Sync::ical20::Suspend,Client::Sync::vcard30::Retry,Client::Sync::vcard30::Suspend,Client::Sync::itodo20::Retry,Client::Sync::itodo20::Suspend,Client::Sync::text::Retry,Client::Sync::text::Suspend,Client::Sync::vcard30_ical20_itodo20_text::Retry,Client::Sync::vcard30_ical20_itodo20_text::Suspend,Client::Sync::text_itodo20_ical20_vcard30::Retry,Client::Sync::text_itodo20_ical20_vcard30::Suspend CLIENT_TEST_DELAY=5 CLIENT_TEST_COMPARE_LOG=T",
                                      testPrefix=options.testprefix)
context.add(scheduleworldtest)

memotootest = SyncEvolutionTest("memotoo", compile,
                                "", options.shell,
                                [ "Client::Sync" ],
                                [ "vcard21" ],
                                "CLIENT_TEST_NUM_ITEMS=10 CLIENT_TEST_FAILURES= CLIENT_TEST_DELAY=15",
                                testPrefix=options.testprefix)
context.add(memotootest)

egroupwaretest = SyncEvolutionTest("egroupware", compile,
                                   "", options.shell,
                                   [ "Client::Sync::vcard21", "Client::Sync::ical20::testCopy", "Client::Sync::ical20::testUpdate", "Client::Sync::ical20::testDelete", \
                                     "Client::Sync::vcard21_ical20::testCopy", "Client::Sync::vcard21_ical20::testUpdate", "Client::Sync::vcard21_ical20::testDelete" \
                                     "Client::Sync::ical20_vcard21::testCopy", "Client::Sync::ical20_vcard21::testUpdate", "Client::Sync::ical20_vcard21::testDelete"  ],
                                   [ "vcard21", "ical20" ],
                                   # ContactSync::testRefreshFromServerSync,ContactSync::testRefreshFromClientSync,ContactSync::testDeleteAllRefresh,ContactSync::testRefreshSemantic,ContactSync::testRefreshStatus - refresh-from-client not supported by server
                                   # ContactSync::testOneWayFromClient - not supported by server?
                                   # ContactSync::testItems - loses a lot of information
                                   # ContactSync::testComplexUpdate - only one phone number preserved
                                   # ContactSync::testMaxMsg,ContactSync::testLargeObject,ContactSync::testLargeObjectBin - server fails to parse extra info?
                                   # ContactSync::testTwinning - duplicates contacts
                                   # CalendarSync::testCopy,CalendarSync::testUpdate - shifts time?
                                   "CLIENT_TEST_FAILURES=ContactSync::testRefreshFromServerSync,ContactSync::testRefreshFromClientSync,ContactSync::testDeleteAllRefresh,ContactSync::testRefreshSemantic,ContactSync::testRefreshStatus,ContactSync::testOneWayFromClient,ContactSync::testAddUpdate,ContactSync::testItems,ContactSync::testComplexUpdate,ContactSync::testTwinning,ContactSync::testMaxMsg,ContactSync::testLargeObject,ContactSync::testLargeObjectBin,CalendarSync::testCopy,CalendarSync::testUpdate",
                                   lambda x: x.replace('oasis.ethz.ch','<host hidden>').\
                                             replace('cG9obHk6cWQyYTVtZ1gzZk5GQQ==','xxx'),
                                   testPrefix=options.testprefix)
context.add(egroupwaretest)

class SynthesisTest(SyncEvolutionTest):
    def __init__(self, name, build, synthesisdir, runner, testPrefix):
        SyncEvolutionTest.__init__(self, name, build, "", # os.path.join(synthesisdir, "logs")
                                   runner, [ "Client::Sync" ],
                                   [ "vcard21", "text" ],
                                   "CLIENT_TEST_SKIP=Client::Sync::ical20::Retry,Client::Sync::ical20::Suspend,Client::Sync::vcard21::Retry,Client::Sync::vcard21::Suspend,Client::Sync::itodo20::Retry,Client::Sync::itodo20::Suspend,Client::Sync::text::Retry,Client::Sync::text::Suspend,Client::Sync::vcard21_text::Retry,Client::Sync::vcard21_text::Suspend CLIENT_TEST_NUM_ITEMS=20 CLIENT_TEST_DELAY=2 CLIENT_TEST_COMPARE_LOG=T",
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
                                   [ "Client::Sync" ],
                                   [ "vcard21", "ical20", "itodo20", "text" ],
                                   "CLIENT_TEST_SKIP=Client::Sync::ical20::Retry,Client::Sync::ical20::Suspend,Client::Sync::vcard21::Retry,Client::Sync::vcard21::Suspend,Client::Sync::itodo20::Retry,Client::Sync::itodo20::Suspend,Client::Sync::text::Retry,Client::Sync::text::Suspend,Client::Sync::vcard21_ical20_itodo20_text::Retry,Client::Sync::vcard21_ical20_itodo20_text::Suspend,Client::Sync::text_itodo20_ical20_vcard21::Retry,Client::Sync::text_itodo20_ical20_vcard21::Suspend CLIENT_TEST_XML=1 CLIENT_TEST_MAX_ITEMSIZE=2048  CLIENT_TEST_DELAY=10 CLIENT_TEST_FAILURES= CLIENT_TEST_COMPARE_LOG=T",
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
                            [ "Client::Sync" ],
                            [ "vcard21" ],
                            "CLIENT_TEST_NUM_ITEMS=10 CLIENT_TEST_SKIP=Client::Sync::vcard21::Retry,Client::Sync::vcard21::Suspend CLIENT_TEST_DELAY=5 CLIENT_TEST_COMPARE_LOG=T",
                            testPrefix=options.testprefix)
context.add(zybtest)

googletest = SyncEvolutionTest("google", compile,
                               "", options.shell,
                               [ "Client::Sync" ],
                               [ "vcard21" ],
                               "CLIENT_TEST_NUM_ITEMS=10 CLIENT_TEST_XML=0 CLIENT_TEST_MAX_ITEMSIZE=2048 CLIENT_TEST_FAILURES=Client::Sync::vcard21::testRefreshFromClientSync,Client::Sync::vcard21::testRefreshFromClientSemantic,Client::Sync::vcard21::testRefreshStatus,Client::Sync::vcard21::testOneWayFromClient,Client::Sync::vcard21::testItemsXML CLIENT_TEST_SKIP=Client::Sync::vcard21::Retry,Client::Sync::vcard21::Suspend CLIENT_TEST_DELAY=5 CLIENT_TEST_COMPARE_LOG+T",
                               testPrefix=options.testprefix)
context.add(googletest)

if options.list:
    for action in context.todo:
        print action.name
else:
    context.execute()
