//
// AbstractObserver.h
//
// Library: Foundation
// Package: Notifications
// Module:  NotificationCenter
//
// Definition of the AbstractObserver class.
//
// Copyright (c) 2004-2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//


#ifndef Foundation_AbstractObserver_INCLUDED
#define Foundation_AbstractObserver_INCLUDED


#include "Poco/Foundation.h"
#include "Poco/Notification.h"


namespace Poco {


class Foundation_API AbstractObserver
	/// The base class for all instantiations of
	/// the Observer and NObserver template classes.
{
public:

	AbstractObserver();
	AbstractObserver(const AbstractObserver& observer);
	AbstractObserver(AbstractObserver&& observer);
	virtual ~AbstractObserver();

	AbstractObserver& operator = (const AbstractObserver& observer);
	AbstractObserver& operator = (AbstractObserver&& observer);

	virtual void notify(Notification* pNf) const = 0;

	virtual NotificationResult notifySync(Notification* pNf) const;
		/// Synchronous notification processing. Blocks and returns a result.
		/// Default implementation throws NotImplementedException.

	virtual bool equals(const AbstractObserver& observer) const = 0;

	POCO_DEPRECATED("use `bool accepts(Notification::Ptr&)` instead")
	virtual bool accepts(Notification* pNf, const char* pName) const = 0;

	virtual bool accepts(const Notification::Ptr& pNf) const = 0;

	virtual bool acceptsSync() const;
		/// Returns true if this observer supports synchronous notification processing.

	virtual AbstractObserver* clone() const = 0;

	virtual void start();
		/// No-op.
		/// This method can be implemented by inheriting classes which require
		/// explicit start in order to begin processing notifications.

	virtual void disable() = 0;

	virtual int backlog() const;
		/// Returns number of queued messages that this Observer has.
		/// For non-active (synchronous) observers, always returns zero.
};

//
// inlines
//

inline void AbstractObserver::start()
{
}


inline int AbstractObserver::backlog() const
{
	return 0;
}


} // namespace Poco


#endif // Foundation_AbstractObserver_INCLUDED
