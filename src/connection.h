// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_CONNECTION_H
#define FS_CONNECTION_H

#include "networkmessage.h"

static constexpr int32_t CONNECTION_WRITE_TIMEOUT = 30;
static constexpr int32_t CONNECTION_READ_TIMEOUT = 30;

class Protocol;
class OutputMessage;
class Connection;
class ServiceBase;
class ServicePort;

using Protocol_ptr = std::shared_ptr<Protocol>;
using OutputMessage_ptr = std::shared_ptr<OutputMessage>;
using Connection_ptr = std::shared_ptr<Connection>;
using ConnectionWeak_ptr = std::weak_ptr<Connection>;
using Service_ptr = std::shared_ptr<ServiceBase>;
using ServicePort_ptr = std::shared_ptr<ServicePort>;

namespace asio = boost::asio;
namespace system = boost::system;

class Connection : public std::enable_shared_from_this<Connection>
{
public:
	using SocketAddress = boost::asio::ip::address;

	enum ConnectionState
	{
		Pending,
		RequestCharacterList,
		GameWorldAuthentication,
		Game,
		Disconnected,
	};

	enum ChecksumMode
	{
		Disabled,
		Adler,
		Sequence
	};

	Connection(asio::io_context& ioc, std::shared_ptr<const ServicePort> service_port);
	~Connection();

	// non-copyable
	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	void accept(Protocol_ptr protocol = nullptr);
	void disconnect();
	void close_socket();
	void disconnect_and_close_socket();
	void send_message(const OutputMessage_ptr& msg);

	const SocketAddress& socket_address() const { return address; };

private:
	asio::ip::tcp::socket& getSocket() { return socket; }
	void parse_packet_header(const system::error_code& error);
	void parse_packet_body(const system::error_code& error);

	void send_message_to_socket(const OutputMessage_ptr& msg);
	void on_write_to_socket(const system::error_code& error);
	static void handle_socket_timeout(ConnectionWeak_ptr connection_weak, const system::error_code& error);

	std::shared_ptr<const ServicePort> service_port;
	friend class ServicePort;

	NetworkMessage msg;
	Protocol_ptr protocol;
	std::list<OutputMessage_ptr> server_messages;
	ConnectionState state = ConnectionState::Pending;
	time_t timeConnected;
	uint32_t packets_sent = 0;
	bool receivedFirst = false;
	bool receivedName = false;
	bool receivedLastChar = false;

	asio::steady_timer socket_read_timer;
	asio::steady_timer socket_write_timer;
	asio::ip::tcp::socket socket;
	asio::ip::address address;

	std::recursive_mutex connection_lock;
};

namespace tfs::net {

Connection_ptr create_connection(asio::io_context ioc, std::shared_ptr<const ServicePort> service_port);
void disconnect(const Connection_ptr& connection);
void disconnect_all();
bool has_connection_blocked(const Connection::SocketAddress& socket_address);

} // namespace tfs::net

#endif // FS_CONNECTION_H
