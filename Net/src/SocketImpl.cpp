//
// SocketImpl.cpp
//
// Library: Net
// Package: Sockets
// Module:  SocketImpl
//
// Copyright (c) 2005-2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//


#include "Poco/Net/SocketImpl.h"
#include "Poco/Net/NetException.h"
#include "Poco/Net/StreamSocketImpl.h"
#include "Poco/NumberFormatter.h"
#include "Poco/Timestamp.h"
#include "Poco/FileStream.h"
#include "Poco/Error.h"
#include <string.h> // FD_SET needs memset on some platforms, so we can't use <cstring>


#if defined(POCO_HAVE_FD_EPOLL)
	#ifdef POCO_OS_FAMILY_WINDOWS
		#include "wepoll.h"
		#include "mswsock.h"
	#else
		#include <sys/epoll.h>
		#include <sys/eventfd.h>
	#endif
#elif defined(POCO_HAVE_FD_POLL)
	#ifndef _WIN32
		#include <poll.h>
	#endif
#endif


#if defined(sun) || defined(__sun) || defined(__sun__)
#include <unistd.h>
#include <stropts.h>
#endif


#ifdef POCO_OS_FAMILY_WINDOWS
#include <windows.h>
#else
#include <csignal>
#endif


#if POCO_OS == POCO_OS_MAC_OS_X || POCO_OS == POCO_OS_FREE_BSD
#include <sys/uio.h>
#include <sys/types.h>
#endif


#if POCO_OS == POCO_OS_LINUX && defined(POCO_HAVE_SENDFILE) && !defined(POCO_EMSCRIPTEN)
#include <sys/sendfile.h>
#endif


#if defined(_MSC_VER)
#pragma warning(disable:4996) // deprecation warnings
#endif


using Poco::IOException;
using Poco::TimeoutException;
using Poco::InvalidArgumentException;
using Poco::NumberFormatter;
using Poco::Timespan;


#ifdef WEPOLL_H_
namespace {

	int close(HANDLE h)
	{
		return epoll_close(h);
	}

}
#endif // WEPOLL_H_


namespace Poco {
namespace Net {


bool checkIsBrokenTimeout()
{
#if defined(POCO_BROKEN_TIMEOUTS)
	return true;
#elif defined(POCO_OS_FAMILY_WINDOWS)
	// on Windows 7 and lower, socket timeouts have a minimum of 500ms, use poll for timeouts on this case
	// https://social.msdn.microsoft.com/Forums/en-US/76620f6d-22b1-4872-aaf0-833204f3f867/minimum-timeout-value-for-sorcvtimeo
	OSVERSIONINFO vi;
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (GetVersionEx(&vi) == 0) return true;
	return vi.dwMajorVersion < 6 || (vi.dwMajorVersion == 6 && vi.dwMinorVersion < 2);
#endif
	return false;
}


SocketImpl::SocketImpl():
	_sockfd(POCO_INVALID_SOCKET),
	_blocking(true),
	_isBrokenTimeout(checkIsBrokenTimeout())
{
}


SocketImpl::SocketImpl(poco_socket_t sockfd):
	_sockfd(sockfd),
	_blocking(true),
	_isBrokenTimeout(checkIsBrokenTimeout())
{
}


SocketImpl::~SocketImpl()
{
	close();
}


SocketImpl* SocketImpl::acceptConnection(SocketAddress& clientAddr)
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	sockaddr_storage buffer;
	struct sockaddr* pSA = reinterpret_cast<struct sockaddr*>(&buffer);
	poco_socklen_t saLen = sizeof(buffer);
	poco_socket_t sd;
	do
	{
		sd = ::accept(_sockfd, pSA, &saLen);
	}
	while (sd == POCO_INVALID_SOCKET && lastError() == POCO_EINTR);
	if (sd != POCO_INVALID_SOCKET)
	{
		clientAddr = SocketAddress(pSA, saLen);
		return new StreamSocketImpl(sd);
	}
	error(); // will throw
	return nullptr;
}


void SocketImpl::connect(const SocketAddress& address)
{
	if (_sockfd == POCO_INVALID_SOCKET)
	{
		init(address.af());
	}
	int rc;
	do
	{
#if defined(POCO_VXWORKS)
		rc = ::connect(_sockfd, (sockaddr*) address.addr(), address.length());
#else
		rc = ::connect(_sockfd, address.addr(), address.length());
#endif
	}
	while (rc != 0 && lastError() == POCO_EINTR);
	if (rc != 0)
	{
		int err = lastError();
		error(err, address.toString());
	}
}


void SocketImpl::connect(const SocketAddress& address, const Poco::Timespan& timeout)
{
	if (_sockfd == POCO_INVALID_SOCKET)
	{
		init(address.af());
	}
	setBlocking(false);
	try
	{
#if defined(POCO_VXWORKS)
		int rc = ::connect(_sockfd, (sockaddr*) address.addr(), address.length());
#else
		int rc = ::connect(_sockfd, address.addr(), address.length());
#endif
		if (rc != 0)
		{
			int err = lastError();
			if (err != POCO_EINPROGRESS && err != POCO_EWOULDBLOCK)
				error(err, address.toString());
			if (!poll(timeout, SELECT_READ | SELECT_WRITE | SELECT_ERROR))
				throw Poco::TimeoutException("connect timed out", address.toString());
			err = socketError();
			if (err != 0) error(err);
		}
	}
	catch (Poco::Exception&)
	{
		setBlocking(true);
		throw;
	}
	setBlocking(true);
}


void SocketImpl::connectNB(const SocketAddress& address)
{
	if (_sockfd == POCO_INVALID_SOCKET)
	{
		init(address.af());
	}
	setBlocking(false);
#if defined(POCO_VXWORKS)
	int rc = ::connect(_sockfd, (sockaddr*) address.addr(), address.length());
#else
	int rc = ::connect(_sockfd, address.addr(), address.length());
#endif
	if (rc != 0)
	{
		int err = lastError();
		if (err != POCO_EINPROGRESS && err != POCO_EWOULDBLOCK)
			error(err, address.toString());
	}
}


void SocketImpl::bind(const SocketAddress& address, bool reuseAddress)
{
	bind(address, reuseAddress, reuseAddress);
}


void SocketImpl::bind(const SocketAddress& address, bool reuseAddress, bool reusePort)
{
	if (_sockfd == POCO_INVALID_SOCKET)
	{
		init(address.af());
	}

#ifdef POCO_HAS_UNIX_SOCKET
	if (address.family() != SocketAddress::Family::UNIX_LOCAL)
#endif
	{
		setReuseAddress(reuseAddress);
		setReusePort(reusePort);
	}

#if defined(POCO_VXWORKS)
	int rc = ::bind(_sockfd, (sockaddr*) address.addr(), address.length());
#else
	int rc = ::bind(_sockfd, address.addr(), address.length());
#endif
	if (rc != 0) error(address.toString());
}


void SocketImpl::bind6(const SocketAddress& address, bool reuseAddress, bool ipV6Only)
{
	bind6(address, reuseAddress, reuseAddress, ipV6Only);
}


void SocketImpl::bind6(const SocketAddress& address, bool reuseAddress, bool reusePort, bool ipV6Only)
{
#if defined(POCO_HAVE_IPv6)
	if (address.family() != SocketAddress::IPv6)
		throw Poco::InvalidArgumentException("SocketAddress must be an IPv6 address");

	if (_sockfd == POCO_INVALID_SOCKET)
	{
		init(address.af());
	}
#ifdef IPV6_V6ONLY
	setOption(IPPROTO_IPV6, IPV6_V6ONLY, ipV6Only ? 1 : 0);
#else
	if (ipV6Only) throw Poco::NotImplementedException("IPV6_V6ONLY not defined.");
#endif
	setReuseAddress(reuseAddress);
	setReusePort(reusePort);
	int rc = ::bind(_sockfd, address.addr(), address.length());
	if (rc != 0) error(address.toString());
#else
	throw Poco::NotImplementedException("No IPv6 support available");
#endif
}


void SocketImpl::useFileDescriptor(poco_socket_t fd)
{
	poco_assert (_sockfd == POCO_INVALID_SOCKET);

	_sockfd = fd;
}


void SocketImpl::listen(int backlog)
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	int rc = ::listen(_sockfd, backlog);
	if (rc != 0) error();
}


