/*
 * Copyright (C) 2010 Patrick Ohly
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

#include <syncevo/LocalTransportAgent.h>
#include <syncevo/SyncContext.h>
#include <syncevo/SyncML.h>
#include <syncevo/LogRedirect.h>
#include <syncevo/StringDataBlob.h>
#include <syncevo/IniConfigNode.h>
#include <syncevo/GLibSupport.h>
#include <syncevo/DBusTraits.h>
#include <syncevo/SuspendFlags.h>
#include <syncevo/LogRedirect.h>
#include <syncevo/LogDLT.h>
#include <syncevo/BoostHelper.h>
#include <syncevo/TmpFile.h>

#include <synthesis/syerror.h>

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pcrecpp.h>

#include <algorithm>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

//
// It would be better to make these officially part of the libsynthesis API...
//
extern "C" {
    extern void  *(*smlLibMalloc)(size_t size);
    extern void  (*smlLibFree)(void *ptr);
}

/**
 * This class intercepts libsmltk memory functions and redirects the
 * buffer allocated for SyncML messages into shared memory.
 *
 * This works because:
 * - each side allocates exactly one such buffer
 * - the size of the buffer is twice the configured maximum message size
 * - we don't need to clean up or worry about the singleton because
 *   each process in SyncEvolution only runs one sync session
 */
class SMLTKSharedMemory
{
public:
    static SMLTKSharedMemory &singleton()
    {
        static SMLTKSharedMemory instance;
        return instance;
    }

    void initParent(size_t msgSize)
    {
        m_messageBufferSize = msgSize * 2;
        prepareBuffer(m_localBuffer, m_messageBufferSize);
        prepareBuffer(m_remoteBuffer, m_messageBufferSize);
        setenv("SYNCEVOLUTION_LOCAL_SYNC_PARENT_FD", StringPrintf("%d", m_localBuffer.getFD()).c_str(), true);
        setenv("SYNCEVOLUTION_LOCAL_SYNC_CHILD_FD", StringPrintf("%d", m_remoteBuffer.getFD()).c_str(), true);
        m_remoteBuffer.map(NULL, NULL);
    }

    void initChild(size_t msgSize)
    {
        m_messageBufferSize = msgSize * 2;
        m_remoteBuffer.create(atoi(getEnv("SYNCEVOLUTION_LOCAL_SYNC_PARENT_FD", "-1")));
        m_localBuffer.create(atoi(getEnv("SYNCEVOLUTION_LOCAL_SYNC_CHILD_FD", "-1")));
        m_remoteBuffer.map(NULL, NULL);
        pcrecpp::StringPiece remote = m_remoteBuffer.stringPiece();
        if ((size_t)remote.size() != m_messageBufferSize) {
            SE_THROW(StringPrintf("local and remote side do not agree on shared buffer size: %ld != %ld",
                                  (long)m_messageBufferSize, (long)remote.size()));
        }
    }

    pcrecpp::StringPiece getLocalBuffer() { return m_localBuffer.stringPiece(); }
    pcrecpp::StringPiece getRemoteBuffer() { return m_remoteBuffer.stringPiece(); }

    size_t toLocalOffset(const char *data, size_t len)
    {
        if (!len) {
            return 0;
        }

        pcrecpp::StringPiece localBuffer = getLocalBuffer();
        if (data < localBuffer.data() ||
            data + len > localBuffer.data() + localBuffer.size()) {
            SE_THROW("unexpected send buffer");
        }
        return data - localBuffer.data();
    }

private:
    SMLTKSharedMemory()
    {
        smlLibMalloc = sshalloc;
        smlLibFree = sshfree;
    }

    size_t m_messageBufferSize;
    void *m_messageBuffer;
    TmpFile m_localBuffer, m_remoteBuffer;

    static void *sshalloc(size_t size) { return singleton().shalloc(size); }
    void *shalloc(size_t size)
    {
        if (size == m_messageBufferSize) {
            try {
                m_localBuffer.map(&m_messageBuffer, NULL);
                return m_messageBuffer;
            } catch (...) {
                Exception::handle();
                return NULL;
            }
        } else {
            return malloc(size);
        }
    }

    static void sshfree(void *ptr) { return singleton().shfree(ptr); }
    void shfree(void *ptr)
    {
        if (ptr == m_messageBuffer) {
            m_messageBuffer = NULL;
        } else {
            free(ptr);
        }
    }

    void prepareBuffer(TmpFile &tmpfile, size_t bufferSize)
    {
        // Reset buffer, in case it was used before (happens in client-test).
        tmpfile.close();
        tmpfile.unmap();

        tmpfile.create();
        if (ftruncate(tmpfile.getFD(), bufferSize)) {
            SE_THROW(StringPrintf("resizing message buffer file to %ld bytes failed: %s",
                                  (long)bufferSize, strerror(errno)));
        }
        tmpfile.remove();
    }
};


class NoopAgentDestructor
{
public:
    void operator () (TransportAgent *agent) throw() {}
};

LocalTransportAgent::LocalTransportAgent(SyncContext *server,
                                         const std::string &clientConfig,
                                         void *loop) :
    m_server(server),
    m_clientConfig(SyncConfig::normalizeConfigString(clientConfig)),
    m_status(INACTIVE),
    m_loop(loop ?
           GMainLoopCXX(static_cast<GMainLoop *>(loop), ADD_REF) :
           GMainLoopCXX(g_main_loop_new(NULL, false), TRANSFER_REF))
{
    SMLTKSharedMemory::singleton().initParent(server->getMaxMsgSize());
}

boost::shared_ptr<LocalTransportAgent> LocalTransportAgent::create(SyncContext *server,
                                                                   const std::string &clientConfig,
                                                                   void *loop)
{
    boost::shared_ptr<LocalTransportAgent> self(new LocalTransportAgent(server, clientConfig, loop));
    self->m_self = self;
    return self;
}

LocalTransportAgent::~LocalTransportAgent()
{
}

