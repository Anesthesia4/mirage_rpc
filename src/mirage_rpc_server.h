#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>
#include <stdexcept>
#include <thread>

// 引入第三方库头文件
#include <spdlog/spdlog.h>
#include "zmq.hpp"
#include "grpcpp/grpcpp.h"

/**
 * @file mirage_rpc_server.h
 * @brief 定义了 Mirage RPC 服务器。
 *
 * 该服务器集成了 gRPC 用于提供同步的 RPC 服务，
 * 并利用 ZeroMQ 进行高性能的异步消息发布或处理。它被设计为线程安全，
 * 能够在一个实例中同时管理 gRPC 和 ZMQ 的服务。
 */

// --- 配置结构体 (Configuration Struct) ---

/**
 * @brief Mirage RPC 服务器的配置结构体。
 *
 * 用于初始化和配置服务器的所有参数，包括 gRPC 和 ZMQ 的监听地址、
 * 消息大小限制、ZMQ 高水位线等。
 */
struct mirage_rpc_config {
    // --- 核心地址配置 ---
    std::string grpc_addr; ///< gRPC 服务器监听地址，格式为 "ip:port"。
    std::string zmq_addr;  ///< ZMQ 服务器监听地址，格式可以为 "tcp://ip:port" 或 "ipc:///path/to/socket"。

    // --- ZMQ 特定配置 ---
    zmq::socket_type zmq_socket_type = zmq::socket_type::pub; ///< ZMQ socket 类型，默认为 PUB (发布)。
    std::function<void(const zmq::message_t&)> zmq_message_handler; ///< ZMQ 消息回调函数 (用于 SUB/PULL/REP 类型)。
    int zmq_io_threads = 1;      ///< ZMQ I/O 线程数。
    int zmq_linger_ms = 0;       ///< socket 关闭前的等待时间(毫秒)，服务器端通常设为 0。
    int zmq_hwm = 1000;          ///< ZMQ 高水位线 (High Water Mark)，用于防止消息队列无限增长。

    // --- gRPC 特定配置 ---
    size_t grpc_max_receive_message_size = 1024 * 1024 * 4; ///< gRPC 允许接收的最大消息大小 (默认 4MB)。
    size_t grpc_max_send_message_size = 1024 * 1024 * 4;    ///< gRPC 允许发送的最大消息大小 (默认 4MB)。

    // --- 便捷设置函数 (Convenience Setters) ---

    /**
     * @brief 设置 gRPC 的 TCP 监听地址。
     * @param ip IP 地址，如 "0.0.0.0" 表示监听所有网络接口。
     * @param port 端口号。
     */
    void set_grpc_addr(const std::string& ip, int32_t port) {
        if (ip.empty() || port <= 0 || port > 65535) {
            throw std::invalid_argument("无效的 gRPC IP 地址或端口号");
        }
        grpc_addr = ip + ":" + std::to_string(port);
    }

    /**
     * @brief 设置 ZMQ 的 IPC (进程间通信) 监听地址。
     * @param name IPC socket 的唯一名称。
     */
    void set_zmq_ipc_addr(const std::string& name) {
        if (name.empty()) {
            throw std::invalid_argument("IPC 名称不能为空");
        }
        zmq_addr = "ipc:///tmp/" + name + ".sock";
    }

    /**
     * @brief 设置 ZMQ 的 TCP 监听地址。
     * @param ip IP 地址，如 "*" 表示监听所有网络接口。
     * @param port 端口号。
     */
    void set_zmq_tcp_addr(const std::string& ip, int32_t port) {
        if (ip.empty() || port <= 0 || port > 65535) {
            throw std::invalid_argument("无效的 ZMQ IP 地址或端口号");
        }
        zmq_addr = "tcp://" + ip + ":" + std::to_string(port);
    }
};