void SocketImpl::close()
{
	if (_sockfd != POCO_INVALID_SOCKET)
	{
		poco_closesocket(_sockfd);
		_sockfd = POCO_INVALID_SOCKET;
	}
}


void SocketImpl::shutdownReceive()
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	int rc = ::shutdown(_sockfd, 0);
	if (rc != 0) error();
}


int SocketImpl::shutdownSend()
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	int rc = ::shutdown(_sockfd, 1);
	if (rc != 0) error();
	return 0;
}


int SocketImpl::shutdown()
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	int rc = ::shutdown(_sockfd, 2);
	if (rc != 0) error();
	return 0;
}


void SocketImpl::checkBrokenTimeout(SelectMode mode)
{
	if (_isBrokenTimeout)
	{
		Poco::Timespan timeout = (mode == SELECT_READ) ? _recvTimeout : _sndTimeout;
		if (timeout.totalMicroseconds() != 0)
		{
			if (!poll(timeout, mode))
				throw TimeoutException();
		}
	}
}


int SocketImpl::sendBytes(const void* buffer, int length, int flags)
{
	if (_blocking)
	{
		checkBrokenTimeout(SELECT_WRITE);
	}
	int rc;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();
		rc = ::send(_sockfd, reinterpret_cast<const char*>(buffer), length, flags);
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


int SocketImpl::sendBytes(const SocketBufVec& buffers, int flags)
{
	if (_blocking)
	{
		checkBrokenTimeout(SELECT_WRITE);
	}
	int rc = 0;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();
#if defined(POCO_OS_FAMILY_WINDOWS)
		DWORD sent = 0;
		rc = WSASend(_sockfd, const_cast<LPWSABUF>(&buffers[0]),
					static_cast<DWORD>(buffers.size()), &sent,
					static_cast<DWORD>(flags), 0, 0);
		if (rc == SOCKET_ERROR) error();
		rc = sent;
#elif defined(POCO_OS_FAMILY_UNIX)
		rc = writev(_sockfd, &buffers[0], static_cast<int>(buffers.size()));
#endif
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


int SocketImpl::receiveBytes(void* buffer, int length, int flags)
{
	if (_blocking)
	{
		checkBrokenTimeout(SELECT_READ);
	}
	int rc;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();
		rc = ::recv(_sockfd, reinterpret_cast<char*>(buffer), length, flags);
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


int SocketImpl::receiveBytes(SocketBufVec& buffers, int flags)
{
	if (_blocking)
	{
		checkBrokenTimeout(SELECT_READ);
	}
	int rc = 0;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();
#if defined(POCO_OS_FAMILY_WINDOWS)
		DWORD recvd = 0;
		DWORD dwFlags = static_cast<DWORD>(flags);
		rc = WSARecv(_sockfd, &buffers[0], static_cast<DWORD>(buffers.size()),
					&recvd, &dwFlags, 0, 0);
		if (rc == SOCKET_ERROR) error();
		rc = recvd;
#elif defined(POCO_OS_FAMILY_UNIX)
		rc = readv(_sockfd, &buffers[0], static_cast<int>(buffers.size()));
#endif
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


int SocketImpl::receiveBytes(Poco::Buffer<char>& buffer, int flags, const Poco::Timespan& timeout)
{
	int rc = 0;
	if (poll(timeout, SELECT_READ))
	{
		int avail = available();
		if (buffer.size() < avail) buffer.resize(avail);

		do
		{
			if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();
			rc = ::recv(_sockfd, buffer.begin(), static_cast<int>(buffer.size()), flags);
		}
		while (_blocking && rc < 0 && lastError() == POCO_EINTR);
		if (rc < 0)
		{
			int err = lastError();
			if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
				;
			else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
				throw TimeoutException(err);
			else
				error(err);
		}
		if (rc < buffer.size()) buffer.resize(rc);
	}
	return rc;
}


int SocketImpl::sendTo(const void* buffer, int length, const SocketAddress& address, int flags)
{
	int rc;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) init(address.af());
#if defined(POCO_VXWORKS)
		rc = ::sendto(_sockfd, (char*) buffer, length, flags, (sockaddr*) address.addr(), address.length());
#else
		rc = ::sendto(_sockfd, reinterpret_cast<const char*>(buffer), length, flags, address.addr(), address.length());
#endif
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


int SocketImpl::sendTo(const SocketBufVec& buffers, const SocketAddress& address, int flags)
{
	int rc = 0;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) init(address.af());
#if defined(POCO_OS_FAMILY_WINDOWS)
		DWORD sent = 0;
		rc = WSASendTo(_sockfd, const_cast<LPWSABUF>(&buffers[0]),
						static_cast<DWORD>(buffers.size()), &sent,
						static_cast<DWORD>(flags),
						address.addr(), address.length(), 0, 0);
		if (rc == SOCKET_ERROR) error();
		rc = sent;
#elif defined(POCO_OS_FAMILY_UNIX)
		struct msghdr msgHdr;
		msgHdr.msg_name = const_cast<sockaddr*>(address.addr());
		msgHdr.msg_namelen = address.length();
		msgHdr.msg_iov = const_cast<iovec*>(&buffers[0]);
		msgHdr.msg_iovlen = buffers.size();
		msgHdr.msg_control = nullptr;
		msgHdr.msg_controllen = 0;
		msgHdr.msg_flags = flags;
		rc = sendmsg(_sockfd, &msgHdr, flags);
#endif
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


int SocketImpl::receiveFrom(void* buffer, int length, SocketAddress& address, int flags)
{
	sockaddr_storage abuffer;
	struct sockaddr* pSA = reinterpret_cast<struct sockaddr*>(&abuffer);
	poco_socklen_t saLen = sizeof(abuffer);
	poco_socklen_t* pSALen = &saLen;
	int rc = receiveFrom(buffer, length, &pSA, &pSALen, flags);
	if (rc >= 0)
	{
		address = SocketAddress(pSA, saLen);
	}
	return rc;
}


int SocketImpl::receiveFrom(void* buffer, int length, struct sockaddr** ppSA, poco_socklen_t** ppSALen, int flags)
{
	if (_blocking)
	{
		checkBrokenTimeout(SELECT_READ);
	}
	int rc;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();
		rc = ::recvfrom(_sockfd, reinterpret_cast<char*>(buffer), length, flags, *ppSA, *ppSALen);
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


int SocketImpl::receiveFrom(SocketBufVec& buffers, SocketAddress& address, int flags)
{
	sockaddr_storage abuffer;
	struct sockaddr* pSA = reinterpret_cast<struct sockaddr*>(&abuffer);
	poco_socklen_t saLen = sizeof(abuffer);
	poco_socklen_t* pSALen = &saLen;
	int rc = receiveFrom(buffers, &pSA, &pSALen, flags);
	if(rc >= 0)
	{
		address = SocketAddress(pSA, saLen);
	}
	return rc;
}


int SocketImpl::receiveFrom(SocketBufVec& buffers, struct sockaddr** pSA, poco_socklen_t** ppSALen, int flags)
{
	if (_blocking)
	{
		checkBrokenTimeout(SELECT_READ);
	}
	int rc = 0;
	do
	{
		if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();
#if defined(POCO_OS_FAMILY_WINDOWS)
		DWORD recvd = 0;
		DWORD dwFlags = static_cast<DWORD>(flags);
		rc = WSARecvFrom(_sockfd, &buffers[0], static_cast<DWORD>(buffers.size()),
						&recvd, &dwFlags, *pSA, *ppSALen, 0, 0);
		if (rc == SOCKET_ERROR) error();
		rc = recvd;
#elif defined(POCO_OS_FAMILY_UNIX)
		struct msghdr msgHdr;
		msgHdr.msg_name = *pSA;
		msgHdr.msg_namelen = **ppSALen;
		msgHdr.msg_iov = &buffers[0];
		msgHdr.msg_iovlen = buffers.size();
		msgHdr.msg_control = nullptr;
		msgHdr.msg_controllen = 0;
		msgHdr.msg_flags = flags;
		rc = recvmsg(_sockfd, &msgHdr, flags);
		if (rc >= 0) **ppSALen = msgHdr.msg_namelen;
#endif
	}
	while (_blocking && rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0)
	{
		int err = lastError();
		if (!_blocking && (err == POCO_EAGAIN || err == POCO_EWOULDBLOCK))
			;
		else if (err == POCO_EAGAIN || err == POCO_ETIMEDOUT)
			throw TimeoutException(err);
		else
			error(err);
	}
	return rc;
}


void SocketImpl::sendUrgent(unsigned char data)
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	int rc = ::send(_sockfd, reinterpret_cast<const char*>(&data), sizeof(data), MSG_OOB);
	if (rc < 0) error();
}


std::streamsize SocketImpl::sendFile(FileInputStream& fileInputStream, std::streamoff offset, std::streamsize count)
{
	if (!getBlocking()) throw NetException("sendFile() not supported for non-blocking sockets");

#ifdef POCO_HAVE_SENDFILE
	if (secure())
	{
		return sendFileBlockwise(fileInputStream, offset, count);
	}
	else
	{
		return sendFileNative(fileInputStream, offset, count);
	}
#else
	return sendFileBlockwise(fileInputStream, offset, count);
#endif
}


int SocketImpl::available()
{
	int result = 0;
	ioctl(FIONREAD, result);
#if (POCO_OS != POCO_OS_LINUX)
	if (result && (type() == SOCKET_TYPE_DATAGRAM))
	{
		std::vector<char> buf(result);
		result = recvfrom(sockfd(), &buf[0], result, MSG_PEEK, nullptr, nullptr);
	}
#endif
	return result;
}


bool SocketImpl::secure() const
{
	return false;
}


bool SocketImpl::poll(const Poco::Timespan& timeout, int mode)
{
	poco_socket_t sockfd = _sockfd;
	if (sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

#if defined(POCO_HAVE_FD_EPOLL)
#ifdef WEPOLL_H_
	HANDLE epollfd = epoll_create(1);
#else
	int epollfd = epoll_create(1);
#endif

#ifdef WEPOLL_H_
	if (!epollfd)
#else
	if (epollfd < 0)
#endif
	{
		error("Can't create epoll queue");
	}

	struct epoll_event evin;
	memset(&evin, 0, sizeof(evin));

	if (mode & SELECT_READ)
		evin.events |= EPOLLIN;
	if (mode & SELECT_WRITE)
		evin.events |= EPOLLOUT;
	if (mode & SELECT_ERROR)
		evin.events |= EPOLLERR;

	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &evin) < 0)
	{
		::close(epollfd);
		error("Can't insert socket to epoll queue");
	}

	Poco::Timespan remainingTime(timeout);
	int rc;
	do
	{
		struct epoll_event evout;
		memset(&evout, 0, sizeof(evout));

		Poco::Timestamp start;
		rc = epoll_wait(epollfd, &evout, 1, static_cast<int>(remainingTime.totalMilliseconds()));
		if (rc < 0 && lastError() == POCO_EINTR)
		{
			Poco::Timestamp end;
			Poco::Timespan waited = end - start;
			if (waited < remainingTime)
				remainingTime -= waited;
			else
				remainingTime = 0;
		}
	}
	while (rc < 0 && lastError() == POCO_EINTR);

	::close(epollfd);
	if (rc < 0) error();
	return rc > 0;

#elif defined(POCO_HAVE_FD_POLL)

	pollfd pollBuf;

	memset(&pollBuf, 0, sizeof(pollfd));
	pollBuf.fd = _sockfd;
	if (mode & SELECT_READ) pollBuf.events |= POLLIN;
	if (mode & SELECT_WRITE) pollBuf.events |= POLLOUT;

	Poco::Timespan remainingTime(timeout);
	int rc;
	do
	{
		Poco::Timestamp start;
#ifdef _WIN32
		rc = WSAPoll(&pollBuf, 1, static_cast<INT>(remainingTime.totalMilliseconds()));
#else
		rc = ::poll(&pollBuf, 1, remainingTime.totalMilliseconds());
#endif
		if (rc < 0 && lastError() == POCO_EINTR)
		{
			Poco::Timestamp end;
			Poco::Timespan waited = end - start;
			if (waited < remainingTime)
				remainingTime -= waited;
			else
				remainingTime = 0;
		}
	}
	while (rc < 0 && lastError() == POCO_EINTR);
	if (rc < 0) error();
	return rc > 0;

#else

	fd_set fdRead;
	fd_set fdWrite;
	fd_set fdExcept;
	FD_ZERO(&fdRead);
	FD_ZERO(&fdWrite);
	FD_ZERO(&fdExcept);
	if (mode & SELECT_READ)
	{
		FD_SET(sockfd, &fdRead);
	}
	if (mode & SELECT_WRITE)
	{
		FD_SET(sockfd, &fdWrite);
	}
	if (mode & SELECT_ERROR)
	{
		FD_SET(sockfd, &fdExcept);
	}
	Poco::Timespan remainingTime(timeout);
	int errorCode = POCO_ENOERR;
	int rc;
	do
	{
		struct timeval tv;
		tv.tv_sec  = (long) remainingTime.totalSeconds();
		tv.tv_usec = (long) remainingTime.useconds();
		Poco::Timestamp start;
		rc = ::select(int(sockfd) + 1, &fdRead, &fdWrite, &fdExcept, &tv);
		if (rc < 0 && (errorCode = lastError()) == POCO_EINTR)
		{
			Poco::Timestamp end;
			Poco::Timespan waited = end - start;
			if (waited < remainingTime)
				remainingTime -= waited;
			else
				remainingTime = 0;
		}
	}
	while (rc < 0 && errorCode == POCO_EINTR);
	if (rc < 0) error(errorCode);
	return rc > 0;

#endif // POCO_HAVE_FD_EPOLL
}


int SocketImpl::getError()
{
	int result;
	getOption(SOL_SOCKET, SO_ERROR, result);
	return result;
}


void SocketImpl::setSendBufferSize(int size)
{
	setOption(SOL_SOCKET, SO_SNDBUF, size);
}


int SocketImpl::getSendBufferSize()
{
	int result;
	getOption(SOL_SOCKET, SO_SNDBUF, result);
	return result;
}


void SocketImpl::setReceiveBufferSize(int size)
{
	setOption(SOL_SOCKET, SO_RCVBUF, size);
}


int SocketImpl::getReceiveBufferSize()
{
	int result;
	getOption(SOL_SOCKET, SO_RCVBUF, result);
	return result;
}


void SocketImpl::setSendTimeout(const Poco::Timespan& timeout)
{
#if defined(_WIN32) && !defined(POCO_BROKEN_TIMEOUTS)
	int value = (int) timeout.totalMilliseconds();
	setOption(SOL_SOCKET, SO_SNDTIMEO, value);
#elif !defined(POCO_BROKEN_TIMEOUTS)
	setOption(SOL_SOCKET, SO_SNDTIMEO, timeout);
#endif
	if (_isBrokenTimeout)
		_sndTimeout = timeout;
}


Poco::Timespan SocketImpl::getSendTimeout()
{
	Timespan result;
#if defined(_WIN32) && !defined(POCO_BROKEN_TIMEOUTS)
	int value;
	getOption(SOL_SOCKET, SO_SNDTIMEO, value);
	result = Timespan::TimeDiff(value)*1000;
#elif !defined(POCO_BROKEN_TIMEOUTS)
	getOption(SOL_SOCKET, SO_SNDTIMEO, result);
#endif
	if (_isBrokenTimeout)
		result = _sndTimeout;
	return result;
}


void SocketImpl::setReceiveTimeout(const Poco::Timespan& timeout)
{
#ifndef POCO_BROKEN_TIMEOUTS
#if defined(_WIN32)
	int value = (int) timeout.totalMilliseconds();
	setOption(SOL_SOCKET, SO_RCVTIMEO, value);
#else
	setOption(SOL_SOCKET, SO_RCVTIMEO, timeout);
#endif
#endif
	if (_isBrokenTimeout)
		_recvTimeout = timeout;
}


Poco::Timespan SocketImpl::getReceiveTimeout()
{
	Timespan result;
#if defined(_WIN32) && !defined(POCO_BROKEN_TIMEOUTS)
	int value;
	getOption(SOL_SOCKET, SO_RCVTIMEO, value);
	result = Timespan::TimeDiff(value)*1000;
#elif !defined(POCO_BROKEN_TIMEOUTS)
	getOption(SOL_SOCKET, SO_RCVTIMEO, result);
#endif
	if (_isBrokenTimeout)
		result = _recvTimeout;
	return result;
}


SocketAddress SocketImpl::address()
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	sockaddr_storage buffer;
	struct sockaddr* pSA = reinterpret_cast<struct sockaddr*>(&buffer);
	poco_socklen_t saLen = sizeof(buffer);
	int rc = ::getsockname(_sockfd, pSA, &saLen);
	if (rc == 0)
		return SocketAddress(pSA, saLen);
	else
		error();
	return SocketAddress();
}


SocketAddress SocketImpl::peerAddress()
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	sockaddr_storage buffer;
	struct sockaddr* pSA = reinterpret_cast<struct sockaddr*>(&buffer);
	poco_socklen_t saLen = sizeof(buffer);
	int rc = ::getpeername(_sockfd, pSA, &saLen);
	if (rc == 0)
		return SocketAddress(pSA, saLen);
	else
		error();
	return SocketAddress();
}


void SocketImpl::setOption(int level, int option, int value)
{
	setRawOption(level, option, &value, sizeof(value));
}


void SocketImpl::setOption(int level, int option, unsigned value)
{
	setRawOption(level, option, &value, sizeof(value));
}


void SocketImpl::setOption(int level, int option, unsigned char value)
{
	setRawOption(level, option, &value, sizeof(value));
}


void SocketImpl::setOption(int level, int option, const IPAddress& value)
{
	setRawOption(level, option, value.addr(), value.length());
}


void SocketImpl::setOption(int level, int option, const Poco::Timespan& value)
{
	struct timeval tv;
	tv.tv_sec  = (long) value.totalSeconds();
	tv.tv_usec = (long) value.useconds();

	setRawOption(level, option, &tv, sizeof(tv));
}


void SocketImpl::setRawOption(int level, int option, const void* value, poco_socklen_t length)
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

#if defined(POCO_VXWORKS)
	int rc = ::setsockopt(_sockfd, level, option, (char*) value, length);
#else
	int rc = ::setsockopt(_sockfd, level, option, reinterpret_cast<const char*>(value), length);
#endif
	if (rc == -1) error();
}


