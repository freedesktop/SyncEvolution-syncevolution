dnl -*- mode: Autoconf; -*-
dnl Invoke autogen.sh to produce a configure script.

# The STABLE_VERSION is used when building
# Debian packages. For prereleases (beta, alpha),
# set it to something like "0.9.2+" and the AC_INIT
# VERSION to 1.0beta1 to produce 0.9.2+1.0beta1.
#
# Starting with the 1.1 release cycle, the rpm-style
# .99 pseudo-version number is used to mark a pre-release.
AC_INIT([syncevolution], [m4_esyscmd([build/gen-git-version.sh 1.4.99.2])])
# STABLE_VERSION=1.0.1+
AC_SUBST(STABLE_VERSION)

# Generate some files.
SE_GENERATE_AM_FILES
SE_GENERATE_LINGUAS

# Default value for --enable/disable-release-mode.
# Determined by gen-autotools.sh based on versioning.
# Official, stable releases enable it, pre-releases
# disable it.
#
SE_CHECK_FOR_STABLE_RELEASE

# Minimum version of libsynthesis as defined in its
# configure script and thus .pc files:
define([SYNTHESIS_MIN_VERSION], [3.4.0.47.2])

# Line above is patched by gen-autotools.sh. Handle
# both "yes" and "no".
m4_define([STABLE_RELEASE_HELP], m4_if(STABLE_RELEASE,[yes],[--disable-release-mode],[--enable-release-mode]))

AC_ARG_ENABLE(release-mode,
              AS_HELP_STRING([STABLE_RELEASE_HELP],
                             [Controls whether resulting binary is for end-users or
                              testers/developers. For example, stable releases automatically
                              migrate on-disk files without asking, whereas other releases
                              ask before making downgrades impossible (or difficult).
                              Default in this source code is
                              "stable release: STABLE_RELEASE"]),
              enable_release_mode="$enableval", enable_release_mode="STABLE_RELEASE")
if test "$enable_release_mode" = "yes"; then
   AC_DEFINE(SYNCEVOLUTION_STABLE_RELEASE, 1, [binary is meant for end-users])
fi

AM_INIT_AUTOMAKE([1.11.1 tar-ustar silent-rules subdir-objects -Wno-portability])

AM_PROG_CC_C_O

AM_MAINTAINER_MODE([enable]) # needed for nightly builds where reconfiguration fails under certain chroots
AC_CONFIG_MACRO_DIR([m4])
define([SYNTHESISSRC_REPO], [])
dnl Specify git revisions/branches without prefix, i.e., without 'origin'.
dnl We'll sort that out below.
define([SYNTHESISSRC_REVISION], [syncevolution-0.9])
AC_CONFIG_HEADERS(config.h)
AC_LIBTOOL_DLOPEN
AC_PROG_LIBTOOL

dnl check for programs.
AC_PROG_CXX
AC_PROG_MAKE_SET

# Boost headers: boost/foreach.hpp is needed (1.33/Debian Etch
# doesn't have it, 1.34/Ubuntu 8.10 Hardy does). 1.35 is available
# as Debian Etch backport.
AX_BOOST_BASE(1.34)

# TODO: Fix code to pass with -pedantic -Wextra.
# -Wno-unknown-pragmas needed because icalstrdup.h
# currently uses the "#pragma }" trick. Should remove that.
# Fix code to work without deprecated methods: G GDK GDK_PIXBUF CAIRO PANGO GTK
DK_ARG_ENABLE_WARNINGS([SYNCEVO_WFLAGS],
                       [-Wall -Wno-unknown-pragmas -Wno-deprecated-declarations],
                       [-Wall -Wno-unknown-pragmas -Wno-deprecated-declarations],
                       [])

dnl default device type (see AC_DEFINE below)
DEVICE_TYPE=workstation

AC_ARG_WITH(synthesis-src,
            AS_HELP_STRING([--with-synthesis-src=<base directory|svn URL|git URL>],
                           [Specifies location of the Synthesis root directory.
                           Use this when the Synthesis library is to
                           be compiled as part of the SyncEvolution compilation. In release
                           versions of SyncEvolution, a copy of the Synthesis code is bundled
                           under 'src/synthesis' and compiled unless something else is
                           specified. --with-synthesis-src can be given a path to sources
                           checked out already, a Subversion repository URL or a git repository
                           URL. When given a repository URL, then the configure script
                           will checkout the sources into 'src/synthesis-workdir' or
                           update that working copy if the directory already exists.
                           Default: bundled source in src/synthesis (in released SyncEvolution sources),
                           SYNTHESISSRC_REPO otherwise.]),
            [SYNTHESISSRC="$withval"
             test "$SYNTHESISSRC" != "yes" || AC_MSG_ERROR([--with-synthesis-src requires a parameter (base directory, svn URL or git URL)])],
            [SYNTHESISSRC="$SYNTHESISSRC_DEF"; REVISION="SYNTHESISSRC_REVISION"])

AC_ARG_WITH(syncml-engines,
            AS_HELP_STRING([--with-syncml-engines=client|server|both],
                           [Determines which kind of support for SyncML is compiled and linked into SyncEvolution. Default is both. Currently has no effect.]),
            [SYNCML_ENGINES="$withval"], SYNCML_ENGINES=both)

case $SYNCML_ENGINES in both|client) AC_DEFINE(ENABLE_SYNCML_CLIENT, 1, [SyncML client support available]);; esac
case $SYNCML_ENGINES in both|server) AC_DEFINE(ENABLE_SYNCML_SERVER, 1, [SyncML server support available]);; esac
case $SYNCML_ENGINES in both|server|client) true;; *) AC_MSG_ERROR([Invalid value for --with-syncml-engines: $SYNCML_ENGINES]);; esac

AC_ARG_WITH(synthesis-username,
            AS_HELP_STRING([--with-synthesis-username=<svn username>],
                           [username to use when checking out --with-synthesis-src sources from Subversion, default 'guest']),
            [USERNAME="$withval"], [USERNAME="guest"])

AC_ARG_WITH(synthesis-revision,
            AS_HELP_STRING([--with-synthesis-revision=<git tag/branch/hash or Subversion revision>],
                           [Identifies which source revision to use from --with-synthesis-src repository, empty string stands for latest. Default for default --synthesis-src: SYNTHESISSRC_REVISION]),
            [REVISION="$withval"])

AC_ARG_ENABLE(shared,
              AS_HELP_STRING([--enable-shared],
                             [build backends as dynamically loadable modules]),
              enable_shared="$enableval", enable_shared="no")

AC_ARG_ENABLE(static,
              AS_HELP_STRING([--enable-static],
                             [build backends also as static libraries]),
              enable_static="$enableval", enable_static="no")

AC_ARG_ENABLE(unit-tests,
              AS_HELP_STRING([--enable-unit-tests],
                             [enables tests embedded in the source code of the library (changes content of executable)]),
              enable_unit_tests="$enableval", enable_unit_tests="no")
AC_ARG_ENABLE(integration-tests,
              AS_HELP_STRING([--enable-integration-tests],
                             [enables tests outside of the library (can be used together with normal builds of the library)]),
              enable_integration_tests="$enableval", enable_integration_tests="no")

AC_ARG_ENABLE(static-cxx,
              AS_HELP_STRING([--enable-static-cxx],
                             [build executables which contain libstdc++ instead of requiring suitable libstdc++.so to run]),
              enable_static_cxx="$enableval", enable_static_cxx="no")

AC_ARG_ENABLE(evolution-compatibility,
              AS_HELP_STRING([--enable-evolution-compatibility],
                             [build executables which only call Evolution via dlopen/dlsym: this avoids all hard dependencies on EDS shared objects, but might lead to crashes when their ABI changes; use --enable-evolution-compatibility=ical to enable a weaker mode where linking is done normally and only libical.so.0/1 enum differences are worked around (allows patching resulting executables to use either of these two)]),
              enable_evolution_compatibility="$enableval", enable_evolution_compatibility="no")

