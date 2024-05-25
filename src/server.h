// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_SERVER_H
#define FS_SERVER_H

#include "connection.h"
#include "signals.h"

class ServiceBase
{
public:
	virtual ~ServiceBase() = default;

	virtual bool is_single_socket() const = 0;
	virtual bool is_checksummed() const = 0;
	virtual uint8_t get_protocol_identifier() const = 0;
	virtual const char* get_protocol_name() const = 0;

	virtual Protocol_ptr make_protocol(const Connection_ptr& c) const = 0;
};

template <typename ProtocolType>
class Service final : public ServiceBase
{
public:
	bool is_single_socket() const override { return ProtocolType::server_sends_first; }
	bool is_checksummed() const override { return ProtocolType::use_checksum; }
	uint8_t get_protocol_identifier() const override { return ProtocolType::protocol_identifier; }
	const char* get_protocol_name() const override { return ProtocolType::protocol_name(); }

	Protocol_ptr make_protocol(const Connection_ptr& c) const override { return std::make_shared<ProtocolType>(c); }
};

class ServicePort : public std::enable_shared_from_this<ServicePort>
{
public:
	explicit ServicePort(boost::asio::io_context& io_context) : io_context(io_context) {}
	~ServicePort();

	// non-copyable
	ServicePort(const ServicePort&) = delete;
	ServicePort& operator=(const ServicePort&) = delete;

	void open(uint16_t port);
	void close();
	bool is_single_socket() const;
	std::string get_protocol_names() const;

	bool add_service(const Service_ptr& new_svc);
	Protocol_ptr make_protocol(NetworkMessage& msg, const Connection_ptr& connection) const;

	void onStopServer();
	void onAccept(Connection_ptr connection, const boost::system::error_code& error);

private:
	void accept();

	boost::asio::io_context& io_context;
	std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor;
	std::vector<Service_ptr> services;

	uint16_t serverPort = 0;
	bool pendingStart = false;
};

namespace tfs::io::services {

bool start();
void shutdown();

template <typename ProtocolType>
bool add(uint16_t port);

} // namespace tfs::service

#endif // FS_SERVER_H