void SocketImpl::getOption(int level, int option, int& value)
{
	poco_socklen_t len = sizeof(value);
	getRawOption(level, option, &value, len);
}


void SocketImpl::getOption(int level, int option, unsigned& value)
{
	poco_socklen_t len = sizeof(value);
	getRawOption(level, option, &value, len);
}


void SocketImpl::getOption(int level, int option, unsigned char& value)
{
	poco_socklen_t len = sizeof(value);
	getRawOption(level, option, &value, len);
}


void SocketImpl::getOption(int level, int option, Poco::Timespan& value)
{
	struct timeval tv;
	poco_socklen_t len = sizeof(tv);
	getRawOption(level, option, &tv, len);
	value.assign(tv.tv_sec, tv.tv_usec);
}


void SocketImpl::getOption(int level, int option, IPAddress& value)
{
	char buffer[IPAddress::MAX_ADDRESS_LENGTH];
	poco_socklen_t len = sizeof(buffer);
	getRawOption(level, option, buffer, len);
	value = IPAddress(buffer, len);
}


void SocketImpl::getRawOption(int level, int option, void* value, poco_socklen_t& length)
{
	if (_sockfd == POCO_INVALID_SOCKET) throw InvalidSocketException();

	int rc = ::getsockopt(_sockfd, level, option, reinterpret_cast<char*>(value), &length);
	if (rc == -1) error();
}