void LocalTransportAgent::start()
{
    // TODO (?): check that there are no conflicts between the active
    // sources. The old "contexts must be different" check achieved that
    // via brute force (because by definition, databases from different
    // contexts are meant to be independent), but it was too coarse
    // and ruled out valid configurations.
    // if (m_clientContext == m_server->getContextName()) {
    //     SE_THROW(StringPrintf("invalid local sync inside context '%s', need second context with different databases", context.c_str()));
    // }

    if (m_forkexec) {
        SE_THROW("local transport already started");
    }
    m_status = ACTIVE;
    m_forkexec = ForkExecParent::create("syncevo-local-sync");
#ifdef USE_DLT
    if (getenv("SYNCEVOLUTION_USE_DLT")) {
        m_forkexec->addEnvVar("SYNCEVOLUTION_USE_DLT", StringPrintf("%d", LoggerDLT::getCurrentDLTLogLevel()));
    }
#endif
    m_forkexec->m_onConnect.connect(boost::bind(&LocalTransportAgent::onChildConnect, this, _1));
    // fatal problems, including quitting child with non-zero status
    m_forkexec->m_onFailure.connect(boost::bind(&LocalTransportAgent::onFailure, this, _2));
    // watch onQuit and remember whether the child is still running,
    // because it might quit prematurely with a zero return code (for
    // example, when an unexpected slow sync is detected)
    m_forkexec->m_onQuit.connect(boost::bind(&LocalTransportAgent::onChildQuit, this, _1));
    m_forkexec->start();
}

/**
 * Uses the D-Bus API provided by LocalTransportParent.
 */
class LocalTransportParent : private GDBusCXX::DBusRemoteObject
{
 public:
    static const char *path() { return "/"; }
    static const char *interface() { return "org.syncevolution.localtransport.parent"; }
    static const char *destination() { return "local.destination"; }
    static const char *askPasswordName() { return "AskPassword"; }
    static const char *storeSyncReportName() { return "StoreSyncReport"; }

    LocalTransportParent(const GDBusCXX::DBusConnectionPtr &conn) :
        GDBusCXX::DBusRemoteObject(conn, path(), interface(), destination()),
        m_askPassword(*this, askPasswordName()),
        m_storeSyncReport(*this, storeSyncReportName())
    {}

    /** LocalTransportAgent::askPassword() */
    GDBusCXX::DBusClientCall1<std::string> m_askPassword;
    /** LocalTransportAgent::storeSyncReport() */
    GDBusCXX::DBusClientCall0 m_storeSyncReport;
};

/**
 * Uses the D-Bus API provided by LocalTransportAgentChild.
 */
class LocalTransportChild : public GDBusCXX::DBusRemoteObject
{
 public:
    static const char *path() { return "/"; }
    static const char *interface() { return "org.syncevolution.localtransport.child"; }
    static const char *destination() { return "local.destination"; }
    static const char *logOutputName() { return "LogOutput"; }
    static const char *setFreezeName() { return "SetFreeze"; }
    static const char *startSyncName() { return "StartSync"; }
    static const char *sendMsgName() { return "SendMsg"; }

    LocalTransportChild(const GDBusCXX::DBusConnectionPtr &conn) :
        GDBusCXX::DBusRemoteObject(conn, path(), interface(), destination()),
        m_logOutput(*this, logOutputName(), false),
        m_setFreeze(*this, setFreezeName()),
        m_startSync(*this, startSyncName()),
        m_sendMsg(*this, sendMsgName())
    {}

    /**
     * information from server config about active sources:
     * mapping is from server source names to child source name + sync mode
     * (again as set on the server side!)
     */
    typedef std::map<std::string, StringPair> ActiveSources_t;
    /** use this to send a message back from child to parent */
    typedef boost::shared_ptr< GDBusCXX::Result3< std::string, size_t, size_t > > ReplyPtr;

    /** log output with level and message; process name will be added by parent */
    GDBusCXX::SignalWatch2<string, string> m_logOutput;

    /** LocalTransportAgentChild::setFreeze() */
    GDBusCXX::DBusClientCall0 m_setFreeze;
    /** LocalTransportAgentChild::startSync() */
    GDBusCXX::DBusClientCall3<std::string, size_t, size_t > m_startSync;
    /** LocalTransportAgentChild::sendMsg() */
    GDBusCXX::DBusClientCall3<std::string, size_t, size_t > m_sendMsg;
};

void LocalTransportAgent::logChildOutput(const std::string &level, const std::string &message)
{
    Logger::MessageOptions options(Logger::strToLevel(level.c_str()));
    options.m_processName = &m_clientConfig;
    // Child should have written this into its own log file and/or syslog/dlt already.
    // Only pass it on to a user of the command line interface.
    options.m_flags = Logger::MessageOptions::ALREADY_LOGGED;
    SyncEvo::Logger::instance().messageWithOptions(options, "%s", message.c_str());
}

