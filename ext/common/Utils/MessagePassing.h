/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2012 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_MESSAGE_PASSING_H_
#define _PASSENGER_MESSAGE_PASSING_H_


#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <oxt/macros.hpp>
#include <Exceptions.h>
#include <Utils/SystemTime.h>
#include <Utils/VariantMap.h>

#include <list>


namespace Passenger {

using namespace std;
using namespace boost;


/**
 * A simple in-process message passing library. Each message has a name, a bunch
 * of named arguments and an arbitrary object. Recipients can wait for a certain
 * message to arrive, possibly with a timeout. The receive function will return
 * as soon as the mailbox contains at least one message with the given name,
 * and removes that message from the mailbox, returning it.
 *
 * This library is designed for convenience and correctness, not speed. Messages
 * are allocated on the heap and are never copied: only their smart pointers are
 * passed around. This way you can pass arbitrary C++ objects.
 */
class MessageBox;
struct Message;
typedef shared_ptr<MessageBox> MessageBoxPtr;
typedef shared_ptr<Message> MessagePtr;

inline void _sendToMessageBox(const MessageBoxPtr &messageBox, const MessagePtr &message);


struct Message {
	string name;
	VariantMap args;
	weak_ptr<MessageBox> from;
	void *data;
	void (*freeData)(void *p);

	Message()
		: data(0),
		  freeData(0)
		{ }

	Message(const string &_name)
		: name(_name),
		  data(0),
		  freeData(0)
		{ }

	Message(const MessageBoxPtr &from, const string &_name)
		: name(_name),
		  data(0),
		  freeData(0)
	{
		setFrom(from);
	}

	~Message() {
		if (data != NULL && freeData != NULL) {
			freeData(data);
		}
	}

	void setFrom(const MessageBoxPtr &messageBox) {
		from = weak_ptr<MessageBox>(messageBox);
	}

	void sendReply(const MessagePtr &message) {
		MessageBoxPtr messageBox = from.lock();
		if (messageBox != NULL) {
			_sendToMessageBox(messageBox, message);
		}
	}

	void sendReply(const string &name) {
		sendReply(make_shared<Message>(name));
	}
};


class MessageBox: public enable_shared_from_this<MessageBox> {
	typedef list<MessagePtr> MessageList;
	typedef MessageList::iterator Iterator;
	typedef MessageList::const_iterator ConstIterator;

	mutable boost::mutex syncher;
	condition_variable cond;
	MessageList messages;

	Iterator search(const string &name) {
		Iterator it, end = messages.end();
		for (it = messages.begin(); it != end; it++) {
			const MessagePtr &message = *it;
			if (message->name == name) {
				return it;
			}
		}
		return end;
	}

	void substractTimePassed(unsigned long long *timeout, unsigned long long beginTime) {
		unsigned long long now = SystemTime::getMsec();
		unsigned long long diff;
		if (now > beginTime) {
			diff = now - beginTime;
		} else {
			diff = 0;
		}
		if (*timeout > diff) {
			*timeout -= diff;
		} else {
			*timeout = 0;
		}
	}

public:
	void send(const MessagePtr &message) {
		lock_guard<boost::mutex> l(syncher);
		message->setFrom(shared_from_this());
		messages.push_back(message);
		cond.notify_all();
	}

	void send(const string &name) {
		send(make_shared<Message>(name));
	}

	MessagePtr recv(const string &name, unsigned long long *timeout = NULL) {
		unique_lock<boost::mutex> l(syncher);
		posix_time::ptime deadline;
		unsigned long long beginTime;
		if (timeout != NULL) {
			beginTime = SystemTime::getUsec();
			deadline = posix_time::microsec_clock::local_time() +
				posix_time::microsec(*timeout);
		}

		Iterator it;
		while ((it = search(name)) == messages.end()) {
			if (timeout != NULL) {
				posix_time::time_duration diff = deadline -
					posix_time::microsec_clock::local_time();
				bool timedOut;
				if (diff.is_negative() < 0) {
					timedOut = true;
				} else {
					timedOut = !cond.timed_wait(l,
						posix_time::milliseconds(diff.total_milliseconds()));
				}
				if (timedOut) {
					substractTimePassed(timeout, beginTime);
					return MessagePtr();
				}
			} else {
				cond.wait(l);
			}
		}
		if (timeout != NULL) {
			substractTimePassed(timeout, beginTime);
		}

		MessagePtr result = *it;
		messages.erase(it);
		return result;
	}

	MessagePtr recvTE(const string &name, unsigned long long *timeout = NULL) {
		MessagePtr result = recv(name, timeout);
		if (result != NULL) {
			return result;
		} else {
			throw TimeoutException("Timeout receiving from message box");
		}
	}

	unsigned int size() const {
		lock_guard<boost::mutex> l(syncher);
		return messages.size();
	}
};


inline void
_sendToMessageBox(const MessageBoxPtr &messageBox, const MessagePtr &message) {
	messageBox->send(message);
}


} // namespace Passenger

#endif /* _PASSENGER_MESSAGE_PASSING_H_ */