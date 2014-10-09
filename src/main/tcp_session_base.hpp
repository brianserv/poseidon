#ifndef POSEIDON_TCP_SESSION_BASE_HPP_
#define POSEIDON_TCP_SESSION_BASE_HPP_

#include "../cxx_ver.hpp"
#include "session_base.hpp"
#include <string>
#include <cstddef>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include "raii.hpp"
#include "atomic.hpp"
#include "stream_buffer.hpp"

namespace Poseidon {

class TcpSessionBase : public SessionBase {
private:
	ScopedFile m_socket;
	std::string m_remoteIp;

	volatile bool m_shutdown;
	mutable boost::mutex m_bufferMutex;
	StreamBuffer m_sendBuffer;

protected:
	explicit TcpSessionBase(Move<ScopedFile> socket);
	virtual ~TcpSessionBase();

public:
	void onReadAvail(const void *data, std::size_t size) = 0;
	void sendUsingMove(StreamBuffer &buffer);
	void shutdown();
	void forceShutdown();

	int getFd() const {
		return m_socket.get();
	}
	// 如果 size 为零则返回所有待发送字节数。
	std::size_t peekWriteAvail(boost::mutex::scoped_lock &lock,
		void *data, std::size_t size) const;
	// 从队列中移除指定的字节数。
	void notifyWritten(std::size_t size);

	const std::string &getRemoteIp() const {
		return m_remoteIp;
	}
	bool hasBeenShutdown() const {
		return atomicLoad(m_shutdown);
	}

	void send(const void *data, std::size_t size){
		StreamBuffer tmp(data, size);
		sendUsingMove(tmp);
	}
	void send(const StreamBuffer &buffer){
		StreamBuffer tmp(buffer);
		sendUsingMove(tmp);
	}
#ifdef POSEIDON_CXX11
	void send(StreamBuffer &&buffer){
		sendUsingMove(buffer);
	}
	void sendUsingMove(StreamBuffer &&buffer){
		sendUsingMove(buffer);
	}
#endif
};

}

#endif