/**
 * @class mirage_rpc_server
 * @brief 一个功能完备的 RPC 服务器类。
 *
 * 该类封装了 gRPC 服务的注册和启动，以及 ZMQ 的消息发布和接收。
 * 它使用独立的线程分别处理 gRPC 和 ZMQ 的事件循环，并通过一个线程安全的消息队列
 * 来处理 ZMQ 的出站消息。
 * 设计上遵循 RAII 原则，禁止拷贝，支持移动。
 */
class mirage_rpc_server {
public:
    // --- 构造函数与析构函数 (Constructors & Destructor) ---

    mirage_rpc_server() = default;
    ~mirage_rpc_server() {
        stop();
    }

    // --- 资源管理：禁止拷贝，允许移动 ---
    mirage_rpc_server(const mirage_rpc_server&) = delete;
    mirage_rpc_server& operator=(const mirage_rpc_server&) = delete;

    mirage_rpc_server(mirage_rpc_server&& other) noexcept {
        *this = std::move(other);
    }

    mirage_rpc_server& operator=(mirage_rpc_server&& other) noexcept {
        if (this != &other) {
            stop(); // 先停止并清理当前服务器
            config_ = std::move(other.config_);
            // 其他成员在 stop 后会被重置，无需移动
        }
        return *this;
    }

    // --- 生命周期管理 (Lifecycle Management) ---

    /**
     * @brief 启动 RPC 服务器。
     * @details 根据配置启动 gRPC 和 ZMQ 服务。gRPC 服务会在一个专用线程中运行，
     * ZMQ 的消息处理也在另一个专用线程中进行。
     * @tparam Services 可变参数模板，接受一个或多个 gRPC 服务实例的指针。
     * @param config 服务器配置对象。
     * @param services 指向 gRPC 服务实例的指针列表。
     * @throws std::runtime_error 如果服务器启动失败。
     * @example
     *   MyGreeterService service;
     *   mirage_rpc_config cfg;
     *   cfg.set_grpc_addr("0.0.0.0", 50051);
     *   cfg.set_zmq_tcp_addr("*", 5555);
     *
     *   mirage_rpc_server server;
     *   server.start(cfg, &service);
     *   // ... 服务器正在运行 ...
     *   server.stop();
     */
    template <typename... Services>
    void start(const mirage_rpc_config& config, Services*... services) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (running_.load()) {
            spdlog::warn("RPC 服务器已在运行中");
            return;
        }