void LocalTransportAgent::onChildConnect(const GDBusCXX::DBusConnectionPtr &conn)
{
    SE_LOG_DEBUG(NULL, "child is ready");
    m_parent.reset(new GDBusCXX::DBusObjectHelper(conn,
                                                  LocalTransportParent::path(),
                                                  LocalTransportParent::interface(),
                                                  GDBusCXX::DBusObjectHelper::Callback_t(),
                                                  true));
    m_parent->add(this, &LocalTransportAgent::askPassword, LocalTransportParent::askPasswordName());
    m_parent->add(this, &LocalTransportAgent::storeSyncReport, LocalTransportParent::storeSyncReportName());
    m_parent->activate();
    m_child.reset(new LocalTransportChild(conn));
    m_child->m_logOutput.activate(boost::bind(&LocalTransportAgent::logChildOutput, this, _1, _2));

    // now tell child what to do
    LocalTransportChild::ActiveSources_t sources;
    BOOST_FOREACH(const string &sourceName, m_server->getSyncSources()) {
        SyncSourceNodes nodes = m_server->getSyncSourceNodesNoTracking(sourceName);
        SyncSourceConfig source(sourceName, nodes);
        std::string sync = source.getSync();
        if (sync != "disabled") {
            string targetName = source.getURINonEmpty();
            sources[sourceName] = std::make_pair(targetName, sync);
        }
    }

    // Some sync properties come from the originating sync config.
    // They might have been set temporarily, so we have to read them
    // here. We must ensure that this value is used, even if unset.
    FullProps props = m_server->getConfigProps();
    props[""].m_syncProps[SyncMaxMsgSize] = StringPrintf("%lu", m_server->getMaxMsgSize().get());
    // TODO: also handle "preventSlowSync" like this. Currently it must
    // be set in the target sync config. For backward compatibility we
    // must disable slow sync when it is set on either side.

    m_child->m_startSync.start(m_clientConfig,
                               StringPair(m_server->getConfigName(),
                                          m_server->isEphemeral() ?
                                          "ephemeral" :
                                          m_server->getRootPath()),
                               static_cast<std::string>(m_server->getLogDir()),
                               m_server->getDoLogging(),
                               std::make_pair(m_server->getSyncUser(),
                                              m_server->getSyncPassword()),
                               props,
                               sources,
                               boost::bind(&LocalTransportAgent::storeReplyMsg, m_self, _1, _2, _3, _4));
}

void LocalTransportAgent::onFailure(const std::string &error)
{
    m_status = FAILED;
    g_main_loop_quit(m_loop.get());

    SE_LOG_ERROR(NULL, "local transport failed: %s", error.c_str());
    m_parent.reset();
    m_child.reset();
}

void LocalTransportAgent::onChildQuit(int status)
{
    SE_LOG_DEBUG(NULL, "child process has quit with status %d", status);
    g_main_loop_quit(m_loop.get());
}

static void GotPassword(const boost::shared_ptr< GDBusCXX::Result1<const std::string &> > &reply,
                        const std::string &password)
{
    reply->done(password);
}

static void PasswordException(const boost::shared_ptr< GDBusCXX::Result1<const std::string &> > &reply)
{
    // TODO: refactor, this is the same as dbusErrorCallback
    try {
        // If there is no pending exception, the process will abort
        // with "terminate called without an active exception";
        // dbusErrorCallback() should only be called when there is
        // a pending exception.
        // TODO: catch this misuse in a better way
        throw;
    } catch (...) {
        // let D-Bus parent log the error
        std::string explanation;
        Exception::handle(explanation, HANDLE_EXCEPTION_NO_ERROR);
        reply->failed(GDBusCXX::dbus_error("org.syncevolution.localtransport.error", explanation));
    }
}

void LocalTransportAgent::askPassword(const std::string &passwordName,
                                      const std::string &descr,
                                      const ConfigPasswordKey &key,
                                      const boost::shared_ptr< GDBusCXX::Result1<const std::string &> > &reply)
{
    // pass that work to our own SyncContext and its UI - currently blocks
    SE_LOG_DEBUG(NULL, "local sync parent: asked for password %s, %s",
                 passwordName.c_str(),
                 descr.c_str());
    try {
        if (m_server) {
            m_server->getUserInterfaceNonNull().askPasswordAsync(passwordName, descr, key,
                                                                 // TODO refactor: use dbus-callbacks.h
                                                                 boost::bind(GotPassword,
                                                                             reply,
                                                                             _1),
                                                                 boost::bind(PasswordException,
                                                                             reply));
        } else {
            SE_LOG_DEBUG(NULL, "local sync parent: password request failed because no m_server");
            reply->failed(GDBusCXX::dbus_error("org.syncevolution.localtransport.error",
                                               "not connected to UI"));
        }
    } catch (...) {
        PasswordException(reply);
    }
}

void LocalTransportAgent::storeSyncReport(const std::string &report)
{
    SE_LOG_DEBUG(NULL, "got child sync report:\n%s",
                 report.c_str());
    m_clientReport = SyncReport(report);
}

void LocalTransportAgent::getClientSyncReport(SyncReport &report)
{
    report = m_clientReport;
}

void LocalTransportAgent::setContentType(const std::string &type)
{
    m_contentType = type;
}

// workaround for limitations of bind+signals when used together with plain GMainLoop pointer
// (pointer to undefined struct)
static void gMainLoopQuit(GMainLoopCXX *loop)
{
    g_main_loop_quit(loop->get());
}

void LocalTransportAgent::shutdown()
{
    SE_LOG_DEBUG(NULL, "parent is shutting down");
    if (m_forkexec) {
        // block until child is done
        boost::signals2::scoped_connection c(m_forkexec->m_onQuit.connect(boost::bind(gMainLoopQuit,
                                                                                      &m_loop)));
        // don't kill the child here - we expect it to complete by
        // itself at some point
        // TODO: how do we detect a child which gets stuck after its last
        // communication with the parent?
        // m_forkexec->stop();
        while (m_forkexec->getState() != ForkExecParent::TERMINATED) {
            SE_LOG_DEBUG(NULL, "waiting for child to stop");
            g_main_loop_run(m_loop.get());
        }

        m_forkexec.reset();
        m_parent.reset();
        m_child.reset();
    }
}

void LocalTransportAgent::setFreeze(bool freeze)
{
    // Relay to other side, check for error exception synchronously.
    if (m_child) {
        m_child->m_setFreeze(freeze);
    }
}

void LocalTransportAgent::send(const char *data, size_t len)
{
    if (m_child) {
        size_t offset = SMLTKSharedMemory::singleton().toLocalOffset(data, len);
        m_status = ACTIVE;
        m_child->m_sendMsg.start(m_contentType, offset, len,
                                 boost::bind(&LocalTransportAgent::storeReplyMsg, m_self, _1, _2, _3, _4));
    } else {
        m_status = FAILED;
        SE_THROW_EXCEPTION(TransportException,
                           "cannot send message because child process is gone");
    }
}