AC_ARG_ENABLE(internal-icaltz,
              AS_HELP_STRING([--disable-internal-icaltz],
                             [libical 1.0 updated its system zone data parsing code so that it produces VTIMEZONEs which are unsuitable for syncing. SyncEvolution ships with a copy of the older code and uses it by default.]),
              [enable_icaltz_util="$enableval"
               test "$enable_icaltz_util" = "yes" || test "$enable_icaltz_util" = "no" || AC_ERROR([invalid value of --disable-internal-icaltz: $enableval])],
              enable_icaltz_util="yes")
if test "$enable_icaltz_util" = "yes"; then
   AC_DEFINE(ENABLE_ICALTZ_UTIL, [1], [use internal icaltz-util.c])
   AC_DEFINE(DISABLE_ICALTZUTIL_GET_ZONE_DIRECTORY, [1], [use libical's icaltzutil_get_zone_directory()])
fi
AM_CONDITIONAL([ENABLE_ICALTZ_UTIL], [test "$enable_icaltz_util" = "yes"])

AC_ARG_ENABLE(developer-mode,
              AS_HELP_STRING([--enable-developer-mode],
                             [The dynamic loadble backend libraries is loaded from current build directory instead of the standard library path]),
              enable_developer_mode="$enableval", enable_developer_mode="no")

# Maemo hacks:
# - wrap e_book_from_string() to fix invalid parameter
# - don't use UTF-8 encoding in Perl script
AC_ARG_ENABLE(maemo,
              AS_HELP_STRING([--enable-maemo],
                             [enables some hacks which work around problems with the Maemo 2.0 until at least 3.0 EDS-Dbus]),
              [AC_DEFINE(ENABLE_MAEMO, 1, [enable Maemo hacks])
               DEVICE_TYPE=Maemo
               MODIFY_SYNCCOMPARE='-e "s/use encoding/#use encoding/;" -e "s/:utf8//;"'])
AC_SUBST(MODIFY_SYNCCOMPARE)

AC_CHECK_HEADERS(signal.h dlfcn.h)

# For icaltz-util.c
AC_CHECK_HEADERS(byteswap.h endian.h sys/endian.h unistd.h stdint.h)

# cppunit-config is used even when both unit tests and integration tests are disabled.
AC_PATH_PROG([CPPUNIT_CONFIG], [cppunit-config], [no])

# cppunit needed?
if test "x$enable_unit_tests" = 'xyes' || test "x$enable_integration_tests" = 'xyes'; then
  test "x$CPPUNIT_CONFIG" != 'xno' || AC_MSG_ERROR("cppunit-config not found.")
fi
if test "x$CPPUNIT_CONFIG" != 'xno'; then
  # Export the flags if available, even if not enabled. This allows
  # "make src/client-test" in cases where "make all" would not build
  # client-test.
  CPPUNIT_CXXFLAGS=`$CPPUNIT_CONFIG --cflags`
  CPPUNIT_LDFLAGS=`$CPPUNIT_CONFIG --libs`
fi
AC_SUBST(CPPUNIT_CXXFLAGS)
AC_SUBST(CPPUNIT_LDFLAGS)

if test "x$enable_unit_tests" = 'xyes'; then
        AC_DEFINE(ENABLE_UNIT_TESTS, 1, [enable unit tests inside the library's source code])
fi
if test "x$enable_integration_tests" = 'xyes'; then
        AC_DEFINE(ENABLE_INTEGRATION_TESTS, 1, [enable integration tests inside the final library])
fi
AM_CONDITIONAL([ENABLE_UNIT_TESTS], [test "$enable_unit_tests" = "yes"])
AM_CONDITIONAL([ENABLE_TESTING], [test "$enable_unit_tests" = "yes" || test "$enable_integration_tests" = "yes" ])

if test $enable_static_cxx = "yes"; then
        LIBS="$LIBS -L."
        CORE_LDADD_DEP=libstdc++.a
fi
AC_SUBST(CORE_LDADD_DEP)

# Check for transport layer.
# Both curl and libsoup can be enabled and disabled explicitly.
# The default is to use libsoup if available, otherwise curl.

AC_MSG_CHECKING([for libcurl])
if LIBCURL_LIBS=`sh -c 'curl-config --libs' 2>&AS_MESSAGE_LOG_FD` && \
    LIBCURL_CFLAGS=`sh -c 'curl-config --cflags' 2>&AS_MESSAGE_LOG_FD`; then
    AC_MSG_RESULT([yes])
    have_libcurl="yes"
else
    AC_MSG_RESULT([no])
    have_libcurl="no"
fi

PKG_CHECK_MODULES(LIBSOUP, libsoup-gnome-2.4,
                  [have_libsoup="yes"
                   AC_DEFINE(HAVE_LIBSOUP_SOUP_GNOME_FEATURES_H, 1, [enable GNOME specific libsoup])],
                  [PKG_CHECK_MODULES(LIBSOUP, libsoup-2.4,
                                     have_libsoup="yes",
                                     have_libsoup="no")])

PKG_CHECK_MODULES(LIBOPENOBEX, openobex, have_obex="yes", have_obex="no")
have_bluetooth="no"
if test $have_obex = "yes"; then
    PKG_CHECK_MODULES(BLUEZ, bluez, have_bluez="yes", have_bluez="no")
    if test $have_bluez = "yes"; then
        have_bluetooth="yes"
    fi
fi
AC_SUBST(LIBOPENOBEX_CFLAGS)
AC_SUBST(LIBOPENOBEX_LIBS)
AC_SUBST(BLUEZ_CFLAGS)
AC_SUBST(BLUEZ_LIBS)

TRANSPORT=
TRANSPORT_LIBS=
TRANSPORT_CFLAGS=

AC_ARG_WITH(ca-certificates,
            AS_HELP_STRING([--with-ca-certificates=<colon separated list of files>],
                           [Specifies location of one or more CA certificate files.
                           This sets the default value for the SSLServerCertificates option.
                           Default: empty when using libcurl (because it has its own default),
                           a list of paths known to work for Debian and Red Hat otherwise.]),
            [CA_CERTIFICATES="$withval"])


# choose default http transport (mirrors code in EvolutionSyncClient::createTransportAgent())
if test "$have_libsoup" = "yes"; then
   default_http_transport="libsoup"
elif test "$have_libcurl" = "yes"; then
   default_http_transport="libcurl"
fi

AC_ARG_ENABLE(libcurl,
              AS_HELP_STRING([--enable-libcurl],
                             [enable libcurl as transport layer]),
              [ if test "$enableval" = "yes"; then
                   test "$have_libcurl" = "yes" || AC_MSG_ERROR([libcurl not found])
                   TRANSPORT="$TRANSPORT libcurl"
                   TRANSPORT_LIBS="$TRANSPORT_LIBS $LIBCURL_LIBS"
                   TRANSPORT_CFLAGS="$TRANSPORT_CFLAGS $LIBCURL_CFLAGS"
                   AC_DEFINE(ENABLE_LIBCURL, 1, [enable libcurl transport])
                else
                   libcurl_disabled="yes"
                fi ],
              [ if test "$have_libcurl" = "yes" && test "$default_http_transport" = "libcurl" ; then
                   TRANSPORT="$TRANSPORT libcurl"
                   TRANSPORT_LIBS="$TRANSPORT_LIBS $LIBCURL_LIBS"
                   TRANSPORT_CFLAGS="$TRANSPORT_CFLAGS $LIBCURL_CFLAGS"
                   AC_DEFINE(ENABLE_LIBCURL, 1, [enable libcurl transport])
                fi ])

AC_ARG_ENABLE(libsoup,
              AS_HELP_STRING([--enable-libsoup],
                             [enable libsoup as transport layer]),
              [ if test "$enableval" = "yes"; then
                   test "$have_libsoup" = "yes" || AC_MSG_ERROR([libsoup not found])
                   TRANSPORT="$TRANSPORT libsoup"
                   TRANSPORT_LIBS="$TRANSPORT_LIBS $LIBSOUP_LIBS"
                   TRANSPORT_CFLAGS="$TRANSPORT_CFLAGS $LIBSOUP_CFLAGS"
                   AC_DEFINE(ENABLE_LIBSOUP, 1, [enable libsoup transport])
                else
                   libsoup_disabled="yes"
                fi ],
              [ if test "$have_libsoup" = "yes" && test "$default_http_transport" = "libsoup"; then
                   TRANSPORT="$TRANSPORT libsoup"
                   TRANSPORT_LIBS="$TRANSPORT_LIBS $LIBSOUP_LIBS"
                   TRANSPORT_CFLAGS="$TRANSPORT_CFLAGS $LIBSOUP_CFLAGS"
                   AC_DEFINE(ENABLE_LIBSOUP, 1, [enable libsoup transport])
                fi ])

# SoupTransportAgent depends on glib
case "$TRANSPORT" in *libsoup*) need_glib=yes;; esac

AC_ARG_ENABLE(dlt,
              AS_HELP_STRING([--enable-dlt],
                             [enable logging via GENIVI Diagnostic Log and Trace (DLT)]),
              [enable_dlt=$enableval
               test $enable_dlt = "yes" || test $enable_dlt = "no" || AC_ERROR([invalid value of --enable-dlt: $enableval])],
              [enable_dlt="no"])
if test "$enable_dlt" = "yes"; then
    PKG_CHECK_MODULES(DLT, automotive-dlt,
                      [USE_DLT=1],
                      [AC_ERROR([dlt not found, required for --enable-dlt])])
    AC_DEFINE(USE_DLT, 1, "optionally use GENIVI Diagnostic Log and Trace for logging")
    AC_ARG_WITH([dlt-syncevolution],
                AS_HELP_STRING([--with-dlt-syncevolution=SYNS,SYNH,SYNL],
                               [controls the application IDs used by syncevo-dbus-server, syncevo-dbus-helper and syncevo-local-sync]),
                [with_dlt_ids="$withval"],
                [with_dlt_ids="SYNS,SYNH,SYNL"])
    syns=`echo $with_dlt_ids | cut -d , -f 1`
    synh=`echo $with_dlt_ids | cut -d , -f 2`
    synl=`echo $with_dlt_ids | cut -d , -f 3`
    AC_DEFINE_UNQUOTED(DLT_SYNCEVO_DBUS_SERVER_ID, "$syns", "DLT app ID for syncevo-dbus-server")
    AC_DEFINE_UNQUOTED(DLT_SYNCEVO_DBUS_HELPER_ID, "$synh", "DLT app ID for syncevo-dbus-helper")
    AC_DEFINE_UNQUOTED(DLT_SYNCEVO_LOCAL_HELPER_ID, "$synl", "DLT app ID for syncevo-local-helper")
fi

bluetooth_disabled=no
AC_ARG_ENABLE(bluetooth,
              AS_HELP_STRING([--enable-bluetooth],
                             [enable bluetooth transport support]),
              [ enable_bluetooth="$enableval"
                if test "$enableval" = "no"; then
                    bluetooth_disabled=yes
                fi
              ],
              [ enable_bluetooth="$have_bluetooth" ])

if test "$enable_bluetooth" = "yes"; then
   # currently we need Bluetooth and OBEX support
   test "$have_bluetooth" = "yes" || AC_MSG_ERROR([openobex or bluez not found])

   AC_LANG(C)
   CFLAGS_old="$CFLAGS"
   CFLAGS="$CPPFLAGS $BLUEZ_CFLAGS"
   # test in this order:
   # - recent libbluetooth (no _safe variant, base function has bufsize)
   # - intermediate with _safe
   # - else assume old-style (no bufsize, no _safe)
   #
   # The source code checks the signature both by via pointer assignment and calling
   # it (better safe than sorry). One these should fail if the signature is not right.
   AC_COMPILE_IFELSE([AC_LANG_SOURCE([
                      #include <bluetooth/sdp.h>
                      #include <bluetooth/sdp_lib.h>
                      sdp_record_t *(*extract_pdu)(const uint8_t *pdata, int bufsize, int *scanned) =
                           sdp_extract_pdu;
                      void foo(void) {
                          uint8_t *pdata = NULL;
                          int scanned;
                          sdp_extract_pdu(pdata, 100, &scanned);
                      }
                     ])],
                     AC_DEFINE(HAVE_BLUEZ_BUFSIZE, 1, [base libbluetooth functions accept bufsize parameter]),
                     AC_COMPILE_IFELSE([AC_LANG_SOURCE([
                                        #include <bluetooth/sdp.h>
                                        #include <bluetooth/sdp_lib.h>
                                        sdp_record_t *(*extract_pdu)(const uint8_t *pdata, int bufsize, int *scanned) =
                                               sdp_extract_pdu_safe;
                                        void foo(void) {
                                            uint8_t *pdata = NULL;
                                            int scanned;
                                            sdp_extract_pdu_safe(pdata, 100, &scanned);
                                        }
                                       ])],
                                       AC_DEFINE(HAVE_BLUEZ_SAFE, 1, [libbluetooth has _safe variants])))
   CFLAGS="$CFLAGS_old"

   if test "$have_obex" = "yes"; then
      AC_DEFINE(ENABLE_OBEX, 1, [define if openobex library is available])
   fi
   if test "$have_bluez" = "yes"; then
      AC_DEFINE(ENABLE_BLUETOOTH, 1, [define if bluez library is available])
   fi
fi
AM_CONDITIONAL([ENABLE_OBEX], [test "$have_obex" = "yes" && test "$enable_bluetooth" = "yes"])
AM_CONDITIONAL([ENABLE_BLUETOOTH], [test "$have_bluetooth" = "yes" && test "$enable_bluetooth" = "yes"])

if test ! "$TRANSPORT" &&
   test "$libsoup_disabled" != "yes" &&
   test "$libcurl_disabled" != "yes" &&
   test "$bluetooth_disabled" != "yes" &&
   test "$have_bluetooth" != "yes" ; then
   AC_MSG_ERROR([no transport library found, configure with --disable-libcurl --disable-libsoup --disable-bluetooth to continue anyway (only useful if users of libsyncevolution provide transport implementation)])
fi

# for libsoup we must specify the SSL certificate file outself
if test "$libsoup_disabled" != "yes" && test -z "$CA_CERTIFICATES"; then
   # Debian and Red Hat paths
   CA_CERTIFICATES="/etc/ssl/certs/ca-certificates.crt:/etc/pki/tls/certs/ca-bundle.crt:/usr/share/ssl/certs/ca-bundle.crt"
fi
AC_DEFINE_UNQUOTED(SYNCEVOLUTION_SSL_SERVER_CERTIFICATES, "$CA_CERTIFICATES", [default value for SSLServerCertificates option])

AC_SUBST(TRANSPORT_LIBS)
AC_SUBST(TRANSPORT_CFLAGS)

AC_ARG_ENABLE(ssl-certificate-check,
              AS_HELP_STRING([--disable-ssl-certificate-check],
                             [Disable SSL certificate checking in all server
                              *templates*.  Users can still choose to enable or
                              disable it in their configuration. This is necessary on
                              platforms where the transport library has problems
                              verifying the server's certificate (libsoup + Google,
                              http://bugzilla.moblin.org/show_bug.cgi?id=4551)]),
             enable_ssl_certificate_check="$enableval",
             enable_ssl_certificate_check="yes")
if test "$enable_ssl_certificate_check" = "yes"; then
   AC_DEFINE(ENABLE_SSL_CERTIFICATE_CHECK, 1, [enable SSL certificate check in server templates])
fi

# for dbus interface file mangling
AC_PATH_PROG(XSLT, xsltproc)


# Changes in GTK3 mean that supporting both GTK3 and GTK2 in the same codebase
# is difficult. We want to support GTK2 for the time being so the code is forked.
AC_ARG_ENABLE(gtk,
              AS_HELP_STRING([--enable-gtk=major version],
                             [Selects the gtk+ version ("2" or "3") to use for the UI.
                              If this option is used, --enable-gui should be used as well.
                              "3" is the default option if available, otherwise "2".]),
              [ if test "$enableval" = "3" ; then
                    gtk_version=gtk+-3.0
                elif test "$enableval" = "2" ; then
                    gtk_version=gtk+-2.0
                else
                    AC_MSG_ERROR([Unknown gtk version: '$enableval'])
                fi
              ],
              [ PKG_CHECK_EXISTS([gtk+-3.0],
                                 [gtk_version=gtk+-3.0],
                                 [gtk_version=gtk+-2.0]) ])


AM_CONDITIONAL([COND_GTK2], [test "$gtk_version" = "gtk+-2.0"])

AC_ARG_ENABLE(gui,
              AS_HELP_STRING([--enable-gui[=gui type]],
                             [enables building the GTK+ UI that uses the SyncEvolution DBus API.
                              Options: gtk, moblin, all (builds sync-ui-gtk and sync-ui-moblin)
                              "gtk" is the default for --enable-gui without type. No GUI is
                              built when --enable-gui is not used.]),
              [ if test "$enableval" = "gtk" ; then
                    enable_gui=gtk
                elif test "$enableval" = "yes" ; then
                    enable_gui=gtk
                elif test "$enableval" = "moblin" ; then
                    enable_gui=moblin
                elif test "$enableval" = "no" ; then
                    enable_gui=no
                elif test "$enableval" = "all" ; then
                    enable_gui=all
                else
                    AC_MSG_ERROR([Unknown gui type: '$enableval'])
                fi
              ],
              [ enable_gui=no ])

AM_CONDITIONAL([COND_GUI], [test "$enable_gui" != "no"])

AC_ARG_ENABLE(core,
              AS_HELP_STRING([--enable-core],
                             [enables building the core SyncEvolution (library, backends)]),
              enable_core="$enableval",
              enable_core="yes")
AM_CONDITIONAL([COND_CORE], [test "$enable_core" = "yes"])

AC_ARG_ENABLE(dbus-service,
              AS_HELP_STRING([--enable-dbus-service=args],
                             [Enables building the dbus service executable and all related features
                             (the DBus wrapper library, command line usage of server, etc).
                             The optional arguments are syncevo-dbus-server command line arguments
                             that are used when auto-starting via D-Bus or .desktop file. By default,
                             the daemon logs to syslog. This can be changed via command line arguments.
                             ]),
              enable_dbus_service="$enableval",
              [if test $enable_gui = "no"; then
                  enable_dbus_service="no"
               else
                  enable_dbus_service="yes"
               fi])
AM_CONDITIONAL([COND_DBUS], [test "$enable_dbus_service" != "no"])
if test "$enable_dbus_service" != "no" && test "$enable_dbus_service" != "yes"; then
   SYNCEVO_DBUS_SERVER_ARGS="$enable_dbus_service"
fi
AC_SUBST(SYNCEVO_DBUS_SERVER_ARGS)

AC_ARG_WITH([gio-gdbus],
            AS_HELP_STRING([--with-gio-gdbus],
                           [enables use of GIO's GDBus instead of the in-tree, Bluez gdbus.]),
            with_gio_gdbus="$withval",
            PKG_CHECK_EXISTS([gio-2.0 >= 2.30],
                             [with_gio_gdbus="yes"],
                             [with_gio_gdbus="no"]))
AM_CONDITIONAL([COND_GIO_GDBUS], [test "x$with_gio_gdbus" = "xyes"])

# We only need to check for dbus-1 if gio-gdbus is not used.
#
# Local sync depends on D-Bus communication between parent
# and child process (works without a D-Bus daemon), and local
# sync is not an optional feature. Could be made one if
# someone is interested enough.
#
# Therefore, at the moment, either libdbus or gio-gdbus are needed
# unconditionally. glib is needed in all cases now.
need_glib=yes
AS_IF([test "x$with_gio_gdbus" = "xyes"],
      [PKG_CHECK_MODULES([DBUS], [gio-2.0 >= 2.26])
       AC_DEFINE([WITH_GIO_GDBUS],[],[Set if using GIO GDBus])],
      [PKG_CHECK_MODULES(DBUS, dbus-1, dummy=yes,
                         AC_MSG_ERROR(libdbus-1 is required))
       AC_CHECK_LIB(dbus-1, dbus_watch_get_unix_fd, dummy=yes,
                    AC_DEFINE(NEED_DBUS_WATCH_GET_UNIX_FD, 1,
                    [Define to 1 if you need the dbus_watch_get_unix_fd() function.]))])

if test "$enable_dbus_service" != "no"; then
    if test -z "$XSLT"; then
       AC_MSG_ERROR([xsltproc not found, is required for D-Bus service])
    fi
    # Recent libnotify releases work with gtk+-2.0 and gtk+-3.0.
    AC_ARG_ENABLE([notify],
        AS_HELP_STRING([--enable-notify],
                       [send notifications for automatic sync events, using libnotify]),
        enable_notify="$enableval",
        PKG_CHECK_EXISTS([libnotify $gtk_version],
                         [enable_notify="yes"],
                         [enable_notify="no"]))
    AS_IF([test "x$enable_notify" = "xyes"], [PKG_CHECK_MODULES([LIBNOTIFY], [libnotify $gtk_version])]
                                             [AC_DEFINE(HAS_NOTIFY, 1,
                                                        [define if libnotify could be used in dbus service])])

    AC_ARG_ENABLE(notify-compatibility,
                  AS_HELP_STRING([--enable-notify-compatibility],
                                 [increase compatibility with binary libnotify installations by loading libnotify.so.1..4 dynamically instead of linking against it]),
                                 [enable_notify_compat="$enableval"],
                                 [enable_notify_compat="no"]
                 )
    if test "$enable_notify_compat" = "yes"; then
        AC_DEFINE(NOTIFY_COMPATIBILITY, 1, [dynamically open libnotify])
        LIBNOTIFY_LIBS="`echo $LIBNOTIFY_LIBS | sed -e 's/\(-lnotify\|[^ ]*libnotify.la\)/-ldl/'`"
    fi

    # Here we're using QtGui too because mlite fails to depend on it,
    # despite using QAction.
    PKG_CHECK_MODULES(MLITE, [mlite QtGui], HAVE_MLITE=yes, HAVE_MLITE=no)
    AC_ARG_ENABLE(mlite,
                  AS_HELP_STRING([--enable-mlite],
                                 [send notifications for automatic sync events, using mlite (off by default)]),
                  [ enable_mlite="$enableval"
                    test "$enableval" = "no" || test $HAVE_MLITE = "yes" || AC_MSG_ERROR([required mlite package not found]) ],
                  [ enable_mlite="no" ])
    if test $enable_mlite = "yes"; then
        AC_DEFINE(HAS_MLITE, 1, [define if mlite could be used in dbus service])
    else
        # don't use mlite, even if found
        MLITE_CFLAGS=
        MLITE_LIBS=
    fi
    AC_DEFINE(DBUS_SERVICE, 1, [define if dbus service is enabled])

    AC_ARG_ENABLE(dbus-service-pim,
                  AS_HELP_STRING([--enable-dbus-service-pim[=<locale>]],
                                 [Enable implementation of org._01.pim D-Bus APIs (depends on libfolks),
                                  using src/dbus/server/pim/locale-factory-<locale>.cpp to implement sorting
                                  and searching. The default is <locale>=boost, which uses boost::locale.]),
                  [ enable_dbus_pim="$enableval" ],
                  [ enable_dbus_pim="no" ])
    case "$enable_dbus_pim" in
         no) ;;
         *)
                if test "$enable_dbus_pim" = "yes"; then
                    enable_dbus_pim=boost
                fi
                if ! test -r "$srcdir/src/dbus/server/pim/locale-factory-$enable_dbus_pim.cpp"; then
                    AC_MSG_ERROR([invalid value '$enable_dbus_pim' for --enable-dbus-service-pim, $srcdir/src/dbus/server/pim/locale-factory-$enable_dbus_pim.cpp does not exist or is not readable])
                fi
                PKG_CHECK_MODULES(FOLKS, [folks folks-eds])
                AC_DEFINE(ENABLE_DBUS_PIM, 1, [org._01.pim D-Bus API enabled])
                DBUS_PIM_PLUGIN=$enable_dbus_pim
                AC_SUBST(DBUS_PIM_PLUGIN)

                case "$enable_dbus_pim" in
                    boost)
                        AX_BOOST_LOCALE
                        # AX_BOOST_LOCALE incorrectly puts -L/... into LDFLAGS.
                        # That's broken because it then overrides the search path
                        # for *all* libraries in a link, not just for boost. Fix
                        # this by putting the LDFLAGS before the lib and leaving
                        # DBUS_PIM_PLUGIN_LDFLAGS empty (for now - might have to
                        # be revised if there ever are any boost flags which need
                        # to go to the start of the link line).
                        DBUS_PIM_PLUGIN_LIBS='$(BOOST_LDFLAGS) $(BOOST_LOCALE_LIB)'
                        DBUS_PIM_PLUGIN_LDFLAGS=
                        # We need to call ICU directly for the Han->Latin transformation.
                        PKG_CHECK_MODULES(ICU, [icu-uc icu-i18n])
                        ;;
                esac
                AC_SUBST(DBUS_PIM_PLUGIN_CFLAGS)
                AC_SUBST(DBUS_PIM_PLUGIN_LIBS)
                AC_SUBST(DBUS_PIM_PLUGIN_LDFLAGS)

                # http://code.google.com/p/libphonenumber/
                AC_LANG(C++)
                SAVED_CPPFLAGS=$CPPFLAGS
                if test ! "$PHONENUMBERS_CFLAGS"; then
                      PHONENUMBERS_CFLAGS=-DI18N_PHONENUMBERS_USE_BOOST
                fi
                CPPFLAGS="$CPPFLAGS ${PHONENUMBERS_CFLAGS}"
                AC_CHECK_HEADERS([phonenumbers/phonenumberutil.h])
                SAVED_LIBS=$LIBS
                if test ! "$PHONENUMBERS_LIBS"; then
                      PHONENUMBERS_LIBS=-lphonenumber
                fi
                LIBS="$LIBS $PHONENUMBERS_LIBS"
                AC_LINK_IFELSE(
                      [AC_LANG_PROGRAM([#include <phonenumbers/phonenumberutil.h>],
                                       [i18n::phonenumbers::PhoneNumberUtil::GetInstance()])],
                      [true],
                      [AC_ERROR([libphonebook not found, set PHONENUMBERS_CFLAGS and PHONENUMBERS_LIBS.])])
                AC_SUBST(PHONENUMBERS_CFLAGS)
                AC_SUBST(PHONENUMBERS_LIBS)
                LIBS=$SAVED_LIBS
                CPPFLAGS=$SAVED_CPPFLAGS
                ;;
    esac
fi
AM_CONDITIONAL([NOTIFY_COMPATIBILITY], [test "$enable_notify_compat" = "yes"])
AM_CONDITIONAL([COND_DBUS_PIM], [test "$enable_dbus_pim" != "no"])

AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)
AC_SUBST(DBUS_GLIB_CFLAGS)
AC_SUBST(DBUS_GLIB_LIBS)
AC_SUBST(LIBNOTIFY_CFLAGS)
AC_SUBST(LIBNOTIFY_LIBS)
AC_SUBST(LIBEXECDIR)

DBUS_SERVICES_DIR="${datadir}/dbus-1/services"
AC_SUBST(DBUS_SERVICES_DIR)
AC_DEFINE_UNQUOTED(DBUS_SERVICES_DIR, "$DBUS_SERVICES_DIR", [Location of D-Bus services directory])

if test "$enable_gui" != "no" || test "$enable_dbus_service" != "no"; then
    IT_PROG_INTLTOOL([0.37.1])
    GETTEXT_PACKAGE=syncevolution
    AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, "$GETTEXT_PACKAGE", [The gettext package name])
    AM_GLIB_GNU_GETTEXT
    SYNCEVOLUTION_LOCALEDIR=[${datadir}/locale]
fi

# decide which sync-ui(s) we are building:
# sync-ui (in either GTK or Moblin mode) or both (in separate binaries)

if test $gtk_version = "gtk+-3.0"; then
    gtk_dir=src/gtk3-ui
else
    gtk_dir=src/gtk-ui
fi

case $enable_gui in
     all) GUI_PROGRAMS=${gtk_dir}'/sync-ui-gtk${EXEEXT} '${gtk_dir}'/sync-ui-moblin${EXEEXT}'; GUI_DESKTOP_FILES="${gtk_dir}/sync-gtk.desktop ${gtk_dir}/sync-moblin.desktop";;
     gtk|moblin) GUI_PROGRAMS=${gtk_dir}'/sync-ui${EXEEXT}'; GUI_DESKTOP_FILES="${gtk_dir}/sync.desktop";;
     no) GUI_PROGRAMS=; GUI_DESKTOP_FILES=;;
     *) AC_MSG_ERROR([Unknown enable_gui type: '$enable_gui'])