void SocketImpl::setLinger(bool on, int seconds)
{
	struct linger l;
	l.l_onoff  = on ? 1 : 0;
	l.l_linger = seconds;
	setRawOption(SOL_SOCKET, SO_LINGER, &l, sizeof(l));
}


void SocketImpl::getLinger(bool& on, int& seconds)
{
	struct linger l;
	poco_socklen_t len = sizeof(l);
	getRawOption(SOL_SOCKET, SO_LINGER, &l, len);
	on      = l.l_onoff != 0;
	seconds = l.l_linger;
}


void SocketImpl::setNoDelay(bool flag)
{
	int value = flag ? 1 : 0;
	setOption(IPPROTO_TCP, TCP_NODELAY, value);
}


bool SocketImpl::getNoDelay()
{
	int value(0);
	getOption(IPPROTO_TCP, TCP_NODELAY, value);
	return value != 0;
}


void SocketImpl::setKeepAlive(bool flag)
{
	int value = flag ? 1 : 0;
	setOption(SOL_SOCKET, SO_KEEPALIVE, value);
}


bool SocketImpl::getKeepAlive()
{
	int value(0);
	getOption(SOL_SOCKET, SO_KEEPALIVE, value);
	return value != 0;
}


void SocketImpl::setReuseAddress(bool flag)
{
	int value = flag ? 1 : 0;
	setOption(SOL_SOCKET, SO_REUSEADDR, value);
#ifdef POCO_OS_FAMILY_WINDOWS
	value = flag ? 0 : 1;
	setOption(SOL_SOCKET, SO_EXCLUSIVEADDRUSE, value);
#endif
}