void LocalTransportAgent::storeReplyMsg(const std::string &contentType,
                                        size_t offset, size_t len,
                                        const std::string &error)
{
    pcrecpp::StringPiece remoteBuffer = SMLTKSharedMemory::singleton().getRemoteBuffer();
    m_replyMsg.set(remoteBuffer.data() + offset, len);
    m_replyContentType = contentType;
    if (error.empty()) {
        m_status = GOT_REPLY;
    } else {
        // Only an error if the client hasn't shut down normally.
        if (m_clientReport.empty()) {
            SE_LOG_ERROR(NULL, "sending message to child failed: %s", error.c_str());
            m_status = FAILED;
        }
    }
    g_main_loop_quit(m_loop.get());
}

void LocalTransportAgent::cancel()
{
    if (m_forkexec) {
        SE_LOG_DEBUG(NULL, "killing local transport child in cancel()");
        m_forkexec->stop();
    }
    m_status = CANCELED;
}

TransportAgent::Status LocalTransportAgent::wait(bool noReply)
{
    if (m_status == ACTIVE) {
        // need next message; for noReply == true we are done
        if (noReply) {
            m_status = INACTIVE;
        } else {
            while (m_status == ACTIVE) {
                SE_LOG_DEBUG(NULL, "waiting for child to send message");
                if (m_forkexec &&
                    m_forkexec->getState() == ForkExecParent::TERMINATED) {
                    m_status = FAILED;
                    if (m_clientReport.getStatus() != STATUS_OK &&
                        m_clientReport.getStatus() != STATUS_HTTP_OK) {
                        // Report that status, with an error message which contains the explanation
                        // added to the client's error. We are a bit fuzzy about matching the status:
                        // 10xxx matches xxx and vice versa.
                        int status = m_clientReport.getStatus();
                        if (status >= sysync::LOCAL_STATUS_CODE && status <= sysync::LOCAL_STATUS_CODE_END) {
                            status -= sysync::LOCAL_STATUS_CODE;
                        }
                        std::string explanation = StringPrintf("failure on target side %s of local sync",
                                                               m_clientConfig.c_str());
                        static const pcrecpp::RE re("\\((?:local|remote), status (\\d+)\\): (.*)");
                        int clientStatus;
                        std::string clientExplanation;
                        if (re.PartialMatch(m_clientReport.getError(), &clientStatus, &clientExplanation) &&
                            (status == clientStatus ||
                             status == clientStatus - sysync::LOCAL_STATUS_CODE)) {
                            explanation += ": ";
                            explanation += clientExplanation;
                        }
                        SE_THROW_EXCEPTION_STATUS(StatusException,
                                                  explanation,
                                                  m_clientReport.getStatus());
                    } else {
                        SE_THROW_EXCEPTION(TransportException,
                                           "child process quit without sending its message");
                    }
                }
                g_main_loop_run(m_loop.get());
            }
        }
    }
    return m_status;
}

void LocalTransportAgent::getReply(const char *&data, size_t &len, std::string &contentType)
{
    if (m_status != GOT_REPLY) {
        SE_THROW("internal error, no reply available");
    }
    contentType = m_replyContentType;
    data = m_replyMsg.data();
    len = m_replyMsg.size();
}

void LocalTransportAgent::setTimeout(int seconds)
{
    // setTimeout() was meant for unreliable transports like HTTP
    // which cannot determine whether the peer is still alive. The
    // LocalTransportAgent uses sockets and will notice when a peer
    // dies unexpectedly, so timeouts should never be necessary.
    //
    // Quite the opposite, because the "client" in a local sync
    // with WebDAV on the client side can be quite slow, incorrect
    // timeouts were seen where the client side took longer than
    // the default timeout of 5 minutes to process a message and
    // send a reply.
    //
    // Therefore we ignore the request to set a timeout here and thus
    // local send/receive operations are allowed to continue for as
    // long as they like.
    //
    // m_timeoutSeconds = seconds;
}

class LocalTransportUI : public UserInterface
{
    boost::shared_ptr<LocalTransportParent> m_parent;

public:
    LocalTransportUI(const boost::shared_ptr<LocalTransportParent> &parent) :
        m_parent(parent)
    {}

    /** implements password request by asking the parent via D-Bus */
    virtual string askPassword(const string &passwordName,
                               const string &descr,
                               const ConfigPasswordKey &key)
    {
        SE_LOG_DEBUG(NULL, "local transport child: requesting password %s, %s via D-Bus",
                     passwordName.c_str(),
                     descr.c_str());
        std::string password;
        std::string error;
        bool havePassword = false;
        m_parent->m_askPassword.start(passwordName, descr, key,
                                      boost::bind(&LocalTransportUI::storePassword, this,
                                                  boost::ref(password), boost::ref(error),
                                                  boost::ref(havePassword),
                                                  _1, _2));
        SuspendFlags &s = SuspendFlags::getSuspendFlags();
        while (!havePassword) {
            if (s.getState() != SuspendFlags::NORMAL) {
                SE_THROW_EXCEPTION_STATUS(StatusException,
                                          StringPrintf("User did not provide the '%s' password.",
                                                       passwordName.c_str()),
                                          SyncMLStatus(sysync::LOCERR_USERABORT));
            }
            g_main_context_iteration(NULL, true);
        }
        if (!error.empty()) {
            Exception::tryRethrowDBus(error);
            SE_THROW(StringPrintf("retrieving password failed: %s", error.c_str()));
        }
        return password;
    }

    virtual bool savePassword(const std::string &passwordName, const std::string &password, const ConfigPasswordKey &key) { SE_THROW("not implemented"); return false; }
    virtual void readStdin(std::string &content) { SE_THROW("not implemented"); }

private:
    void storePassword(std::string &res, std::string &errorRes, bool &haveRes, const std::string &password, const std::string &error)
    {
        if (!error.empty()) {
            SE_LOG_DEBUG(NULL, "local transport child: D-Bus password request failed: %s",
                         error.c_str());
            errorRes = error;
        } else {
            SE_LOG_DEBUG(NULL, "local transport child: D-Bus password request succeeded");
            res = password;
        }
        haveRes = true;
    }
};