esac

if test $enable_gui != "no"; then
    PKG_CHECK_MODULES(DBUS_GLIB, dbus-glib-1 glib-2.0)
    AC_PATH_PROG(DBUS_BINDING_TOOL, dbus-binding-tool)
    AC_PATH_PROG(GLIB_GENMARSHAL, glib-genmarshal)

    gui_modules="$gtk_version glib-2.0 dbus-glib-1 >= 0.60 gio-2.0 gio-unix-2.0"
    if test $enable_gui = "moblin"; then
        AC_DEFINE(USE_MOBLIN_UX, 1, [Use Moblin UI widgets])
    fi

    # gtk3 provides a switch widget, otherwise we need mx-gtk
    if test $gtk_version != "gtk+-3.0" &&
       test $enable_gui = "moblin" -o $enable_gui = "all"; then
        gui_modules="$gui_modules mx-gtk-1.0"

        PKG_CHECK_MODULES(MX_GTK_0_99_1, mx-gtk-1.0 >= 0.99.1,
                          have_mx_gtk_0_99_1="yes",
                          have_mx_gtk_0_99_1="no")
        if test $have_mx_gtk_0_99_1 = "yes"; then
            AC_DEFINE(MX_GTK_0_99_1, 1, [we have Mx-Gtk 0.99.1 or better])
        fi
    fi

    PKG_CHECK_MODULES(UNIQUE, unique-1.0,
                      have_unique="yes",
                      have_unique="no")
    if test $have_unique = "yes"; then
        gui_modules="$gui_modules unique-1.0"
        AC_DEFINE(ENABLE_UNIQUE, 1, [enable single-app-instance functionality])
    fi

    PKG_CHECK_MODULES(GTK_2_18, gtk+-2.0 >= 2.18,
                      have_gtk_2_18="yes",
                      have_gtk_2_18="no")
    if test $have_gtk_2_18 = "yes"; then
        AC_DEFINE(GTK_2_18, 1, [we have GTK+ 2.18 or better])
    fi

    PKG_CHECK_MODULES(GUI, $gui_modules)

