#pragma once

#include <memory>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

// 引入第三方库头文件
#include <spdlog/spdlog.h>
#include "zmq.hpp"
#include "grpcpp/grpcpp.h"

/**
 * @file mirage_rpc_client.h
 * @brief 定义了 Mirage RPC 客户端。
 *
 * 该客户端集成了 gRPC 用于同步的请求/响应式通信，
 * 以及 ZeroMQ 用于高性能的异步消息订阅或推送。它被设计为线程安全，
 * 并通过一个统一的接口管理两种通信模式的生命周期。
 */

// --- 配置结构体 (Configuration Struct) ---

/**
 * @brief Mirage RPC 客户端的配置结构体。
 *
 * 用于初始化和配置客户端的所有参数，包括 gRPC 和 ZMQ 的连接地址、
 * 超时设置、消息大小限制等。
 */
struct mirage_rpc_client_config {
    // --- 核心地址配置 ---
    std::string grpc_addr; ///< gRPC 服务器地址，格式为 "ip:port"。
    std::string zmq_addr;  ///< ZMQ 服务器地址，格式可以为 "tcp://ip:port" 或 "ipc:///path/to/socket"。

    // --- ZMQ 特定配置 ---
    zmq::socket_type zmq_socket_type = zmq::socket_type::sub; ///< ZMQ socket 类型，默认为 SUB (订阅)。
    std::function<void(const zmq::message_t&)> zmq_message_handler; ///< ZMQ 消息回调函数。
    int zmq_io_threads = 1;         ///< ZMQ I/O 线程数。
    int zmq_linger_ms = 1000;       ///< socket 关闭前的等待时间(毫秒)，确保挂起的消息已发送。
    int zmq_rcv_timeout_ms = 1000;  ///< ZMQ 接收操作的超时时间(毫秒)。

    // --- gRPC 特定配置 ---
    size_t grpc_max_receive_message_size = 1024 * 1024 * 4; ///< gRPC 允许接收的最大消息大小 (默认 4MB)。
    size_t grpc_max_send_message_size = 1024 * 1024 * 4;    ///< gRPC 允许发送的最大消息大小 (默认 4MB)。
    int grpc_timeout_ms = 30000; ///< gRPC 连接超时时间 (默认 30秒)。

    // --- 便捷设置函数 (Convenience Setters) ---

    /**
     * @brief 设置 gRPC 的 TCP 地址。
     * @param ip IP 地址。
     * @param port 端口号。
     */
    void set_grpc_addr(const std::string& ip, int32_t port) {
        if (ip.empty() || port <= 0 || port > 65535) {
            throw std::invalid_argument("无效的 gRPC IP 地址或端口号");
        }
        grpc_addr = ip + ":" + std::to_string(port);
    }

    /**
     * @brief 设置 ZMQ 的 IPC (进程间通信) 地址。
     * @param name IPC socket 的唯一名称。
     */
    void set_zmq_ipc_addr(const std::string& name) {
        if (name.empty()) {
            throw std::invalid_argument("IPC 名称不能为空");
        }
        zmq_addr = "ipc:///tmp/" + name + ".sock";
    }

    /**
     * @brief 设置 ZMQ 的 TCP 地址。
     * @param ip IP 地址。
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
 * @class mirage_rpc_client
 * @brief 一个功能完备的 RPC 客户端类。
 *
 * 该类封装了 gRPC 和 ZMQ 的连接管理、消息收发和生命周期控制。
 * 设计上遵循 RAII (资源获取即初始化) 原则，在析构时自动断开连接并清理资源。
 * 禁止拷贝，但支持移动语义，以实现高效的资源所有权转移。
 */
class mirage_rpc_client {
public:
    // --- 构造函数与析构函数 (Constructors & Destructor) ---

    mirage_rpc_client() = default;
    ~mirage_rpc_client() {
        disconnect();
    }

    // --- 资源管理：禁止拷贝，允许移动 ---
    mirage_rpc_client(const mirage_rpc_client&) = delete;
    mirage_rpc_client& operator=(const mirage_rpc_client&) = delete;

