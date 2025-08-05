# Mirage RPC

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Language](https://img.shields.io/badge/language-C%2B%2B17-blue.svg)](https://isocpp.org/)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)](https://github.com/your-repo/mirage-rpc)

**Mirage RPC 是一个将 gRPC 和 ZeroMQ 优雅结合的现代化、Header-Only C++ RPC 框架。**

它旨在为复杂的分布式系统提供一个双通道通信解决方案：

*   **gRPC 通道**: 用于结构化的、可靠的、面向服务的请求/响应式通信（控制平面）。
*   **ZeroMQ 通道**: 用于高性能、低延迟的异步消息发布/订阅或推送/拉取（数据平面）。

`Mirage` (海市蜃楼) 之名，寓意着它能将底层复杂的网络通信细节“幻化”为上层简洁易用的接口，让开发者可以聚焦于业务逻辑本身。

## ✨ 核心特性

*   **混合通信模型**: 在一个框架内无缝集成 gRPC 和 ZMQ，统一管理生命周期，简化架构。
*   **现代化 C++ 设计**: 大量使用 C++17 特性，如智能指针、移动语义、原子操作等，确保代码高效、安全。
*   **线程安全**: 内置完整的线程和锁管理，开发者无需关心多线程环境下的同步问题。
*   **Header-Only**: 极易集成，只需包含头文件并链接相关依赖即可，无需预先编译库文件。
*   **易于使用的 API**: 提供清晰的 `Server` 和 `Client` 类，通过简单的配置即可启动和连接。
*   **RAII 资源管理**: 自动管理网络连接、线程和内存资源，在对象析构时实现优雅停机和资源释放。

## ⛓️ 依赖项

要使用本框架，您需要确保您的项目中已包含并链接以下库：

*   [**gRPC**](https://grpc.io/) (& Protobuf)
*   [**ZeroMQ (libzmq)**](https://zeromq.org/)
*   [**cppzmq** (ZeroMQ C++ binding)](https://github.com/zeromq/cppzmq)
*   [**spdlog** (for logging)](https://github.com/gabime/spdlog)
*   **C++17** 兼容的编译器 (GCC 7+, Clang 5+, MSVC 2017+)
*   **CMake** (推荐用于构建和管理依赖)

## 🚀 快速开始

### 1. 定义 gRPC 服务 (`greeter.proto`)

首先，使用 Protobuf IDL 定义您的 gRPC 服务。

```protobuf
syntax = "proto3";

package mirage.example;

// 定义服务
service Greeter {
  // 一个简单的 RPC 方法
  rpc SayHello(HelloRequest) returns (HelloReply) {}
}

// 请求消息
message HelloRequest {
  string name = 1;
}

// 响应消息
message HelloReply {
  string message = 1;
}
```

使用 `protoc` 编译器生成 C++ 服务代码。

### 2. 创建服务器 (`server.cpp`)

服务器将同时启动 gRPC 服务用于接收请求，并启动 ZMQ (PUB) 服务用于发布实时消息。

```cpp
#include "mirage_rpc_server.h"
#include "greeter.grpc.pb.h" // 由 protoc 生成
#include <iostream>
#include <chrono>

// 1. 实现 gRPC 服务
class GreeterServiceImpl final : public mirage::example::Greeter::Service {
    grpc::Status SayHello(grpc::ServerContext* context, const mirage::example::HelloRequest* request,
                          mirage::example::HelloReply* reply) override {
        std::string prefix("你好, ");
        reply->set_message(prefix + request->name());
        spdlog::info("gRPC call: SayHello, name={}", request->name());
        return grpc::Status::OK;
    }
};

int main() {
    GreeterServiceImpl service;

    // 2. 配置服务器
    mirage_rpc_config config;
    config.set_grpc_addr("0.0.0.0", 50051);
    config.set_zmq_tcp_addr("*", 5555); // 使用 PUB/SUB 模式
    config.zmq_socket_type = zmq::socket_type::pub;

    // 3. 启动服务器
    mirage_rpc_server server;
    server.start(config, &service);

    // 4. 通过 ZMQ 定期发布消息
    int counter = 0;
    while (server.is_running()) {
        std::string topic = "SYSTEM_STATUS";
        std::string message = "Server heartbeat: " + std::to_string(counter++);
        
        // ZMQ 消息格式: [主题][消息体]
        server.zmq_send_string(topic); // 发送主题部分
        // 注：真实的多部分消息需要更复杂的封装，这里为简化示例
        server.zmq_send_string(message); // 发送消息部分

        spdlog::info("ZMQ PUB: {}", message);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    std::cout << "按 Enter 键停止服务器...\n";
    std::cin.get();
    server.stop();

    return 0;
}
```

### 3. 创建客户端 (`client.cpp`)

客户端将连接到服务器，可以发起 gRPC 调用，并同时通过 ZMQ 订阅服务器发布的消息。

```cpp
#include "mirage_rpc_client.h"
#include "greeter.grpc.pb.h" // 由 protoc 生成
#include <iostream>

int main() {
    // 1. 配置客户端
    mirage_rpc_client_config config;
    config.set_grpc_addr("127.0.0.1", 50051);
    config.set_zmq_tcp_addr("127.0.0.1", 5555);
    config.zmq_socket_type = zmq::socket_type::sub;
    
    // 设置 ZMQ 消息回调
    config.zmq_message_handler = [](const zmq::message_t& msg) {
        spdlog::info("ZMQ SUB Received: {}", msg.to_string_view());
    };

    // 2. 连接到服务器
    mirage_rpc_client client;
    client.connect(config);

    if (!client.is_connected()) {
        spdlog::error("无法连接到服务器");
        return 1;
    }

    // 3. 订阅所有 ZMQ 主题
    client.subscribe_topic("");

    // 4. 发起 gRPC 调用
    try {
        auto stub = client.create_stub<mirage::example::Greeter::Stub>();
        mirage::example::HelloRequest request;
        request.set_name("Mirage RPC");

        mirage::example::HelloReply reply;
        grpc::ClientContext context;

        grpc::Status status = stub->SayHello(&context, request, &reply);

        if (status.ok()) {
            spdlog::info("gRPC Response: {}", reply.message());
        } else {
            spdlog::error("gRPC call failed: {}", status.error_message());
        }
    } catch (const std::exception& e) {
        spdlog::error("An error occurred: {}", e.what());
    }

    std::cout << "客户端正在运行，接收 ZMQ 消息中... 按 Enter 键退出。\n";
    std::cin.get();
    
    // 5. 断开连接 (RAII 自动处理，也可手动调用)
    client.disconnect();

    return 0;
}

```

## API 概览

### `mirage_rpc_server`

管理服务器的生命周期和通信。

-   `start(config, ...services)`: 启动服务器，并注册一个或多个 gRPC 服务。
-   `stop()`: 优雅地关闭服务器，释放所有资源。
-   `zmq_send(data, size)`: 通过 ZMQ 发送原始二进制数据。
-   `zmq_send_string(message)`: 发送字符串消息。
-   `is_running()`: 检查服务器是否在运行。

### `mirage_rpc_client`

管理客户端的生命周期和通信。

-   `connect(config)`: 根据配置连接到服务器。
-   `disconnect()`: 断开与服务器的连接。
-   `get_grpc_channel()`: 获取底层的 gRPC Channel，用于高级操作。
-   `create_stub<T>()`: 方便地创建指定类型的 gRPC 服务存根。
-   `zmq_send(...)`: 在 PUSH/REQ 模式下通过 ZMQ 发送消息。
-   `subscribe_topic(topic)`: (SUB 模式) 订阅一个 ZMQ 主题。
-   `unsubscribe_topic(topic)`: (SUB 模式) 取消订阅。
-   `is_connected()`: 检查客户端是否已连接。

## 🎨 设计哲学

1.  **分层与解耦**: gRPC 的控制平面和 ZMQ 的数据平面在逻辑上分离，但通过框架统一管理，实现了高内聚、低耦合。
2.  **默认安全**: 通过现代 C++ 的 RAII 和线程安全原语，最大程度地减少资源泄漏和数据竞争的风险。
3.  **开发者体验优先**: API 设计力求直观，配置项清晰明了，复杂的底层操作被完全封装。

## 📜 许可证

本项目采用 [MIT License](LICENSE) 授权。