elif test "$enable_dbus_service" != "no"; then
    # syncevo-dbus-server needs localization
    :
else
    INTLTOOL_UPDATE="true"
    USE_NLS="no"
fi
AC_SUBST(SYNCEVOLUTION_LOCALEDIR)
AC_SUBST(GETTEXT_PACKAGE)
AC_SUBST(GUI_CFLAGS)
AC_SUBST(GUI_LIBS)
AC_SUBST(GUI_PROGRAMS)
AC_SUBST(GUI_DESKTOP_FILES)

# C++ regular expression support is required often enough to make it
# mandatory.
PKG_CHECK_MODULES(PCRECPP, libpcrecpp,,
AC_CHECK_LIB(pcrecpp,main,
AC_SUBST(PCRECPP_LIBS,-lpcrecpp),
AC_MSG_ERROR([pcrecpp not found])
))

# need rst2man for man pages
AC_ARG_WITH(rst2man,
            AS_HELP_STRING([--with-rst2man=<path to reStructuredText to man converter>],
                           [Specifies an explicit path to the utility if not found in PATH.
                            An explicit --without-rst2man or not having it installed turn off
                            building of man pages.]),
            [RST2MAN=$withval
             if test "$RST2MAN" = "yes"; then
                 AC_PATH_PROG(RST2MAN, rst2man, "no")
             fi
             test "$RST2MAN" = "no" || test -x "$RST2MAN" || AC_MSG_ERROR([--with-rst2man=$RST2MAN: tool not found])],
            [AC_PATH_PROG(RST2MAN, rst2man, "no")])