bool SocketImpl::getReuseAddress()
{
	bool ret = false;
	int value(0);
	getOption(SOL_SOCKET, SO_REUSEADDR, value);
	ret = (value != 0);
#ifdef POCO_OS_FAMILY_WINDOWS
	value = 0;
	getOption(SOL_SOCKET, SO_EXCLUSIVEADDRUSE, value);
	ret = ret && (value == 0);
#endif
	return ret;
}


void SocketImpl::setReusePort(bool flag)
{
#ifdef SO_REUSEPORT
	try
	{
		int value = flag ? 1 : 0;
		setOption(SOL_SOCKET, SO_REUSEPORT, value);
	}
	catch (const IOException&)
	{
		// ignore error, since not all implementations
		// support SO_REUSEPORT, even if the macro
		// is defined.
	}
#endif
}


bool SocketImpl::getReusePort()
{
#ifdef SO_REUSEPORT
	int value(0);
	getOption(SOL_SOCKET, SO_REUSEPORT, value);
	return value != 0;
#else
	return false;
#endif
}


void SocketImpl::setOOBInline(bool flag)
{
	int value = flag ? 1 : 0;
	setOption(SOL_SOCKET, SO_OOBINLINE, value);
}


bool SocketImpl::getOOBInline()
{
	int value(0);
	getOption(SOL_SOCKET, SO_OOBINLINE, value);
	return value != 0;
}