        try {
            config_ = config;
            validate_config();

            // 启动 gRPC 和 ZMQ 的后台线程
            grpc_thread_ = std::thread(&mirage_rpc_server::start_grpc<Services...>, this, services...);
            zmq_thread_ = std::thread(&mirage_rpc_server::start_zmq, this);

            running_.store(true);
            spdlog::info("RPC 服务器启动成功 - gRPC: {}, ZMQ: {}", config_.grpc_addr, config_.zmq_addr);

        } catch (const std::exception& e) {
            spdlog::error("启动服务器失败: {}", e.what());
            cleanup_resources();
            throw;
        }
    }

    /**
     * @brief 停止 RPC 服务器。
     * @details 这是一个优雅停机过程。它会关闭 gRPC 服务器，停止 ZMQ 线程，并清理所有资源。
     */
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_.load()) {
            return;
        }

        spdlog::info("正在停止 RPC 服务器...");
        running_.store(false);

        // 唤醒可能在等待的 ZMQ 发送线程
        cv_.notify_all();

        // 优雅地关闭 gRPC 服务器，这将使 `grpc_server_->Wait()` 返回
        if (grpc_server_) {
            grpc_server_->Shutdown();
        }

        // 等待两个后台线程完全结束
        if (grpc_thread_.joinable()) {
            grpc_thread_.join();
        }
        if (zmq_thread_.joinable()) {
            zmq_thread_.join();
        }

        cleanup_resources();
        spdlog::info("RPC 服务器已停止");
    }

    // --- ZMQ 相关接口 (ZMQ Interface) ---

    /**
     * @brief 将 ZMQ 消息放入发送队列。
     * @details 这是一个非阻塞方法。消息会被放入内部队列，由 ZMQ 线程负责异步发送。
     * 适用于 PUB, PUSH, REP 等 socket 类型。
     * @param data 指向待发送数据的指针。
     * @param size 数据的大小（字节）。
     * @throws std::runtime_error 如果服务器未运行。
     * @throws std::invalid_argument 如果 data 为空或 size 为 0。
     */
    void zmq_send(const void* data, size_t size) {
        if (!data || size == 0) {
            throw std::invalid_argument("无效的消息数据");
        }
        if (!running_.load()) {
            throw std::runtime_error("服务器未运行，无法发送 ZMQ 消息");
        }

        try {
            zmq::message_t message(size);
            std::memcpy(message.data(), data, size);

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                zmq_message_queue_.push(std::move(message));
            }
            cv_.notify_one(); // 唤醒 ZMQ 线程来处理队列
        } catch (const std::exception& e) {
            spdlog::error("准备 ZMQ 消息时失败: {}", e.what());
            throw;
        }
    }

    /**
     * @brief 发送一个可平凡拷贝 (trivially copyable) 的对象。
     * @tparam T 对象的类型，必须是可平凡拷贝的。
     * @param message 要发送的对象实例。
     */
    template <typename T>
    void zmq_send_serializable(const T& message) {
        static_assert(std::is_trivially_copyable_v<T>, "消息类型必须是可平凡拷贝的 (trivially copyable)");
        zmq_send(&message, sizeof(T));
    }

    /**
     * @brief 发送字符串作为 ZMQ 消息。
     * @param message 要发送的字符串。
     */
    void zmq_send_string(const std::string& message) {
        zmq_send(message.data(), message.size());
    }

    // --- 状态检查 (State Checkers) ---

    /**
     * @brief 检查服务器是否正在运行。
     * @returns 如果服务器正在运行，则返回 true；否则返回 false。
     */
    bool is_running() const {
        return running_.load();
    }