AM_CONDITIONAL([COND_MAN_PAGES], [test "$RST2MAN" != "no"])

# need rst2html for HTML version of README
AC_ARG_WITH(rst2html,
            AS_HELP_STRING([--with-rst2html=<path to reStructuredText to HTML converter>],
                           [Specifies an explicit path to the utility if not found in PATH.
                            An explicit --without-rst2html or not having it installed turn off
                            building of README in HTML format.]),
            [RST2HTML=$withval
             if test "$RST2HTML" = "yes"; then
                 AC_PATH_PROG(RST2HTML, rst2html, "no")
             fi
             test "$RST2HTML" = "no" || test -x "$RST2HTML" || AC_MSG_ERROR([--with-rst2html=$RST2HTML: tool not found])],
            [AC_PATH_PROG(RST2HTML, rst2html, "no")])
AM_CONDITIONAL([COND_HTML_README], [test "$RST2HTML" != "no"])

# absolute patch to source of Synthesis client library
SYNTHESIS_SRC=no-synthesis-source
AC_SUBST(SYNTHESIS_SRC)
if test "$SYNTHESISSRC" && test "$SYNTHESISSRC" != "none"; then
    # default: checkout a copy of the sources, remove it during maintainer-clean and distclean
    CLEAN_CLIENT_SRC=synthesis-workdir
    SYNTHESIS_SRC=$PWD/src/synthesis-workdir

    AC_MSG_NOTICE( [updating the content of $SYNTHESIS_SRC from $SYNTHESISSRC] )
    case "$SYNTHESISSRC" in
        *.git) protocol=git;;
        *://*) protocol="`echo $SYNTHESISSRC | sed -e 's;://.*;;'`";;
        *) protocol="file";;
    esac

    mkdir -p src
    case $protocol in
        file)
            # use existing copy of the sources
            CLEAN_CLIENT_SRC=
            case "$SYNTHESISSRC" in
                 /*) SYNTHESIS_SRC="$SYNTHESISSRC";;
                 *) SYNTHESIS_SRC="$PWD/$SYNTHESISSRC";;
            esac
            ;;
        *svn*|*http*)
            SYNTHESISSRCREV="$SYNTHESISSRC"
            if test "$REVISION"; then
                revarg="-r $REVISION "
                if `echo $SYNTHESISSRC | grep '@[0123456789]*'` >/dev/null; then
                    :
                else
                    SYNTHESISSRCREV="$SYNTHESISSRC@$REVISION"
                fi
            fi
            if test -d $SYNTHESIS_SRC ; then
                ( set -x; cd $SYNTHESIS_SRC && svn --username=$USERNAME switch $revarg "$SYNTHESISSRC" ) || AC_MSG_ERROR([updating from $SYNTHESISSRC failed])
            else
                (set -x; svn --username=$USERNAME checkout $revarg "$SYNTHESISSRCREV" $SYNTHESIS_SRC ) || AC_MSG_ERROR([checking out $SYNTHESISSRC failed])
            fi
            ;;
        *)
            if test -d $SYNTHESIS_SRC ; then
                ( set -x; cd $SYNTHESIS_SRC && git fetch "$SYNTHESISSRC" ) || AC_MSG_ERROR([updating from $SYNTHESISSRC failed])
            else
                ( set -x; git clone "$SYNTHESISSRC" $SYNTHESIS_SRC ) || AC_MSG_ERROR([cloning $SYNTHESISSRC failed])
            fi
            if test "$REVISION"; then
                # git 1.6 finds tags and branches without explicit prefix, 1.4.4.4 doesn't
                ( set -x; cd $SYNTHESIS_SRC &&
                  (git checkout "$REVISION" ||
                   git checkout "tags/$REVISION" ||
                   git checkout "origin/$REVISION") ) || AC_MSG_ERROR([checking out $SYNTHESISSRC failed])
            fi
            ;;
    esac
elif test "$SYNTHESISSRC" != "none" && test -d $srcdir/src/synthesis; then
    # use existing copy of the sources; beware of
    # out-of-tree compilation
    case $srcdir in
         /*) SYNTHESIS_SRC="$srcdir/src/synthesis";;
         *) SYNTHESIS_SRC="$PWD/$srcdir/src/synthesis";;
    esac
elif test "$enable_shared" = "no"; then
    # link against engine
    PKG_CHECK_MODULES([SYNTHESIS], [synthesis >= 3.4])
    SYNTHESIS_ENGINE="$SYNTHESIS_LIBS -lsynthesis -lsmltk"
else
    # link against SDK alone, except in client-test
    #PKG_CHECK_MODULES(SYNTHESIS, "synthesis-sdk")
    #SYNTHESIS_ENGINE="`echo $SYNTHESIS_LIBS | sed -e 's/-lsynthesisstubs/-lsynthesis/'`"

    # can't use the SDK alone because of sysync::SySyncDebugPuts()
    PKG_CHECK_MODULES([SYNTHESIS], [synthesis >= SYNTHESIS_MIN_VERSION])
    SYNTHESIS_ENGINE="$SYNTHESIS_LIBS"
fi

if test $SYNTHESIS_SRC != "no-synthesis-source"; then
    ( cd $SYNTHESIS_SRC && ( test -f configure || sh autogen.sh ) ) || AC_MSG_ERROR([--with-synthesis-src=$SYNTHESIS_SRC: no Synthesis configure script found in that directory])

    SYNTHESIS_CONFIGURE="$SYNTHESIS_SRC/configure"
    chmod u+x $SYNTHESIS_SRC/configure $SYNTHESIS_SRC/config.sub $SYNTHESIS_SRC/config.guess

    # use local copy of the sources, with dependencies
    # to trigger building the synthesis library
    SYNTHESIS_SUBDIR=$PWD/src/build-synthesis
    SYNTHESIS_CFLAGS="-I$SYNTHESIS_SUBDIR/src"
    SYNTHESIS_LIBS="$SYNTHESIS_SUBDIR/src/libsynthesissdk.la $SYNTHESIS_SUBDIR/src/libsmltk.la"

    if test "x$enable_core" = "xno" && test "x$enable_gui" != "xno"; then
       # SYNTHESIS_SUBDIR is ignored, at least build headers for GUI
       SYNTHESIS_SUBDIR_INCLUDES=src/synthesis-includes
    fi

    if test "$enable_shared" = "no"; then
        # link against the engines that were enabled
        case $SYNCML_ENGINES in both|client|server) SYNTHESIS_LIBS="$SYNTHESIS_LIBS $SYNTHESIS_SUBDIR/src/libsynthesis.la $SYNTHESIS_SUBDIR/src/libsmltk.la";; esac
        AC_DEFINE(ENABLE_SYNCML_LINKED, 1, [SyncML engines are linked directly])
    else
        # It would be nice if we could avoid linking against libsynthesis.la here.
        # This doesn't work at the moment because sysync::SySyncDebugPuts()
        # is called directly by the libsynthesissdk instead of going through
        # the normal C function pointer lookup.
        SYNTHESIS_LIBS="$SYNTHESIS_LIBS $SYNTHESIS_SUBDIR/src/libsynthesis.la $SYNTHESIS_SUBDIR/src/libsmltk.la"
    fi
    SYNTHESIS_DEP=$SYNTHESIS_LIBS

    # for linking client-test
    SYNTHESIS_ENGINE="$SYNTHESIS_SUBDIR/src/libsynthesis.la"

    AC_MSG_NOTICE( [configuring the Synthesis library] )
    if (set -x; mkdir -p $SYNTHESIS_SUBDIR && cd $SYNTHESIS_SUBDIR && eval "\$SHELL \"\$SYNTHESIS_CONFIGURE\" $ac_configure_args \"--srcdir=\$SYNTHESIS_SRC\" " ); then true; else
        AC_MSG_ERROR( [configuring Synthesis library failed] )
    fi

    # do the version check with the .pc files prepared by the configure step above
    export PKG_CONFIG_PATH=$SYNTHESIS_SUBDIR:$PKG_CONFIG_PATH
    PKG_CHECK_MODULES([WITH_SYNTHESIS_SRC], [synthesis >= SYNTHESIS_MIN_VERSION],
                      [],
                      [AC_MSG_ERROR([need at least libsynthesis >= SYNTHESIS_MIN_VERSION; the latest libsynthesis for SyncEvolution is the one from http://cgit.freedesktop.org/SyncEvolution/])])

fi

AC_SUBST(SYNTHESIS_CFLAGS)
AC_SUBST(SYNTHESIS_LIBS)
AC_SUBST(SYNTHESIS)
AC_SUBST(SYNTHESIS_SUBDIR)
AC_SUBST(SYNTHESIS_SUBDIR_INCLUDES)
AC_SUBST(SYNTHESIS_DEP)
AC_SUBST(SYNTHESIS_ENGINE)
AC_SUBST(SYNTHESIS_LIB)
AC_SUBST(SYNTHESISSRC)

BACKEND_CPPFLAGS="$SYNTHESIS_CFLAGS $EPACKAGE_CFLAGS $EBOOK_CFLAGS $ECAL_CFLAGS $GLIB_CFLAGS $BOOST_CPPFLAGS"
AC_SUBST(BACKEND_CPPFLAGS)

# GNOME Bluetooth Panel plugin
PKG_CHECK_MODULES(GNOMEBLUETOOTH, [gnome-bluetooth-1.0 >= 2.27.6],
                  [have_gbt="yes"
                   GNOMEBLUETOOTH_DIR=`$PKG_CONFIG --variable=libdir gnome-bluetooth-1.0 2>/dev/null`/gnome-bluetooth],
                   have_gbt="no")
AC_SUBST(GNOMEBLUETOOTH_CFLAGS)
AC_SUBST(GNOMEBLUETOOTH_DIR)

AC_ARG_ENABLE(gnome-bluetooth-panel-plugin,
        AS_HELP_STRING([--enable-gnome-bluetooth-panel-plugin],
                       [GNOME Bluetooth panel plugin adding a "sync" button for paired devices (off by default)]),
                       [enable_gnome_bluetooth_panel="$enableval"],
        [enable_gnome_bluetooth_panel="no"]
        )
if test "$enable_gnome_bluetooth_panel" = "yes"; then
   test "$have_gbt" = "yes" || AC_MSG_ERROR([--enable-gnome-bluetooth-panel requires
           pkg-config information for gnome-bluetooth-1.0 >= 2.27.6 which was not found])
fi
AM_CONDITIONAL([ENABLE_GNOME_BLUETOOTH_PANEL], [test "$have_gbt" = "yes" && test "$enable_gnome_bluetooth_panel" = "yes"])

AC_ARG_ENABLE(doc,
              AS_HELP_STRING([--enable-doc],
                             [generate backend and DBus API documentation]),
              enable_doc="$enableval", enable_doc="no")

AM_CONDITIONAL([COND_DOC], [test "$enable_doc" != "no"])

dnl add backends stuff.
SE_ADD_BACKENDS

dnl -*- mode: Autoconf; -*-
dnl Invoke autogen.sh to produce a configure script.

AC_ARG_ENABLE(qt-dbus,
              AS_HELP_STRING([--enable-qt-dbus],
                             [build Qt bindings for D-Bus]),
              enable_qt_dbus="$enableval", enable_qt_dbus="no")

if test "$enable_qt_dbus" = "yes"; then
        AC_DEFINE(ENABLE_QT_DBUS, 1, [Qt D-Bus bindings available])
        need_qt_modules="$need_qt_modules +dbus"
        AC_PATH_PROG([QDBUSXML_TO_CPP], [qdbusxml2cpp], [no])
        test "x$QDBUSXML_TO_CPP" != 'xno' || AC_MSG_ERROR([--enable-qt-dbus requires qdbusxml2cpp, which was not found])
fi

AM_CONDITIONAL([ENABLE_QT_DBUS], [test "$enable_qt_dbus" = "yes"])

AC_SUBST(QT_DBUS_LIBS)
AC_CONFIG_FILES([src/dbus/qt/syncevolution-qt-dbus.pc])

dnl -*- mode: Autoconf; -*-
dnl Invoke autogen.sh to produce a configure script.
dnl configure-pre.in and src/backends/*/configure-sub.in and configure-post.in come before this part