    mirage_rpc_client(mirage_rpc_client&& other) noexcept {
        *this = std::move(other);
    }

    mirage_rpc_client& operator=(mirage_rpc_client&& other) noexcept {
        if (this != &other) {
            disconnect(); // 先释放当前对象的资源
            config_ = std::move(other.config_);
            // 其他成员（如 channel, socket）在 `connect` 时创建，无需移动
        }
        return *this;
    }

    // --- 生命周期管理 (Lifecycle Management) ---

    /**
     * @brief 连接到 RPC 服务器。
     * @details 根据提供的配置，初始化并连接 gRPC 和 ZMQ。会启动一个后台线程处理 ZMQ 消息。
     * @param config 客户端配置对象。
     * @throws std::runtime_error 如果连接失败或配置无效。
     * @example
     *   mirage_rpc_client_config cfg;
     *   cfg.set_grpc_addr("127.0.0.1", 50051);
     *   cfg.set_zmq_tcp_addr("127.0.0.1", 5555);
     *   cfg.zmq_message_handler = [](const zmq::message_t& msg) {
     *       spdlog::info("Received: {}", msg.to_string());
     *   };
     *   mirage_rpc_client client;
     *   client.connect(cfg);
     */
    void connect(const mirage_rpc_client_config& config) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (connected_.load()) {
            spdlog::warn("客户端已经连接，无需重复操作");
            return;
        }

        try {
            config_ = config;
            validate_config();

            // 1. 建立 gRPC 连接
            setup_grpc_channel();

            // 2. 启动 ZMQ 后台线程
            zmq_thread_ = std::thread(&mirage_rpc_client::start_zmq, this);

            connected_.store(true);
            spdlog::info("RPC 客户端连接成功 - gRPC: {}, ZMQ: {}", config_.grpc_addr, config_.zmq_addr);

        } catch (const std::exception& e) {
            spdlog::error("连接服务器失败: {}", e.what());
            cleanup_resources(); // 出错时清理已分配的资源
            throw;
        }
    }