static void abortLocalSync(int sigterm)
{
    // logging anything here is not safe (our own logging system might
    // have been interrupted by the SIGTERM and thus be in an inconsistent
    // state), but let's try it anyway
    SE_LOG_INFO(NULL, "local sync child shutting down due to SIGTERM");
    // raise the signal again after disabling the handler, to ensure that
    // the exit status is "killed by signal xxx" - good because then
    // the whoever killed used gets the information that we didn't die for
    // some other reason
    signal(sigterm, SIG_DFL);
    raise(sigterm);
}

/**
 * Provides the "LogOutput" signal.
 * LocalTransportAgentChild adds the method implementations
 * before activating it.
 */
class LocalTransportChildImpl : public GDBusCXX::DBusObjectHelper
{
public:
    LocalTransportChildImpl(const GDBusCXX::DBusConnectionPtr &conn) :
        GDBusCXX::DBusObjectHelper(conn,
                                   LocalTransportChild::path(),
                                   LocalTransportChild::interface(),
                                   GDBusCXX::DBusObjectHelper::Callback_t(),
                                   true),
        m_logOutput(*this, LocalTransportChild::logOutputName())
    {
        add(m_logOutput);
    };

    GDBusCXX::EmitSignal2<std::string,
                          std::string,
                          true /* ignore transmission failures */> m_logOutput;
};

class ChildLogger : public Logger
{
    std::auto_ptr<LogRedirect> m_parentLogger;
    boost::weak_ptr<LocalTransportChildImpl> m_child;

public:
    ChildLogger(const boost::shared_ptr<LocalTransportChildImpl> &child) :
        m_parentLogger(new LogRedirect(LogRedirect::STDERR_AND_STDOUT)),
        m_child(child)
    {}
    ~ChildLogger()
    {
        m_parentLogger.reset();
    }

    /**
     * Write message into our own log and send to parent.
     */
    virtual void messagev(const MessageOptions &options,
                          const char *format,
                          va_list args)
    {
        if (options.m_level <= m_parentLogger->getLevel()) {
            m_parentLogger->process();
            boost::shared_ptr<LocalTransportChildImpl> child = m_child.lock();
            if (child) {
                // prefix is used to set session path
                // for general server output, the object path field is dbus server
                // the object path can't be empty for object paths prevent using empty string.
                string strLevel = Logger::levelToStr(options.m_level);
                string log = StringPrintfV(format, args);
                child->m_logOutput(strLevel, log);
            }
        }
    }
};

class LocalTransportAgentChild : public TransportAgent
{
    /** final return code of our main(): non-zero indicates that we need to shut down */
    int m_ret;

    /**
     * sync report for client side of the local sync
     */
    SyncReport m_clientReport;

    /** used to capture libneon output */
    boost::scoped_ptr<LogRedirect> m_parentLogger;

    /**
     * provides connection to parent, created in constructor
     */
    boost::shared_ptr<ForkExecChild> m_forkexec;

    /**
     * proxy for the parent's D-Bus API in onConnect()
     */
    boost::shared_ptr<LocalTransportParent> m_parent;

    /**
     * our D-Bus interface, created in onConnect()
     */
    boost::shared_ptr<LocalTransportChildImpl> m_child;

    /**
     * sync context, created in Sync() D-Bus call
     */
    boost::scoped_ptr<SyncContext> m_client;

    /**
     * use this D-Bus result handle to send a message from child to parent
     * in response to sync() or (later) sendMsg()
     */
    LocalTransportChild::ReplyPtr m_msgToParent;
    void setMsgToParent(const LocalTransportChild::ReplyPtr &reply,
                        const std::string &reason)
    {
        if (m_msgToParent) {
            m_msgToParent->failed(GDBusCXX::dbus_error("org.syncevolution.localtransport.error",
                                                       "cancelling message: " + reason));
        }
        m_msgToParent = reply;
    }

    /** content type for message to parent */
    std::string m_contentType;

    /**
     * message from parent in the shared memory buffer
     */
    pcrecpp::StringPiece m_message;

    /**
     * content type of message from parent
     */
    std::string m_messageType;

    /**
     * true after parent has received sync report, or sending failed
     */
    bool m_reportSent;

    /**
     * INACTIVE when idle,
     * ACTIVE after having sent and while waiting for next message,
     * GOT_REPLY when we have a message to be processed,
     * FAILED when permanently broken
     */
    Status m_status;

    /**
     * one loop run + error checking
     */
    void step(const std::string &status)
    {
        SE_LOG_DEBUG(NULL, "local transport: %s", status.c_str());
        if (!m_forkexec ||
            m_forkexec->getState() == ForkExecChild::DISCONNECTED) {
            SE_THROW("local transport child no longer has a parent, terminating");
        }
        g_main_context_iteration(NULL, true);
        if (m_ret) {
            SE_THROW("local transport child encountered a problem, terminating");
        }
    }

    static void onParentQuit()
    {
        // Never free this state blocker. We can only abort and
        // quit from now on.
        static boost::shared_ptr<SuspendFlags::StateBlocker> abortGuard;
        SE_LOG_ERROR(NULL, "sync parent quit unexpectedly");
        abortGuard = SuspendFlags::getSuspendFlags().abort();
    }

    void onConnect(const GDBusCXX::DBusConnectionPtr &conn)
    {
        SE_LOG_DEBUG(NULL, "child connected to parent");

        // provide our own API
        m_child.reset(new LocalTransportChildImpl(conn));
        m_child->add(this, &LocalTransportAgentChild::setFreezeLocalSync, LocalTransportChild::setFreezeName());
        m_child->add(this, &LocalTransportAgentChild::startSync, LocalTransportChild::startSyncName());
        m_child->add(this, &LocalTransportAgentChild::sendMsg, LocalTransportChild::sendMsgName());
        m_child->activate();

        // set up connection to parent
        m_parent.reset(new LocalTransportParent(conn));
    }