AC_SUBST(BACKEND_CPPFLAGS)

enable_any="no"
backend_is_enabled () {
    eval echo \${enable_${1}}
}
for backend in $BACKENDS; do
    if test `backend_is_enabled $backend` = "yes"; then
       enable_any="yes"
       SYNCEVOLUTION_MODULES="$SYNCEVOLUTION_MODULES src/backends/sync${backend}.la"
    fi
done

if test "$enable_any" = "no"; then
        AC_MSG_ERROR([no backend enabled - refusing to continue: $anymissing])
fi

dnl glib initialization is done only if requested by some configure-sub.in,
dnl for not needed otherwise even if found
if test "$need_glib" = "yes"; then
        # HAVE_GLIB (aka GLIBFOUND) are a catch-all for these
        # three GNOME libs. Assume we have all three unless one of
        # the checks fails.
        GLIBFOUND=yes

        dnl check for glib - calling g_type_init() is expected on Maemo
        PKG_CHECK_MODULES(GLIB, "glib-2.0", , GLIBFOUND=no)
        # This check here is broken on Ubuntu 8.04: it calls glib-config,
        # which isn't found, but the error is not detected by configure.
        #if test "x${GLIBFOUND}" = "xno"; then
        #	PKG_CHECK_MODULES(GLIB, "glib", GLIBFOUND=yes, GLIBFOUND=no)
        #fi

        PKG_CHECK_MODULES(GTHREAD, "gthread-2.0", , GLIBFOUND=no)
        PKG_CHECK_MODULES(GOBJECT, "gobject-2.0", , GLIBFOUND=no)
        PKG_CHECK_MODULES(GIO, "gio-2.0", , GLIBFOUND=no)
        PKG_CHECK_MODULES(GIOUNIX, "gio-unix-2.0", , GLIBFOUND=no)

        if  test "x${GLIBFOUND}" = "xyes"; then
            AC_DEFINE(HAVE_GLIB, 1, [glib found])
        else
            AC_MSG_ERROR([not all GNOME libraries found])
        fi
        BACKEND_CPPFLAGS="$BACKEND_CPPFLAGS $GLIB_CFLAGS $GTHREAD_CFLAGS $GOBJECT_CFLAGS"
