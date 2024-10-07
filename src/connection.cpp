// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "connection.h"

#include "configmanager.h"
#include "outputmessage.h"
#include "protocol.h"
#include "server.h"
#include "tasks.h"

Connection::Connection(asio::io_context& ioc, std::shared_ptr<const ServicePort> service_port) :
    socket_read_timer(ioc),
    socket_write_timer(ioc),
    service_port(std::move(service_port)),
    socket(ioc),
    timeConnected(time(nullptr))
{}

Connection::~Connection() { close_socket(); }

void Connection::accept(Protocol_ptr protocol)
{
	std::lock_guard<std::recursive_mutex> lock(connection_lock);

	if (protocol) {
		this->protocol = protocol;
		g_dispatcher.addTask([=]() { protocol->onConnect(); });

		state = ConnectionState::GameWorldAuthentication;
	} else if (state == ConnectionState::Pending) {
		state = ConnectionState::RequestCharacterList;
	}

	system::error_code error;
	if (auto endpoint = socket.remote_endpoint(error); !error) {
		address = endpoint.address();
	}

	try {
		socket_read_timer.expires_after(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		socket_read_timer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const system::error_code& error) {
			    Connection::handle_socket_timeout(thisPtr, error);
		    });

		// Read size of the first packet
		auto bufferLength = !receivedLastChar && receivedName && state == ConnectionState::GameWorldAuthentication
		                        ? 1
		                        : NetworkMessage::HEADER_LENGTH;

		asio::async_read(socket, asio::buffer(msg.getBuffer(), bufferLength),
		                 [thisPtr = shared_from_this()](const system::error_code& error, auto /*bytes_transferred*/) {
			                 thisPtr->parse_packet_header(error);
		                 });
	} catch (system::system_error& e) {
		std::cout << "[Network error - Connection::accept] " << e.what() << std::endl;
		disconnect_and_close_socket();
	}
}

void Connection::disconnect()
{
	// any thread
	tfs::net::disconnect(shared_from_this());

	std::lock_guard<std::recursive_mutex> lock(connection_lock);

	state = ConnectionState::Disconnected;

	if (protocol) {
		g_dispatcher.addTask([protocol = protocol]() { protocol->release(); });
	}

	if (server_messages.empty()) {
		close_socket();
	} else {
		// will be closed by the destructor or on_write_to_socket
	}
}

void Connection::close_socket()
{
	if (socket.is_open()) {
		try {
			socket_read_timer.cancel();
			socket_write_timer.cancel();

			system::error_code error;
			socket.shutdown(asio::ip::tcp::socket::shutdown_both, error);
			socket.close(error);
		} catch (system::system_error& e) {
			std::cout << "[Network error - Connection::close_socket] " << e.what() << std::endl;
		}
	}
}

void Connection::disconnect_and_close_socket()
{
	disconnect();
	close_socket();
}

void Connection::send_message(const OutputMessage_ptr& message)
{
	std::lock_guard<std::recursive_mutex> lock(connection_lock);

	if (state == ConnectionState::Disconnected) {
		return;
	}

	bool noPendingWrite = server_messages.empty();
	server_messages.emplace_back(message);
	if (noPendingWrite) {
		send_message_to_socket(message);
	}
}

void Connection::parse_packet_header(const system::error_code& error_on_read)
{
	std::lock_guard<std::recursive_mutex> lock(connection_lock);

	socket_read_timer.cancel();

	if (error_on_read) {
		disconnect_and_close_socket();
		return;
	}

	if (state == ConnectionState::Disconnected) {
		return;
	}

	uint32_t timePassed = std::max<uint32_t>(1, (time(nullptr) - timeConnected) + 1);
	if ((++packetsSent / timePassed) > static_cast<uint32_t>(getNumber(ConfigManager::MAX_PACKETS_PER_SECOND))) {
		std::cout << socket_address() << " disconnected for exceeding packet per second limit." << std::endl;
		disconnect();
		return;
	}

	if (!receivedLastChar && state == ConnectionState::GameWorldAuthentication) {
		uint8_t* msgBuffer = msg.getBuffer();

		if (!receivedName && msgBuffer[1] == 0x00) {
			receivedLastChar = true;
		} else if (!receivedName) {
			receivedName = true;
			accept();
			return;
		} else if (msgBuffer[0] == 0x0A) {
			receivedLastChar = true;
			accept();
			return;
		}
	}

	if (receivedLastChar && state == ConnectionState::GameWorldAuthentication) {
		state = ConnectionState::Game;
	}

	if (timePassed > 2) {
		timeConnected = time(nullptr);
		packetsSent = 0;
	}

	uint16_t size = msg.getLengthHeader();
	if (size == 0 || size >= NETWORKMESSAGE_MAXSIZE - 16) {
		disconnect_and_close_socket();
		return;
	}

	try {
		socket_read_timer.expires_after(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		socket_read_timer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const system::error_code& error) {
			    Connection::handle_socket_timeout(thisPtr, error);
		    });

		// Read packet content
		msg.setLength(size + NetworkMessage::HEADER_LENGTH);

		asio::async_read(socket, asio::buffer(msg.getBodyBuffer(), size),
		                 [thisPtr = shared_from_this()](const system::error_code& error, auto /*bytes_transferred*/) {
			                 thisPtr->parse_packet_body(error);
		                 });
	} catch (system::system_error& e) {
		std::cout << "[Network error - Connection::parseHeader] " << e.what() << std::endl;
		disconnect_and_close_socket();
	}
}