    void onFailure(SyncMLStatus status, const std::string &reason)
    {
        SE_LOG_DEBUG(NULL, "child fork/exec failed: %s", reason.c_str());

        // record failure for parent
        if (!m_clientReport.getStatus()) {
            m_clientReport.setStatus(status);
        }
        if (!reason.empty() &&
            m_clientReport.getError().empty()) {
            m_clientReport.setError(reason);
        }

        // return to step()
        m_ret = 1;
    }

    // D-Bus API, see LocalTransportChild;
    // must keep number of parameters < 9, the maximum supported by
    // our D-Bus binding
    void startSync(const std::string &clientConfig,
                   const StringPair &serverConfig, // config name + root path
                   const std::string &serverLogDir,
                   bool serverDoLogging,
                   const std::pair<UserIdentity, InitStateString> &serverSyncCredentials,
                   const FullProps &serverConfigProps,
                   const LocalTransportChild::ActiveSources_t &sources,
                   const LocalTransportChild::ReplyPtr &reply)
    {
        setMsgToParent(reply, "sync() was called");

        string peer, context, normalConfig;
        normalConfig = SyncConfig::normalizeConfigString(clientConfig);
        SyncConfig::splitConfigString(normalConfig, peer, context);
        if (peer.empty()) {
            peer = "target-config";
        }

        // Keep the process name short in debug output if it is the
        // normal "target-config", be more verbose if it is something
        // else because it may be relevant.
        if (peer != "target-config") {
            Logger::setProcessName(peer + "@" + context);
        } else {
            Logger::setProcessName("@" + context);
        }

        SE_LOG_DEBUG(NULL, "Sync() called, starting the sync");
        const char *delay = getenv("SYNCEVOLUTION_LOCAL_CHILD_DELAY2");
        if (delay) {
            Sleep(atoi(delay));
        }

        // initialize sync context
        m_client.reset(new SyncContext(peer + "@" + context,
                                       serverConfig.first,
                                       serverConfig.second == "ephemeral" ?
                                       serverConfig.second :
                                       serverConfig.second + "/." + normalConfig,
                                       boost::shared_ptr<TransportAgent>(this, NoopAgentDestructor()),
                                       serverDoLogging));
        if (serverConfig.second == "ephemeral") {
            m_client->makeEphemeral();
        }
        boost::shared_ptr<UserInterface> ui(new LocalTransportUI(m_parent));
        m_client->setUserInterface(ui);

        // allow proceeding with sync even if no "target-config" was created,
        // because information about username/password (for WebDAV) or the
        // sources (for file backends) might be enough
        m_client->setConfigNeeded(false);

        // apply temporary config filters
        m_client->setConfigFilter(true, "", serverConfigProps.createSyncFilter(m_client->getConfigName()));
        BOOST_FOREACH(const string &sourceName, m_client->getSyncSources()) {
            m_client->setConfigFilter(false, sourceName, serverConfigProps.createSourceFilter(m_client->getConfigName(), sourceName));
        }

        // With the config in place, initialize message passing.
        SMLTKSharedMemory::singleton().initChild(m_client->getMaxMsgSize());

        // Copy non-empty credentials from main config, because
        // that is where the GUI knows how to store them. A better
        // solution would be to require that credentials are in the
        // "target-config" config.
        //
        // Interactive password requests later in SyncContext::sync()
        // will end up in our LocalTransportContext::askPassword()
        // implementation above, which will pass the question to
        // the local sync parent.
        if (!serverSyncCredentials.first.toString().empty()) {
            m_client->setSyncUsername(serverSyncCredentials.first.toString(), true);
        }
        if (!serverSyncCredentials.second.empty()) {
            m_client->setSyncPassword(serverSyncCredentials.second, true);
        }

        // debugging mode: write logs inside sub-directory of parent,
        // otherwise use normal log settings
        if (!serverDoLogging) {
            m_client->setLogDir(std::string(serverLogDir) + "/child", true);
        }

        // disable all sources temporarily, will be enabled by next loop
        BOOST_FOREACH(const string &targetName, m_client->getSyncSources()) {
            SyncSourceNodes targetNodes = m_client->getSyncSourceNodes(targetName);
            SyncSourceConfig targetSource(targetName, targetNodes);
            targetSource.setSync("disabled", true);
        }

        // activate all sources in client targeted by main config,
        // with right uri
        BOOST_FOREACH(const LocalTransportChild::ActiveSources_t::value_type &entry, sources) {
            // mapping is from server (source) to child (target)
            const std::string &sourceName = entry.first;
            const std::string &targetName = entry.second.first;
            std::string sync = entry.second.second;
            SyncMode mode = StringToSyncMode(sync);
            if (mode != SYNC_NONE) {
                SyncSourceNodes targetNodes = m_client->getSyncSourceNodes(targetName);
                SyncSourceConfig targetSource(targetName, targetNodes);
                string fullTargetName = normalConfig + "/" + targetName;

                if (!targetNodes.dataConfigExists()) {
                    if (targetName.empty()) {
                        Exception::throwError(SE_HERE, "missing URI for one of the datastores");
                    } else {
                        Exception::throwError(SE_HERE, StringPrintf("%s: datastore not configured",
                                                                    fullTargetName.c_str()));
                    }
                }

                // All of the config setting is done as volatile,
                // so none of the regular config nodes have to
                // be written. If a sync mode was set, it must have been
                // done before in this loop => error in original config.
                if (!targetSource.isDisabled()) {
                    Exception::throwError(SE_HERE,
                                          StringPrintf("%s: datastore targetted twice by %s",
                                                       fullTargetName.c_str(),
                                                       serverConfig.first.c_str()));
                }
                // invert data direction
                if (mode == SYNC_REFRESH_FROM_LOCAL) {
                    mode = SYNC_REFRESH_FROM_REMOTE;
                } else if (mode == SYNC_REFRESH_FROM_REMOTE) {
                    mode = SYNC_REFRESH_FROM_LOCAL;
                } else if (mode == SYNC_ONE_WAY_FROM_LOCAL) {
                    mode = SYNC_ONE_WAY_FROM_REMOTE;
                } else if (mode == SYNC_ONE_WAY_FROM_REMOTE) {
                    mode = SYNC_ONE_WAY_FROM_LOCAL;
                } else if (mode == SYNC_LOCAL_CACHE_SLOW) {
                    // Remote side is running in caching mode and
                    // asking for refresh. Send all our data.
                    mode = SYNC_SLOW;
                } else if (mode == SYNC_LOCAL_CACHE_INCREMENTAL) {
                    // Remote side is running in caching mode and
                    // asking for an update. Use two-way mode although
                    // nothing is going to come back (simpler that way
                    // than using one-way, which has special code
                    // paths in libsynthesis).
                    mode = SYNC_TWO_WAY;
                }
                targetSource.setSync(PrettyPrintSyncMode(mode, true), true);
                targetSource.setURI(sourceName, true);
            }
        }


        // ready for m_client->sync()
        m_status = ACTIVE;
    }