fi

dnl check for libical if needed by backend or icaltz-util.
if test "$need_ical" = "yes" || test "$enable_icaltz_util" = "yes"; then
   PKG_CHECK_MODULES(LIBICAL, libical,
                     [true],
                     [PKG_CHECK_MODULES(LIBICAL, libecal-1.2)])
fi

dnl Use libical in eds_abi_wrapper if and only if required by some backend.
if test "$need_ical" = "yes"; then
   AC_DEFINE(ENABLE_ICAL, 1, [libical in use])
fi
AM_CONDITIONAL([ENABLE_ICAL], [test "$need_ical" = "yes"])

# Check for Qt if some backend needs it.
if test "$need_qt_modules"; then
   AT_WITH_QT([-gui $need_qt_modules],
              [$qt_config],
              [$qt_misc])
fi

# determine from where we can get a SHA-256 implementation
have_sha="no"
if test "$GLIBFOUND" = "yes"; then
   # only use glib if we need it anyway, also has to be at lease 2.16
   PKG_CHECK_MODULES(GLIB216, [glib-2.0 >= 2.16],
                     [AC_DEFINE(USE_SHA256, 1, [choose implementation of SHA-256])
                      have_sha="glib"],
                     [true])
fi
if test "$have_sha" = "no"; then
   # Fallback is Mozilla NSS. In contrast to libgcrypt it has a .pc
   # file and a simple API.
   PKG_CHECK_MODULES(NSS, "nss",
                     [AC_DEFINE(USE_SHA256, 2, [choose implementation of SHA-256])
                      have_sha="nss"],
                     [true])
