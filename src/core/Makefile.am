AM_CPPFLAGS = @BACKEND_CPPFLAGS@ @GLIB_CFLAGS@ @FUNAMBOL_CFLAGS@

# applies to sources in SyncEvolution repository, but not
# the Funambol C++ client library
SYNCEVOLUTION_CXXFLAGS = @SYNCEVOLUTION_CXXFLAGS@

SYNCEVOLUTION_LDADD = @SYNCEVOLUTION_LDADD@
MAINTAINERCLEANFILES = Makefile.in
EXTRA_DIST = src/core/scitbx/LICENSE_2_0.txt

pkgpyexec_LTLIBRARIES =
pkglib_LTLIBRARIES =
noinst_LTLIBRARIES =

if ENABLE_PYTHON
pkgpyexec_LTLIBRARIES += syncevolution.la
endif

if ENABLE_MODULES
pkglib_LTLIBRARIES += libsyncevolution.la
else
noinst_LTLIBRARIES += libsyncevolution.la
endif

VOCL_SOURCES = \
	vocl/VObject.h \
	vocl/VProperty.h \
	vocl/VConverter.h \
	vocl/posixadapter.h \
	\
	vocl/VObject.cpp \
	vocl/VProperty.cpp \
	vocl/VConverter.cpp

CORE_SOURCES  = \
	ConfigTree.h \
	ConfigNode.h \
	VolatileConfigNode.h \
	VolatileConfigTree.h \
        EvolutionSmartPtr.h \
	\
	SyncEvolutionUtil.cpp \
	SyncEvolutionUtil.h \
        \
	SyncEvolutionCmdline.cpp \
        SyncEvolutionCmdline.h \
	\
        EvolutionSyncSource.h \
        EvolutionSyncSource.cpp \
	\
        EvolutionSyncClient.h \
	EvolutionSyncClient.cpp \
	\
	SyncEvolutionConfig.h \
	SyncEvolutionConfig.cpp \
	\
	FilterConfigNode.h \
	FilterConfigNode.cpp \
	\
	SafeConfigNode.h \
	SafeConfigNode.cpp \
	\
	PrefixConfigNode.h \
	PrefixConfigNode.cpp \
	\
	FileConfigNode.h \
	FileConfigNode.cpp \
	\
	FileConfigTree.h \
	FileConfigTree.cpp \
	\
	TrackingSyncSource.h \
	TrackingSyncSource.cpp \
	\
	$(VOCL_SOURCES)

libsyncevolution_la_SOURCES = $(CORE_SOURCES)
libsyncevolution_la_LIBADD = @FUNAMBOL_LIBS@ @TRANSPORT_LDFLAGS@ @EPACKAGE_LIBS@ @GLIB_LIBS@ @CPPUNIT_UNITTESTS_LDFLAGS@ @LIBS@
libsyncevolution_la_CXXFLAGS = $(SYNCEVOLUTION_CXXFLAGS) @CPPUNIT_UNITTESTS_CXXFLAGS@

syncevolution_la_SOURCES = syncevolutionmodule.cpp \
	src/core/scitbx/boost_python/container_conversions.h
syncevolution_la_CXXFLAGS = @PYTHON_CXXFLAGS@
syncevolution_la_LIBADD = libsyncevolution.la
syncevolution_la_DEPENDENCIES = libsyncevolution.la
syncevolution_la_LDFLAGS = -module @PYTHON_LDFLAGS@

# include boost in distribution
dist-hook:
	cp -r $(srcdir)/boost $(distdir)
	find $(distdir) -name .libs -o -name "*~" -o -name ".*" -o -name "*.o" -o -name "*.lo" -o -name CVS -o -name autom4te.cache | xargs rm -rf