    /**
     * @brief 断开与 RPC 服务器的连接。
     * @details 这是一个优雅停机过程。它会停止 ZMQ 线程，关闭所有 sockets 和 channels，并清理资源。
     */
    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!connected_.load()) {
            return;
        }

        spdlog::info("正在断开 RPC 客户端连接...");
        connected_.store(false); // 通知 ZMQ 线程退出循环

        if (zmq_thread_.joinable()) {
            zmq_thread_.join(); // 等待 ZMQ 线程完全结束
        }

        cleanup_resources();
        spdlog::info("RPC 客户端已断开连接");
    }

    // --- gRPC 相关接口 (gRPC Interface) ---

    /**
     * @brief 获取 gRPC 的通信 Channel。
     * @details Channel 是与 gRPC 服务器建立连接的抽象，存根(Stub)需要通过它来创建。
     * @returns 指向 grpc::Channel 的共享指针。
     * @throws std::runtime_error 如果客户端未连接。
     */
    std::shared_ptr<grpc::Channel> get_grpc_channel() const {
        if (!connected_.load()) {
            throw std::runtime_error("客户端未连接，无法获取 gRPC Channel");
        }
        return grpc_channel_;
    }

    /**
     * @brief 创建一个 gRPC 服务的存根 (Stub)。
     * @details 这是一个模板函数，可以方便地创建任何类型的 gRPC 服务存根。
     * @tparam StubType gRPC 生成的服务存根类型 (例如 `YourService::Stub`)。
     * @returns 一个封装了服务存根的唯一指针。
     * @example
     *   auto stub = client.create_stub<Greeter::Stub>();
     *   grpc::ClientContext context;
     *   HelloRequest request;
     *   HelloReply reply;
     *   stub->SayHello(&context, request, &reply);
     */
    template <typename StubType>
    std::unique_ptr<StubType> create_stub() const {
        return StubType::NewStub(get_grpc_channel());
    }

    // --- ZMQ 相关接口 (ZMQ Interface) ---

    /**
     * @brief 发送 ZMQ 消息（仅限可发送的 socket 类型）。
     * @details 支持的 socket 类型包括 PUB, PUSH, REQ。
     * @param data 指向待发送数据的指针。
     * @param size 数据的大小（字节）。
     * @throws std::runtime_error 如果客户端未连接或 socket 类型不支持发送。
     * @throws std::invalid_argument 如果 data 为空或 size 为 0。
     */
    void zmq_send(const void* data, size_t size) {
        if (!data || size == 0) {
            throw std::invalid_argument("无效的消息数据");
        }
        if (!connected_.load()) {
            throw std::runtime_error("客户端未连接，无法发送 ZMQ 消息");
        }

        // 检查 socket 类型是否支持发送操作
        if (config_.zmq_socket_type != zmq::socket_type::pub &&
            config_.zmq_socket_type != zmq::socket_type::push &&
            config_.zmq_socket_type != zmq::socket_type::req) {
            throw std::runtime_error("当前的 ZMQ socket 类型不支持发送消息");
        }

        try {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            if (socket_) {
                zmq::message_t message(size);
                std::memcpy(message.data(), data, size);
                socket_->send(message, zmq::send_flags::none);
            }
        } catch (const zmq::error_t& e) {
            spdlog::error("发送 ZMQ 消息失败: {}", e.what());
            throw std::runtime_error("发送 ZMQ 消息失败: " + std::string(e.what()));
        }
    }

    /**
     * @brief 发送字符串作为 ZMQ 消息。
     * @param message 要发送的字符串。
     */
    void zmq_send_string(const std::string& message) {
        zmq_send(message.data(), message.size());
    }

    /**
     * @brief 订阅 ZMQ 主题 (仅适用于 SUB socket)。
     * @param topic 要订阅的主题。空字符串 "" 表示订阅所有主题。
     * @throws std::runtime_error 如果 socket 类型不是 SUB。
     */
    void subscribe_topic(const std::string& topic) {
        if (config_.zmq_socket_type != zmq::socket_type::sub) {
            throw std::runtime_error("只有 SUB socket 支持订阅主题");
        }
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (socket_) {
            try {
                socket_->set(zmq::sockopt::subscribe, topic);
                spdlog::info("已订阅 ZMQ 主题: {}", topic.empty() ? "(所有)" : topic);
            } catch (const zmq::error_t& e) {
                spdlog::error("订阅 ZMQ 主题 '{}' 失败: {}", topic, e.what());
                throw;
            }
        }
    }

    /**
     * @brief 取消订阅 ZMQ 主题 (仅适用于 SUB socket)。
     * @param topic 要取消订阅的主题。
     * @throws std::runtime_error 如果 socket 类型不是 SUB。
     */
    void unsubscribe_topic(const std::string& topic) {
        if (config_.zmq_socket_type != zmq::socket_type::sub) {
            throw std::runtime_error("只有 SUB socket 支持取消订阅主题");
        }
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (socket_) {
            try {
                socket_->set(zmq::sockopt::unsubscribe, topic);
                spdlog::info("已取消订阅 ZMQ 主题: {}", topic);
            } catch (const zmq::error_t& e) {
                spdlog::error("取消订阅 ZMQ 主题 '{}' 失败: {}", topic, e.what());
                throw;
            }
        }
    }

    // --- 状态检查 (State Checkers) ---

    /**
     * @brief 检查客户端是否已连接。
     * @returns 如果客户端已连接，则返回 true；否则返回 false。
     */
    bool is_connected() const {
        return connected_.load();
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

    /** @brief 初始化并建立 gRPC 连接。*/
    void setup_grpc_channel() {
        grpc::ChannelArguments args;
        args.SetMaxReceiveMessageSize(config_.grpc_max_receive_message_size);
        args.SetMaxSendMessageSize(config_.grpc_max_send_message_size);

        grpc_channel_ = grpc::CreateCustomChannel(
            config_.grpc_addr,
            grpc::InsecureChannelCredentials(), // 当前使用不安全的连接
            args
        );

        if (!grpc_channel_) {
            throw std::runtime_error("无法创建 gRPC channel");
        }

        // 等待连接建立，并设置超时
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(config_.grpc_timeout_ms);
        if (!grpc_channel_->WaitForConnected(deadline)) {
            throw std::runtime_error("gRPC 连接超时");
        }
        spdlog::info("gRPC channel 建立成功");
    }

    /**
     * @brief ZMQ 后台线程的执行函数。
     * @details 负责初始化 ZMQ socket，并在一个循环中接收消息，直到客户端断开连接。
     * // TODO: 这个函数的核心循环逻辑较长，可以考虑将消息接收部分拆分为一个独立的私有方法，
     * // 例如 `handle_zmq_reception()`，以提高清晰度。
     */
    void start_zmq() {
        try {
            // 1. 初始化 ZMQ 上下文和 socket
            context_ = std::make_unique<zmq::context_t>(config_.zmq_io_threads);

            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                socket_ = std::make_unique<zmq::socket_t>(*context_, config_.zmq_socket_type);

                // 设置 socket 选项
                socket_->set(zmq::sockopt::linger, config_.zmq_linger_ms);
                socket_->set(zmq::sockopt::rcvtimeo, config_.zmq_rcv_timeout_ms);

                // 连接到服务器
                socket_->connect(config_.zmq_addr);
            }
            spdlog::info("ZMQ socket 连接成功，地址: {}", config_.zmq_addr);

            // 2. 主循环 - 接收消息（仅限可接收的 socket 类型）
            while (connected_.load()) {
                if (config_.zmq_socket_type == zmq::socket_type::sub ||
                    config_.zmq_socket_type == zmq::socket_type::pull ||
                    config_.zmq_socket_type == zmq::socket_type::rep) {

                    zmq::message_t message;
                    std::lock_guard<std::mutex> lock(socket_mutex_);

                    if (socket_) {
                        // 使用非阻塞方式接收消息
                        auto result = socket_->recv(message, zmq::recv_flags::dontwait);
                        if (result && result.value() > 0 && config_.zmq_message_handler) {
                            config_.zmq_message_handler(message);
                        }
                    }
                }
                // 短暂休眠以避免 CPU 占用过高
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } catch (const zmq::error_t& e) {
            spdlog::error("ZMQ 错误: {}", e.what());
            connected_.store(false); // 发生严重错误时，标记为未连接
        } catch (const std::exception& e) {
            spdlog::error("ZMQ 线程发生未知异常: {}", e.what());
            connected_.store(false);
        }
    }

    /** @brief 清理所有分配的资源，如 sockets 和 channels。 */
    void cleanup_resources() {
        try {
            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                if (socket_) {
                    socket_->close();
                    socket_.reset();
                }
            }
            if (context_) {
                context_->close();
                context_.reset();
            }
            if (grpc_channel_) {
                // gRPC channel 是 shared_ptr，会被自动管理，但 reset 可提前释放
                grpc_channel_.reset();
            }
        } catch (const std::exception& e) {
            spdlog::error("清理资源时发生错误: {}", e.what());
        }
    }

private:
    // --- 成员变量 (Member Variables) ---

    // 配置
    mirage_rpc_client_config config_;

    // gRPC 相关
    std::shared_ptr<grpc::Channel> grpc_channel_;

    // ZMQ 相关
    std::unique_ptr<zmq::context_t> context_;
    std::unique_ptr<zmq::socket_t> socket_;

    // 线程管理
    std::thread zmq_thread_;
    std::atomic<bool> connected_{false};

    // 同步原语
    mutable std::mutex mutex_;        ///< 保护客户端生命周期和配置的互斥锁。
    mutable std::mutex socket_mutex_; ///< 保护 ZMQ socket 访问的互斥锁。
};