    void sendMsg(const std::string &contentType,
                 size_t offset, size_t len,
                 const LocalTransportChild::ReplyPtr &reply)
    {
        SE_LOG_DEBUG(NULL, "child got message of %ld bytes", (long)len);
        setMsgToParent(LocalTransportChild::ReplyPtr(), "sendMsg() was called");
        if (m_status == ACTIVE) {
            m_msgToParent = reply;
            pcrecpp::StringPiece remoteBuffer = SMLTKSharedMemory::singleton().getRemoteBuffer();
            m_message.set(remoteBuffer.data() + offset, len);
            m_messageType = contentType;
            m_status = GOT_REPLY;
        } else {
            reply->failed(GDBusCXX::dbus_error("org.syncevolution.localtransport.error",
                                               "child not expecting any message"));
        }
    }

    // Must not be named setFreeze(), that is a virtual method in
    // TransportAgent that we don't want to override!
    void setFreezeLocalSync(bool freeze)
    {
        SE_LOG_DEBUG(NULL, "local transport child: setFreeze(%s)", freeze ? "true" : "false");
        if (m_client) {
            m_client->setFreeze(freeze);
        }
    }

public:
    LocalTransportAgentChild() :
        m_ret(0),
        m_forkexec(SyncEvo::ForkExecChild::create()),
        m_reportSent(false),
        m_status(INACTIVE)
    {
        m_forkexec->m_onConnect.connect(boost::bind(&LocalTransportAgentChild::onConnect, this, _1));
        m_forkexec->m_onFailure.connect(boost::bind(&LocalTransportAgentChild::onFailure, this, _1, _2));
        // When parent quits, we need to abort whatever we do and shut
        // down. There's no way how we can complete our work without it.
        //
        // Note that another way how this process can detect the
        // death of the parent is when it currently is waiting for
        // completion of a method call to the parent, like a request
        // for a password. However, that does not cover failures
        // like the parent not asking us to sync in the first place
        // and also does not work with libdbus (https://bugs.freedesktop.org/show_bug.cgi?id=49728).
        m_forkexec->m_onQuit.connect(&onParentQuit);

        m_forkexec->connect();
    }

    boost::shared_ptr<ChildLogger> createLogger()
    {
        return boost::shared_ptr<ChildLogger>(new ChildLogger(m_child));
    }

    void run()
    {
        SuspendFlags &s = SuspendFlags::getSuspendFlags();

        while (!m_parent) {
            if (s.getState() != SuspendFlags::NORMAL) {
                SE_LOG_DEBUG(NULL, "aborted, returning while waiting for parent");
                return;
            }
            step("waiting for parent");
        }
        while (!m_client) {
            if (s.getState() != SuspendFlags::NORMAL) {
                SE_LOG_DEBUG(NULL, "aborted, returning while waiting for Sync() call from parent");
            }
            step("waiting for Sync() call from parent");
        }
        try {
            // ignore SIGINT signal in local sync helper from now on:
            // the parent process will handle those and tell us when
            // we are expected to abort by sending a SIGTERM
            struct sigaction new_action;
            memset(&new_action, 0, sizeof(new_action));
            new_action.sa_handler = SIG_IGN;
            sigemptyset(&new_action.sa_mask);
            sigaction(SIGINT, &new_action, NULL);

            // SIGTERM would be caught by SuspendFlags and set the "abort"
            // state. But a lot of code running in this process cannot
            // check that flag in a timely manner (blocking calls in
            // libneon, activesync client libraries, ...). Therefore
            // it is better to abort inside the signal handler.
            new_action.sa_handler = abortLocalSync;
            sigaction(SIGTERM, &new_action, NULL);

            SE_LOG_DEBUG(NULL, "LocalTransportChild: ignore SIGINT, die in SIGTERM");
            SE_LOG_INFO(NULL, "target side of local sync ready");
            m_client->sync(&m_clientReport);
        } catch (...) {
            string explanation;
            SyncMLStatus status = Exception::handle(explanation);
            m_clientReport.setStatus(status);
            if (!explanation.empty() &&
                m_clientReport.getError().empty()) {
                m_clientReport.setError(explanation);
            }
            if (m_parent) {
                std::string report = m_clientReport.toString();
                SE_LOG_DEBUG(NULL, "child sending sync report after failure:\n%s", report.c_str());
                m_parent->m_storeSyncReport.start(report,
                                                  boost::bind(&LocalTransportAgentChild::syncReportReceived, this, _1));
                // wait for acknowledgement for report once:
                // we are in some kind of error state, better
                // do not wait too long
                if (m_parent) {
                    SE_LOG_DEBUG(NULL, "waiting for parent's ACK for sync report");
                    g_main_context_iteration(NULL, true);
                }
            }
            throw;
        }

        if (m_parent) {
            // send final report, ignore result
            std::string report = m_clientReport.toString();
            SE_LOG_DEBUG(NULL, "child sending sync report:\n%s", report.c_str());
            m_parent->m_storeSyncReport.start(report,
                                              boost::bind(&LocalTransportAgentChild::syncReportReceived, this, _1));
            while (!m_reportSent && m_parent &&
                   s.getState() == SuspendFlags::NORMAL) {
                step("waiting for parent's ACK for sync report");
            }
        }
    }

