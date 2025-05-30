//
// Net.h
//
// Library: Net
// Package: NetCore
// Module:  Net
//
// Basic definitions for the Poco Net library.
// This file must be the first file included by every other Net
// header file.
//
// Copyright (c) 2005-2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//


#ifndef Net_Net_INCLUDED
#define Net_Net_INCLUDED


#include "Poco/Foundation.h"


//
// The following block is the standard way of creating macros which make exporting
// from a DLL simpler. All files within this DLL are compiled with the Net_EXPORTS
// symbol defined on the command line. this symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see
// Net_API functions as being imported from a DLL, wheras this DLL sees symbols
// defined with this macro as being exported.
//
#if defined(_WIN32) && defined(POCO_DLL)
	#if defined(Net_EXPORTS)
		#define Net_API __declspec(dllexport)
	#else
		#define Net_API __declspec(dllimport)
	#endif
#endif


#if !defined(Net_API)
	#if !defined(POCO_NO_GCC_API_ATTRIBUTE) && defined (__GNUC__) && (__GNUC__ >= 4)
		#define Net_API __attribute__ ((visibility ("default")))
	#else
		#define Net_API
	#endif
#endif


//
// Automatically link Net library.
//
#if defined(_MSC_VER)
	#if !defined(POCO_NO_AUTOMATIC_LIBS) && !defined(Net_EXPORTS)
		#pragma comment(lib, "PocoNet" POCO_LIB_SUFFIX)
	#endif
#endif


// Default to enabled IPv6 support if not explicitly disabled
#if !defined(POCO_NET_NO_IPv6) && !defined (POCO_HAVE_IPv6)
	#define POCO_HAVE_IPv6
#elif defined(POCO_NET_NO_IPv6) && defined (POCO_HAVE_IPv6)
	#undef POCO_HAVE_IPv6
#endif // POCO_NET_NO_IPv6, POCO_HAVE_IPv6


// Default to enabled local socket support if not explicitly disabled
#if !defined(POCO_NET_NO_UNIX_SOCKET) && !defined (POCO_HAVE_UNIX_SOCKET)
	#define POCO_HAVE_UNIX_SOCKET
#elif defined(POCO_NET_NO_UNIX_SOCKET) && defined (POCO_HAVE_UNIX_SOCKET)
	#undef POCO_HAVE_UNIX_SOCKET
#endif // POCO_NET_NO_UNIX_SOCKET, POCO_HAVE_UNIX_SOCKET


namespace Poco {
namespace Net {


void Net_API initializeNetwork();
	/// Initialize the network subsystem.
	/// (Windows only, no-op elsewhere)


void Net_API uninitializeNetwork();
	/// Uninitialize the network subsystem.
	/// (Windows only, no-op elsewhere)


std::string htmlize(const std::string& str);
	/// Returns a copy of html with reserved HTML
	/// characters (<, >, ", &) propery escaped.


} } // namespace Poco::Net


//
// Automate network initialization (only relevant on Windows).
//

#if defined(POCO_OS_FAMILY_WINDOWS) && !defined(POCO_NO_AUTOMATIC_LIB_INIT)

extern "C" const struct Net_API NetworkInitializer pocoNetworkInitializer;

#if defined(POCO_COMPILER_MINGW)
	#define POCO_NET_FORCE_SYMBOL(x) static void *__ ## x ## _fp = (void*)&x;
#elif defined(Net_EXPORTS)
	#if defined(_WIN64)
		#define POCO_NET_FORCE_SYMBOL(s) __pragma(comment (linker, "/export:"#s))
	#elif defined(_WIN32)
		#define POCO_NET_FORCE_SYMBOL(s) __pragma(comment (linker, "/export:_"#s))
	#endif
#else  // !Net_EXPORTS
	#if !defined(POCO_NETWORK_INITIALIZER_INCLUDE_PATH)
		#if defined(_WIN64)
			#define POCO_NETWORK_INITIALIZER_INCLUDE_PATH "/include:"
		#elif defined(_WIN32)
			#define POCO_NETWORK_INITIALIZER_INCLUDE_PATH "/include:_"
		#endif
	#endif
	#define POCO_NET_FORCE_SYMBOL(s) __pragma(comment (linker, POCO_NETWORK_INITIALIZER_INCLUDE_PATH#s))
#endif // Net_EXPORTS

POCO_NET_FORCE_SYMBOL(pocoNetworkInitializer)

#endif // POCO_OS_FAMILY_WINDOWS


//
// Define POCO_NET_HAS_INTERFACE for platforms that have network interface detection implemented.
//
#if defined(POCO_OS_FAMILY_WINDOWS) || (POCO_OS == POCO_OS_LINUX) || (POCO_OS == POCO_OS_ANDROID) || defined(POCO_OS_FAMILY_BSD) || (POCO_OS == POCO_OS_SOLARIS) || (POCO_OS == POCO_OS_QNX)
	#define POCO_NET_HAS_INTERFACE
#endif


#if (POCO_OS == POCO_OS_LINUX) || (POCO_OS == POCO_OS_WINDOWS_NT) || (POCO_OS == POCO_OS_ANDROID)
	#define POCO_HAVE_FD_EPOLL 1
#endif


#if defined(POCO_OS_FAMILY_BSD)
	#ifndef POCO_HAVE_FD_POLL
		#define POCO_HAVE_FD_POLL 1
	#endif
#endif


#endif // Net_Net_INCLUDED
