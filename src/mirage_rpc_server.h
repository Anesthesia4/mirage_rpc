#pragma once
#include <queue>

#include "zmq.hpp"
#include "grpcpp/grpcpp.h"
#include <thread>

struct mirage_rpc_config {
	std::string grpc_addr;
	std::string zmq_addr;
	zmq::socket_type zmq_socket_type = zmq::socket_type::pub;
	std::function<void(const zmq::message_t&)> zmq_message_handler;

	void set_grpc_addr(const std::string& in_ip, int32_t in_port) {
		grpc_addr = in_ip + ":" + std::to_string(in_port);
	}
	void set_zmq_ipc_addr(const std::string& in_name) {
		zmq_addr = "ipc:///tmp/" + in_name + ".sock";
		zmq_socket_type = zmq::socket_type::pub;
	}
	void set_zmq_tcp_addr(const std::string& in_ip, int32_t in_port) {
		zmq_addr = "tcp://" + in_ip + ":" + std::to_string(in_port);
		zmq_socket_type = zmq::socket_type::pub;
	}
};

class mirage_rpc_server {
public:
	~mirage_rpc_server() {
		stop();
	}
	template<typename... Services>
	void start(const mirage_rpc_config& in_config, Services*... in_services) {
		if (running) return; // Already running
		try {
			config = in_config;
			grpc_thread = std::thread(start_grpc, this, in_services...);
			zmq_thread = std::thread(start_zmq, this);
		}
		catch (...) {
		}
	}
	void stop() {
		if (!running) return;

		running = false;
		if (zmq_thread.joinable()) {
			zmq_thread.join();
		}
		if (grpc_thread.joinable()) {
			grpc_thread.join();
		}
		socket.close();
		context.close();
		grpc_server.reset();
	}

	template<typename T>
	void zmq_send(const T& message) {
		zmq::message_t zmq_message(sizeof(T));
		memcpy(zmq_message.data(), &message, sizeof(T));
		zmq_message_queue.push(std::move(zmq_message));
	}
private:
	template<typename... Services>
	void start_grpc(Services*... in_services) {
		// Create a gRPC server builder
		grpc::ServerBuilder builder;

		// Register each service with the server builder
		(builder.RegisterService(in_services), ...);

		// Set the address for the server
		builder.AddListeningPort(config.grpc_addr, grpc::InsecureServerCredentials());
		grpc_server = builder.BuildAndStart();
		grpc_server->Wait();
	}
	void start_zmq() {
		// Initialize ZeroMQ context and socket
		context = zmq::context_t(1);
		socket = zmq::socket_t(context, config.zmq_socket_type);
		socket.bind(config.zmq_addr);
		while (running) {
			// Handle ZeroMQ messages here
			zmq::message_t message;
			socket.recv(message, zmq::recv_flags::none);
			if (config.zmq_message_handler) {
				config.zmq_message_handler(message);
			}
			// Process the message queue
			while (!zmq_message_queue.empty()) {
				zmq::message_t zmq_message = std::move(zmq_message_queue.front());
				zmq_message_queue.pop();
				// Send the message to the ZeroMQ socket
				socket.send(zmq_message, zmq::send_flags::none);
			}
		}
	}

	mirage_rpc_config config;

	std::unique_ptr<grpc::Server> grpc_server;
	zmq::context_t context;
	zmq::socket_t socket;
	std::queue<zmq::message_t> zmq_message_queue;

	std::thread zmq_thread;
	std::thread grpc_thread;
	std::atomic_bool running = true;
};