    void syncReportReceived(const std::string &error)
    {
        SE_LOG_DEBUG(NULL, "sending sync report to parent: %s",
                     error.empty() ? "done" : error.c_str());
        m_reportSent = true;
    }

    int getReturnCode() const { return m_ret; }

    /**
     * set transport specific URL of next message
     */
    virtual void setURL(const std::string &url) {}

    /**
     * define content type for post, see content type constants
     */
    virtual void setContentType(const std::string &type)
    {
        m_contentType = type;
    }

    /**
     * Requests an normal shutdown of the transport. This can take a
     * while, for example if communication is still pending.
     * Therefore wait() has to be called to ensure that the
     * shutdown is complete and that no error occurred.
     *
     * Simply deleting the transport is an *unnormal* shutdown that
     * does not communicate with the peer.
     */
    virtual void shutdown()
    {
        SE_LOG_DEBUG(NULL, "child local transport shutting down");
        if (m_msgToParent) {
            // Must send non-zero message, empty messages cause an
            // error during D-Bus message decoding on the receiving
            // side. Content doesn't matter, ignored by parent.
            m_msgToParent->done("shutdown-message", (size_t)0, (size_t)0);
            m_msgToParent.reset();
        }
        if (m_status != FAILED) {
            m_status = CLOSED;
        }
    }

    /**
     * start sending message
     *
     * Memory must remain valid until reply is received or
     * message transmission is canceled.
     *
     * @param data      start address of data to send
     * @param len       number of bytes
     */
    virtual void send(const char *data, size_t len)
    {
        SE_LOG_DEBUG(NULL, "child local transport sending %ld bytes", (long)len);
        if (m_msgToParent) {
            size_t offset = SMLTKSharedMemory::singleton().toLocalOffset(data, len);
            m_status = ACTIVE;
            m_msgToParent->done(m_contentType, offset, len);
            m_msgToParent.reset();
        } else {
            m_status = FAILED;
            SE_THROW("cannot send data to parent because parent is not waiting for message");
        }
    }

    /**
     * cancel an active message transmission
     *
     * Blocks until send buffer is no longer in use.
     * Returns immediately if nothing pending.
     */
    virtual void cancel() {}


    /**
     * Wait for completion of an operation initiated earlier.
     * The operation can be a send with optional reply or
     * a close request.
     *
     * Returns immediately if no operations is pending.
     *
     * @param noReply    true if no reply is required for a running send;
     *                   only relevant for transports used by a SyncML server
     */
    virtual Status wait(bool noReply = false)
    {
        SuspendFlags &s = SuspendFlags::getSuspendFlags();
        while (m_status == ACTIVE &&
               s.getState() == SuspendFlags::NORMAL) {
            step("waiting for next message");
        }
        return m_status;
    }

    /**
     * Tells the transport agent to stop the transmission the given
     * amount of seconds after send() was called. The transport agent
     * will then stop the message transmission and return a TIME_OUT
     * status in wait().
     *
     * @param seconds      number of seconds to wait before timing out, zero for no timeout
     */
    virtual void setTimeout(int seconds) {}

    /**
     * provides access to reply data
     *
     * Memory pointer remains valid as long as
     * transport agent is not deleted and no other
     * message is sent.
     */
    virtual void getReply(const char *&data, size_t &len, std::string &contentType)
    {
        SE_LOG_DEBUG(NULL, "processing %ld bytes in child", (long)m_message.size());
        if (m_status != GOT_REPLY) {
            SE_THROW("getReply() called in child when no reply available");
        }
        data = m_message.data();
        len = m_message.size();
        contentType = m_messageType;
    }
};

int LocalTransportMain(int argc, char **argv)
{
    // delay the client for debugging purposes
    const char *delay = getenv("SYNCEVOLUTION_LOCAL_CHILD_DELAY");
    if (delay) {
        Sleep(atoi(delay));
    }

    SyncContext::initMain("syncevo-local-sync");

    // Our stderr is either connected to the original stderr (when
    // SYNCEVOLUTION_DEBUG is set) or the local sync's parent
    // LogRedirect. However, that stderr is not normally used.
    // Instead we install our own LogRedirect for both stdout (for
    // Execute() and synccompare, which then knows that it needs to
    // capture the output) and stderr (to get output like the one from
    // libneon into the child log) in LocalTransportAgentChild and
    // send all logging output to the local sync parent via D-Bus, to
    // be forwarded to the user as part of the normal message stream
    // of the sync session.
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    // SIGPIPE must be ignored, some system libs (glib GIO?) trigger
    // it. SIGINT/TERM will be handled via SuspendFlags once the sync
    // runs.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    try {
        if (getenv("SYNCEVOLUTION_DEBUG")) {
            Logger::instance().setLevel(Logger::DEBUG);
        }
        // process name will be set to target config name once it is known
        Logger::setProcessName("syncevo-local-sync");

        boost::shared_ptr<LocalTransportAgentChild> child(new LocalTransportAgentChild);
        PushLogger<Logger> logger;
        // Temporary handle is necessary to avoid compiler issue with
        // clang (ambiguous brackets).
        {
            Logger::Handle handle(child->createLogger());
            logger.reset(handle);
        }

#ifdef USE_DLT
        // Set by syncevo-dbus-server for us.
        bool useDLT = getenv("SYNCEVOLUTION_USE_DLT") != NULL;
        PushLogger<LoggerDLT> loggerdlt;
        if (useDLT) {
            loggerdlt.reset(new LoggerDLT(DLT_SYNCEVO_LOCAL_HELPER_ID, "SyncEvolution local sync helper"));
        }
#endif

        child->run();
        int ret = child->getReturnCode();
        logger.reset();
        child.reset();
        return ret;
    } catch ( const std::exception &ex ) {
        SE_LOG_ERROR(NULL, "%s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, "unknown error");
    }

    return 1;
}

SE_END_CXX