private:
    // --- 私有辅助函数 (Private Helper Functions) ---

    /** @brief 验证配置的有效性。*/
    void validate_config() {
        if (config_.grpc_addr.empty()) {
            throw std::invalid_argument("gRPC 地址不能为空");
        }
        if (config_.zmq_addr.empty()) {
            throw std::invalid_argument("ZMQ 地址不能为空");
        }
    }

    /**
     * @brief gRPC 后台线程的执行函数。
     * @details 负责构建、启动并等待 gRPC 服务器终止。
     */
    template <typename... Services>
    void start_grpc(Services*... services) {
        try {
            grpc::ServerBuilder builder;

            // 注册所有传入的服务
            (builder.RegisterService(services), ...);

            // 设置服务器选项
            builder.AddListeningPort(config_.grpc_addr, grpc::InsecureServerCredentials());
            builder.SetMaxReceiveMessageSize(config_.grpc_max_receive_message_size);
            builder.SetMaxSendMessageSize(config_.grpc_max_send_message_size);

            grpc_server_ = builder.BuildAndStart();
            if (!grpc_server_) {
                throw std::runtime_error("无法启动 gRPC 服务器");
            }
            spdlog::info("gRPC 服务器已在线程中启动，监听地址: {}", config_.grpc_addr);

            // 阻塞等待，直到 `Shutdown()` 被调用
            grpc_server_->Wait();
            spdlog::info("gRPC 服务器线程已停止");

        } catch (const std::exception& e) {
            spdlog::error("gRPC 服务器线程发生异常: {}", e.what());
            running_.store(false);
        }
    }

    /**
     * @brief ZMQ 后台线程的执行函数。
     * @details 负责初始化 ZMQ socket，并在一个循环中处理消息的接收和发送，直到服务器停止。
     */
    void start_zmq() {
        try {
            context_ = std::make_unique<zmq::context_t>(config_.zmq_io_threads);
            socket_ = std::make_unique<zmq::socket_t>(*context_, config_.zmq_socket_type);

            socket_->set(zmq::sockopt::linger, config_.zmq_linger_ms);
            socket_->set(zmq::sockopt::sndhwm, config_.zmq_hwm);
            socket_->set(zmq::sockopt::rcvhwm, config_.zmq_hwm);

            socket_->bind(config_.zmq_addr);
            spdlog::info("ZMQ socket 绑定成功，地址: {}", config_.zmq_addr);

            // 主循环
            while (running_.load()) {
                // 1. 处理接收消息 (仅适用于可接收的 socket 类型)
                if (config_.zmq_socket_type == zmq::socket_type::sub ||
                    config_.zmq_socket_type == zmq::socket_type::pull ||
                    config_.zmq_socket_type == zmq::socket_type::rep) {

                    zmq::message_t message;
                    auto result = socket_->recv(message, zmq::recv_flags::dontwait);
                    if (result && result.value() > 0 && config_.zmq_message_handler) {
                        config_.zmq_message_handler(message);
                    }
                }

                // 2. 处理待发送的消息队列
                process_send_queue();

                // 短暂休眠，避免 CPU 占用过高
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            spdlog::info("ZMQ 服务器线程已停止");
        } catch (const zmq::error_t& e) {
            spdlog::error("ZMQ 错误: {}", e.what());
            running_.store(false);
        } catch (const std::exception& e) {
            spdlog::error("ZMQ 线程发生未知异常: {}", e.what());
            running_.store(false);
        }
    }

    /** @brief 处理并发送消息队列中的所有消息。*/
    void process_send_queue() {
        std::unique_lock<std::mutex> lock(queue_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return; // 如果其他地方正在操作队列，则本次跳过
        }

        while (!zmq_message_queue_.empty() && running_.load()) {
            try {
                zmq::message_t message = std::move(zmq_message_queue_.front());
                zmq_message_queue_.pop();

                // 释放锁后发送，避免阻塞其他线程向队列中添加消息
                lock.unlock();
                socket_->send(message, zmq::send_flags::dontwait);
                lock.lock(); // 重新获取锁以检查循环条件

            } catch (const zmq::error_t& e) {
                // EAGAIN 是一个正常的非阻塞错误，表示发送缓冲区已满
                if (e.num() != EAGAIN) {
                    spdlog::error("发送 ZMQ 消息失败: {}", e.what());
                }
                // 无论何种错误，都中断本次发送循环，等待下次机会
                break;
            }
        }
    }

    /** @brief 清理所有分配的资源，如 sockets 和 server 实例。 */
    void cleanup_resources() {
        try {
            if (socket_) {
                socket_->close();
                socket_.reset();
            }
            if (context_) {
                context_->close();
                context_.reset();
            }
            grpc_server_.reset(); // unique_ptr 会自动处理

            // 清空可能残留的消息队列
            std::lock_guard<std::mutex> lock(queue_mutex_);
            std::queue<zmq::message_t> empty_queue;
            zmq_message_queue_.swap(empty_queue);

        } catch (const std::exception& e) {
            spdlog::error("清理资源时发生错误: {}", e.what());
        }
    }

private:
    // --- 成员变量 (Member Variables) ---

    // 配置
    mirage_rpc_config config_;

    // gRPC 相关
    std::unique_ptr<grpc::Server> grpc_server_;

    // ZMQ 相关
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;
    std::queue<zmq::message_t> zmq_message_queue_;

    // 线程管理
    std::thread zmq_thread_;
    std::thread grpc_thread_;
    std::atomic<bool> running_{false};

    // 同步原语
    mutable std::mutex mutex_;         ///< 保护服务器生命周期和配置的互斥锁。
    mutable std::mutex queue_mutex_;   ///< 保护 ZMQ 消息队列的互斥锁。
    std::condition_variable cv_;       ///< 用于唤醒 ZMQ 发送线程的条件变量。
};