void Connection::parse_packet_body(const system::error_code& error)
{
	std::lock_guard<std::recursive_mutex> lock(connection_lock);

	socket_read_timer.cancel();

	if (error) {
		disconnect_and_close_socket();
		return;
	}

	if (state == ConnectionState::Disconnected) {
		return;
	}

	// Read potential checksum bytes
	msg.get<uint32_t>();

	if (!receivedFirst) {
		receivedFirst = true;

		if (!protocol) {
			// Skip deprecated checksum bytes (with clients that aren't using it in mind)
			uint16_t len = msg.getLength();
			if (len < 280 && len != 151) {
				msg.skipBytes(-NetworkMessage::CHECKSUM_LENGTH);
			}

			// Game protocol has already been created at this point
			protocol = service_port->make_protocol(msg, shared_from_this());
			if (!protocol) {
				disconnect();
				close_socket();
				return;
			}
		} else {
			msg.skipBytes(1); // Skip protocol ID
		}

		protocol->onRecvFirstMessage(msg);
	} else {
		protocol->onRecvMessage(msg); // Send the packet to the current protocol
	}

	try {
		socket_read_timer.expires_after(std::chrono::seconds(CONNECTION_READ_TIMEOUT));
		socket_read_timer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const system::error_code& error) {
			    Connection::handle_socket_timeout(thisPtr, error);
		    });

		// Wait to the next packet
		asio::async_read(socket, asio::buffer(msg.getBuffer(), NetworkMessage::HEADER_LENGTH),
		                 [thisPtr = shared_from_this()](const system::error_code& error, auto /*bytes_transferred*/) {
			                 thisPtr->parse_packet_header(error);
		                 });
	} catch (system::system_error& e) {
		std::cout << "[Network error - Connection::parsePacket] " << e.what() << std::endl;
		disconnect_and_close_socket();
	}
}

void Connection::send_message_to_socket(const OutputMessage_ptr& msg)
{
	protocol->onSendMessage(msg);

	try {
		socket_write_timer.expires_after(std::chrono::seconds(CONNECTION_WRITE_TIMEOUT));
		socket_write_timer.async_wait(
		    [thisPtr = std::weak_ptr<Connection>(shared_from_this())](const system::error_code& error) {
			    Connection::handle_socket_timeout(thisPtr, error);
		    });

		asio::async_write(socket, asio::buffer(msg->getOutputBuffer(), msg->getLength()),
		                  [thisPtr = shared_from_this()](const system::error_code& error, auto /*bytes_transferred*/) {
			                  thisPtr->on_write_to_socket(error);
		                  });
	} catch (system::system_error& e) {
		std::cout << "[Network error - Connection::send_message_to_socket] " << e.what() << std::endl;
		disconnect_and_close_socket();
	}
}

void Connection::on_write_to_socket(const system::error_code& error)
{
	std::lock_guard<std::recursive_mutex> lock(connection_lock);

	socket_write_timer.cancel();

	server_messages.pop_front();
	if (error) {
		server_messages.clear();
		disconnect_and_close_socket();
		return;
	}

	if (!server_messages.empty()) {
		send_message_to_socket(server_messages.front());
	} else if (state == ConnectionState::Disconnected) {
		close_socket();
	}
}

void Connection::handle_socket_timeout(ConnectionWeak_ptr connection_weak, const system::error_code& error)
{
	if (error == asio::error::operation_aborted) {
		// The timer has been cancelled manually
		return;
	}

	if (auto connection = connection_weak.lock()) {
		connection->disconnect_and_close_socket();
	}
}

namespace {

std::unordered_set<Connection_ptr> connections;
std::mutex connections_lock;

struct ConnectionBlock
{
	uint64_t last_attempt;
	uint64_t block_time = 0;
	uint32_t count = 1;
};

std::map<Connection::SocketAddress, ConnectionBlock> connections_block;
std::recursive_mutex connections_block_lock;

} // namespace

Connection_ptr tfs::net::create_connection(asio::io_context ioc, std::shared_ptr<const ServicePort> service_port)
{
	std::lock_guard<std::mutex> lock(connections_lock);

	auto connection = std::make_shared<Connection>(ioc, service_port);
	connections.insert(connection);
	return connection;
}

void tfs::net::disconnect(const Connection_ptr& connection)
{
	std::lock_guard<std::mutex> lock(connections_lock);

	connections.erase(connection);
}

void tfs::net::disconnect_all()
{
	std::lock_guard<std::mutex> lock(connections_lock);

	for (const auto& connection : connections) {
		connection->close_socket();
	}

	connections.clear();
}

bool tfs::net::has_connection_blocked(const Connection::SocketAddress& socket_address)
{
	std::lock_guard<std::recursive_mutex> lock{connections_block_lock};

	uint64_t current_time = OTSYS_TIME();

	auto it = connections_block.find(socket_address);
	if (it == connections_block.end()) {
		connections_block.emplace(socket_address, ConnectionBlock{.last_attempt = current_time});
		return false;
	}

	auto& connection_block = it->second;
	if (connection_block.block_time > current_time) {
		connection_block.block_time += 250;
		return true;
	}

	int64_t time_diff = current_time - connection_block.last_attempt;
	connection_block.last_attempt = current_time;

	if (time_diff <= 5000) {
		if (++connection_block.count > 5) {
			connection_block.count = 0;
			if (time_diff <= 500) {
				connection_block.block_time = current_time + 3000;
				return true;
			}
		}
	} else {
		connection_block.count = 1;
	}
	return false;
}