void SocketImpl::setBroadcast(bool flag)
{
	int value = flag ? 1 : 0;
	setOption(SOL_SOCKET, SO_BROADCAST, value);
}


bool SocketImpl::getBroadcast()
{
	int value(0);
	getOption(SOL_SOCKET, SO_BROADCAST, value);
	return value != 0;
}


void SocketImpl::setBlocking(bool flag)
{
#if !defined(POCO_OS_FAMILY_UNIX)
	int arg = flag ? 0 : 1;
	ioctl(FIONBIO, arg);
#else
	int arg = fcntl(F_GETFL);
	long flags = arg & ~O_NONBLOCK;
	if (!flag) flags |= O_NONBLOCK;
	(void) fcntl(F_SETFL, flags);
#endif
	_blocking = flag;
}


int SocketImpl::socketError()
{
	int result(0);
	getOption(SOL_SOCKET, SO_ERROR, result);
	return result;
}


void SocketImpl::init(int af)
{
	initSocket(af, SOCK_STREAM);
}


void SocketImpl::initSocket(int af, int type, int proto)
{
	poco_assert (_sockfd == POCO_INVALID_SOCKET);

	_sockfd = ::socket(af, type, proto);
	if (_sockfd == POCO_INVALID_SOCKET)
		error();

#if defined(__MACH__) && defined(__APPLE__) || defined(__FreeBSD__)
	// SIGPIPE sends a signal that if unhandled (which is the default)
	// will crash the process. This only happens on UNIX, and not Linux.
	//
	// In order to have POCO sockets behave the same across platforms, it is
	// best to just ignore SIGPIPE altogether.
	setOption(SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
}


void SocketImpl::ioctl(poco_ioctl_request_t request, int& arg)
{
#if defined(_WIN32)
	int rc = ioctlsocket(_sockfd, request, reinterpret_cast<u_long*>(&arg));
#elif defined(POCO_VXWORKS)
	int rc = ::ioctl(_sockfd, request, (int) &arg);
#else
	int rc = ::ioctl(_sockfd, request, &arg);
#endif
	if (rc != 0) error();
}


void SocketImpl::ioctl(poco_ioctl_request_t request, void* arg)
{
#if defined(_WIN32)
	int rc = ioctlsocket(_sockfd, request, reinterpret_cast<u_long*>(arg));
#elif defined(POCO_VXWORKS)
	int rc = ::ioctl(_sockfd, request, (int) arg);
#else
	int rc = ::ioctl(_sockfd, request, arg);
#endif
	if (rc != 0) error();
}


#if defined(POCO_OS_FAMILY_UNIX)
int SocketImpl::fcntl(poco_fcntl_request_t request)
{
	int rc = ::fcntl(_sockfd, request);
	if (rc == -1) error();
	return rc;
}


int SocketImpl::fcntl(poco_fcntl_request_t request, long arg)
{
	int rc = ::fcntl(_sockfd, request, arg);
	if (rc == -1) error();
	return rc;
}
#endif


void SocketImpl::reset(poco_socket_t aSocket)
{
	_sockfd = aSocket;
}


void SocketImpl::error()
{
	int err = lastError();
	std::string empty;
	error(err, empty);
}


void SocketImpl::error(const std::string& arg)
{
	error(lastError(), arg);
}


void SocketImpl::error(int code)
{
	std::string arg;
	error(code, arg);
}


void SocketImpl::error(int code, const std::string& arg)
{
	switch (code)
	{
	case POCO_ENOERR: return;
	case POCO_ESYSNOTREADY:
		throw NetException("Net subsystem not ready", code);
	case POCO_ENOTINIT:
		throw NetException("Net subsystem not initialized", code);
	case POCO_EINTR:
		throw IOException("Interrupted", code);
	case POCO_EACCES:
		throw IOException("Permission denied", code);
	case POCO_EFAULT:
		throw IOException("Bad address", code);
	case POCO_EINVAL:
		throw InvalidArgumentException(code);
	case POCO_EMFILE:
		throw IOException("Too many open files", code);
	case POCO_EWOULDBLOCK:
		throw IOException("Operation would block", code);
	case POCO_EINPROGRESS:
		throw IOException("Operation now in progress", code);
	case POCO_EALREADY:
		throw IOException("Operation already in progress", code);
	case POCO_ENOTSOCK:
		throw IOException("Socket operation attempted on non-socket", code);
	case POCO_EDESTADDRREQ:
		throw NetException("Destination address required", code);
	case POCO_EMSGSIZE:
		throw NetException("Message too long", code);
	case POCO_EPROTOTYPE:
		throw NetException("Wrong protocol type", code);
	case POCO_ENOPROTOOPT:
		throw NetException("Protocol not available", code);
	case POCO_EPROTONOSUPPORT:
		throw NetException("Protocol not supported", code);
	case POCO_ESOCKTNOSUPPORT:
		throw NetException("Socket type not supported", code);
	case POCO_ENOTSUP:
		throw NetException("Operation not supported", code);
	case POCO_EPFNOSUPPORT:
		throw NetException("Protocol family not supported", code);
	case POCO_EAFNOSUPPORT:
		throw NetException("Address family not supported", code);
	case POCO_EADDRINUSE:
		throw NetException("Address already in use", arg, code);
	case POCO_EADDRNOTAVAIL:
		throw NetException("Cannot assign requested address", arg, code);
	case POCO_ENETDOWN:
		throw NetException("Network is down", code);
	case POCO_ENETUNREACH:
		throw NetException("Network is unreachable", code);
	case POCO_ENETRESET:
		throw NetException("Network dropped connection on reset", code);
	case POCO_ECONNABORTED:
		throw ConnectionAbortedException(code);
	case POCO_ECONNRESET:
		throw ConnectionResetException(code);
	case POCO_ENOBUFS:
		throw IOException("No buffer space available", code);
	case POCO_EISCONN:
		throw NetException("Socket is already connected", code);
	case POCO_ENOTCONN:
		throw NetException("Socket is not connected", code);
	case POCO_ESHUTDOWN:
		throw NetException("Cannot send after socket shutdown", code);
	case POCO_ETIMEDOUT:
		throw TimeoutException(code);
	case POCO_ECONNREFUSED:
		throw ConnectionRefusedException(arg, code);
	case POCO_EHOSTDOWN:
		throw NetException("Host is down", arg, code);
	case POCO_EHOSTUNREACH:
		throw NetException("No route to host", arg, code);
#if defined(POCO_OS_FAMILY_UNIX)
	case EPIPE:
		throw IOException("Broken pipe", code);
	case EBADF:
		throw IOException("Bad socket descriptor", code);
	case ENOENT:
		throw IOException("Not found", arg, code);
#endif
	default:
		throw IOException(NumberFormatter::format(code), arg, code);
	}
}


#ifdef POCO_HAVE_SENDFILE
#ifdef POCO_OS_FAMILY_WINDOWS


std::streamsize SocketImpl::sendFileNative(FileInputStream& fileInputStream, std::streamoff offset, std::streamsize count)
{
	FileIOS::NativeHandle fd = fileInputStream.nativeHandle();
	if (count == 0) count = fileInputStream.size() - offset;
	LARGE_INTEGER offsetHelper;
	offsetHelper.QuadPart = offset;
	OVERLAPPED overlapped;
	memset(&overlapped, 0, sizeof(overlapped));
	overlapped.Offset = offsetHelper.LowPart;
	overlapped.OffsetHigh =  offsetHelper.HighPart;
	overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (overlapped.hEvent == nullptr)
	{
		int err = GetLastError();
		error(err);
	}
	bool result = TransmitFile(_sockfd, fd, count, 0, &overlapped, nullptr, 0);
	if (!result)
	{
		int err = WSAGetLastError();
		if ((err != ERROR_IO_PENDING) && (WSAGetLastError() != WSA_IO_PENDING)) 
		{
			CloseHandle(overlapped.hEvent);
			error(err);
		}
		WaitForSingleObject(overlapped.hEvent, INFINITE);
	}
	CloseHandle(overlapped.hEvent);
	return count;
}


#else


namespace
{
	std::streamoff sendFileUnix(poco_socket_t sd, FileIOS::NativeHandle fd, std::streamoff offset, std::streamsize count)
	{
		std::streamoff sent = 0;
		#ifdef __USE_LARGEFILE64
			off_t noffset = offset;
			sent = sendfile64(sd, fd, &noffset, count);
		#else
			#if POCO_OS == POCO_OS_LINUX && !defined(POCO_EMSCRIPTEN)
				off_t noffset = offset;
				sent = sendfile(sd, fd, &noffset, count);
			#elif POCO_OS == POCO_OS_MAC_OS_X
				off_t len = count;
				int result = sendfile(fd, sd, offset, &len, NULL, 0);
				if (result < 0)
				{
					sent = -1;
				} 
				else 
				{
					sent = len;
				}
			#elif POCO_OS == POCO_OS_FREE_BSD
				off_t sbytes;
				int result = sendfile(fd, sd, offset, count, NULL, &sbytes, 0);
				if (result < 0)
				{
					sent = -1;
				} 
				else 
				{
					sent = sbytes;
				}
			#else
				throw Poco::NotImplementedException("native sendfile not implemented for this platform");
			#endif
		#endif
		return sent;
	}	
}


std::streamsize SocketImpl::sendFileNative(FileInputStream& fileInputStream, std::streamoff offset, std::streamsize count)
{
	FileIOS::NativeHandle fd = fileInputStream.nativeHandle();
	if (count == 0) count = fileInputStream.size() - offset;
	std::streamsize sent = 0;
	while (count > 0)
	{
		std::streamoff rc = sendFileUnix(_sockfd, fd, offset, count);
		if (rc >= 0)
		{
			sent += rc;
			offset += rc;
			count -= rc;
		}
		else
		{
			error(errno);
		}
	}
	return sent;
}


#endif // POCO_OS_FAMILY_WINDOWS
#endif // POCO_HAVE_SENDFILE


std::streamsize SocketImpl::sendFileBlockwise(FileInputStream& fileInputStream, std::streamoff offset, std::streamsize count)
{
	fileInputStream.seekg(offset, std::ios_base::beg);
	Poco::Buffer<char> buffer(8192);
	std::size_t bufferSize = buffer.size();
	if (count > 0 && bufferSize > count) bufferSize = count;

	std::streamsize len = 0;
	fileInputStream.read(buffer.begin(), bufferSize);
	std::streamsize n = fileInputStream.gcount();
	while (n > 0 && (count == 0 || len < count))
	{
		len += n;
		sendBytes(buffer.begin(), static_cast<int>(n));
		if (count > 0 && len < count)
		{
			const std::size_t remaining = count - len;
			if (bufferSize > remaining) bufferSize = remaining;
		}
		if (fileInputStream)
		{
			fileInputStream.read(buffer.begin(), bufferSize);
			n = fileInputStream.gcount();
		}
		else n = 0;
	}
	return len;
}


} } // namespace Poco::Net