fi

dnl figure out whether we link all code statically or as modules
AM_CONDITIONAL([ENABLE_MODULES], [test "$enable_shared" = "yes"])
if test "$enable_shared" = "yes"; then
        AC_DEFINE(ENABLE_MODULES, 1, [enable dynamically opening sync source backends])
fi
AC_SUBST(SYNCEVOLUTION_LDADD)

dnl CXXFLAGS gets applied to SyncEvolution and the client library.
dnl For e.g. "-Wall -Werror" this might not be such a good idea;
dnl SYNCEVOLUTION_CXXFLAGS can be used instead. It applies only
dnl to the sources in the SyncEvolution repository.
AC_SUBST(SYNCEVOLUTION_CXXFLAGS)

dnl a quick-and-dirty way to detect compilation for the iPhone
if test "$host" = "arm-apple-darwin"; then
   AC_DEFINE(IPHONE, 1, [compiling for iPhone])
   DEVICE_TYPE=iPhone
fi

dnl --enable-evolution-compatibility
if test "$enable_evolution_compatibility" = "yes"; then
   AC_DEFINE(EVOLUTION_COMPATIBILITY, 1, [avoid hard dependency on Evolution shared objects])
   # don't link against libs wrapped by eds_abi_wrapper (no longer limited to EDS alone...)
   ECAL_LIBS=
   EBOOK_LIBS=
   EPACKAGE_LIBS=
   BLUEZ_LIBS=
fi
if test "$enable_evolution_compatibility" = "ical" || test "$enable_evolution_compatibility" = "yes"; then
   AC_DEFINE(EVOLUTION_ICAL_COMPATIBILITY, 1, [work with both libical.so.0 and libical.so.1])
fi
AM_CONDITIONAL([ENABLE_EVOLUTION_COMPATIBILITY], [test "$enable_evolution_compatibility" = "yes"])
PKG_CHECK_MODULES(LIBICAL_AVAILABLE,
                  libical >= 0.43,
                  AC_DEFINE(HAVE_LIBICAL_R, 1, [have recent enough libical with _r variants]),
                  pass)

dnl --enable-developer-mode
if test "$enable_developer_mode" = "yes"; then
BACKENDS_SEARCH_DIRECTORY="`pwd`/src/backends/"
else
BACKENDS_SEARCH_DIRECTORY='$(libdir)/syncevolution/backends/'
fi

BACKENDS_DIRECTORY='$(libdir)/syncevolution/backends'
AC_SUBST(BACKENDS_DIRECTORY)
AC_SUBST(BACKENDS_SEARCH_DIRECTORY)

# for ActiveSync backend
syncevo_backenddir='$(libdir)'/syncevolution/backends
AC_SUBST(syncevo_backenddir)

dnl This string is sent as part of the SyncML DevInf (device
dnl information) structure to the server. All SyncEvolution platforms
dnl use "SyncEvolution" as HTTP user agent and "Mod" (model), so the
dnl device type is the only way how different kinds of clients can be
dnl distinguished.
AC_DEFINE_UNQUOTED(DEVICE_TYPE, "$DEVICE_TYPE", "SyncML DevInf DevType")

AC_CHECK_HEADERS(stdarg.h valgrind/valgrind.h execinfo.h)

AC_DEFINE(SYNTHESIS, 1, "using Synthesis engine")

# fallback for lack of --with-docdir support in older automake
if test ! "$docdir"; then
   docdir = ${datadir}/doc/syncevolution
   AC_SUBST(docdir)
fi

AC_ARG_ENABLE(dbus-timeout-hack,
              AS_HELP_STRING([--enable-dbus-timeout-hack],
                             [Enables code which overrides the default timeout in dbus_connection_send_with_reply() so that the method call never times out. Needed for libecal/ebook >= 2.30, so enabled by default if either of these is enabled.]),
              [enable_dbus_timeout_hack=$enableval],
              [if test $enable_ebook = "yes" || test $enable_ecal = "yes"; then
                  enable_dbus_timeout_hack="yes"
               fi])
if test "$enable_dbus_timeout_hack" = "yes"; then
   AC_DEFINE(ENABLE_DBUS_TIMEOUT_HACK, 1, [overrides the default D-Bus timeout so that synchronous calls never time out])
fi

# Avoid hard-coding paths in backends. These names are chosen so
# that a backend can alternatively use its own top-level configure
# with PKG_CHECK_MODULES(SYNCEVOLUTION, "syncevolution") to set them.
# need absolute path, use pwd instead of relative $srcdir
#
# When adding something here, remember to also update syncevolution.pc.in.
# -lrt is for clock_gettime() in the Timespec.h inline functions.
SYNCEVOLUTION_CFLAGS=-I`cd $srcdir && pwd`/src
# Linker flags including libsyncevolution.la and some libs.
SYNCEVOLUTION_LIBS="src/syncevo/libsyncevolution.la -lrt"
AC_SUBST(SYNCEVOLUTION_CFLAGS)
AC_SUBST(SYNCEVOLUTION_LIBS)

# invoking syncevolution binary is allowed to fail when cross-compiling
AM_CONDITIONAL([COND_CROSS_COMPILING], [test "$cross_compiling" = "yes"])

# Set by any of the backends providing a keyring, determines the
# default for the "keyring" option.
if test "$have_keyring" = "yes"; then
   AC_DEFINE(HAVE_KEYRING, 1, [some kind of secure credential store is available])
fi

AC_CONFIG_FILES([
  Makefile
  src/syncevo/syncevolution.pc
  src/synthesis-includes/Makefile
  po/Makefile.in
  src/dbus/glib/syncevo-dbus.pc
])
AC_OUTPUT

echo
echo CONFIGURATION SUMMARY
echo "Core SyncEvolution: $enable_core"
for backend in $BACKENDS; do
    eval echo $backend: \${enable_${backend}}
done
echo "DBus service: $enable_dbus_service"
echo "org._01.pim support in DBus service: $enable_dbus_pim"
echo "Notifications: $enable_notify"
echo "GIO GDBus: $with_gio_gdbus"
echo "GNOME keyring: $enable_gnome_keyring"
if test "$enable_gui" = "no"; then
    echo "UI (DBus client): no"
else
    echo "UI (DBus client): $enable_gui (using $gtk_version)"
fi
echo "Bluetooth transport: $have_bluetooth"
echo "GNOME Bluetooth panel plugin: $enable_gnome_bluetooth_panel"
echo "SHA-256: $have_sha"
echo "API documentation: $enable_doc"
echo "D-Bus Timeout Hack: $enable_dbus_timeout_hack"
echo
