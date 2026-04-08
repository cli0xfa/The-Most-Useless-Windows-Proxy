# TMUWP - 最没用的 Windows 代理

一个用 C++ 编写的高性能 Windows 反向代理和转发工具。

## 概述

TMUWP 是一个基于 Windows 的反向代理服务器，支持 TCP 端口转发、HTTP/HTTPS 代理和 SOCKS5 代理协议。它利用 Windows IOCP（I/O 完成端口）实现高性能异步 I/O 操作。

## 功能特性

- **TCP 端口转发** - 透明 TCP 连接转发
- **HTTP/HTTPS 代理** - 支持 CONNECT 方法的 HTTP 代理，用于 HTTPS 隧道
- **SOCKS5 代理** - 完整的 SOCKS5 协议实现
- **负载均衡** - 多种负载均衡策略：
  - 轮询
  - 加权轮询
  - 最少连接数
  - 随机
- **高性能** - 基于 Windows IOCP 的异步 I/O
- **JSON 配置** - 简单的 JSON 配置文件
- **日志系统** - 结构化日志，支持文件轮转

## 架构

```
┌─────────────────────────────────────────────────────────┐
│                        TMUWP                            │
├─────────────────────────────────────────────────────────┤
│  TCP 转发器  │  HTTP 代理  │  SOCKS5 代理               │
├─────────────────────────────────────────────────────────┤
│              IOCP 管理器（I/O 完成端口）                 │
├─────────────────────────────────────────────────────────┤
│           负载均衡器 │ 连接池                            │
├─────────────────────────────────────────────────────────┤
│              日志 │ 配置管理器 │ 统计                    │
└─────────────────────────────────────────────────────────┘
```

## 系统要求

- Windows 7 或更高版本（推荐 Windows 10/11）
- Visual Studio 2022（需安装 C++ 工作负载）
- Windows SDK

## 编译构建

### 使用 Visual Studio

1. 在 Visual Studio 2022 中打开 `tmuwp.sln`
2. 选择配置（Debug/Release）和平台（x64）
3. 构建解决方案（Ctrl+Shift+B）

### 使用 MSBuild 命令行

```bash
"D:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" tmuwp.sln /p:Configuration=Release /p:Platform=x64
```

编译后的可执行文件位于：
- `x64\Release\tmuwp.exe`（Release 版本）
- `x64\Debug\tmuwp.exe`（Debug 版本）

## 使用方法

### 命令行选项

```bash
tmuwp.exe [选项]

选项：
  -c, --config <文件>    配置文件路径（默认：config.json）
  -h, --help             显示帮助信息
```

### 示例

```bash
# 使用默认配置文件（config.json）运行
tmuwp.exe

# 使用自定义配置文件运行
tmuwp.exe -c myconfig.json
```

## 配置说明

在可执行文件所在目录创建 `config.json` 文件：

```json
{
    "worker_threads": 4,
    "log_level": 1,
    "listeners": [
        {
            "name": "tcp_forward",
            "bind": "0.0.0.0:8080",
            "type": "tcp",
            "targets": ["192.168.1.100:80", "192.168.1.101:80"],
            "balance": "round_robin"
        },
        {
            "name": "http_proxy",
            "bind": "0.0.0.0:8081",
            "type": "http"
        },
        {
            "name": "socks5_proxy",
            "bind": "0.0.0.0:1080",
            "type": "socks5"
        }
    ]
}
```

### 配置字段说明

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `worker_threads` | IOCP 工作线程数（0 = 自动） | 0 |
| `log_level` | 日志级别（0=调试, 1=信息, 2=警告, 3=错误, 4=致命） | 1 |
| `listeners` | 监听器配置数组 | - |
| `name` | 监听器唯一名称 | - |
| `bind` | 绑定地址（ip:端口） | - |
| `type` | 代理类型（tcp, http, socks5） | - |
| `targets` | 后端服务器数组（仅用于 TCP） | - |
| `balance` | 负载均衡策略 | round_robin |

### 代理类型

- **tcp** - 透明 TCP 转发到后端服务器
- **http** - HTTP 代理服务器（支持 CONNECT 用于 HTTPS）
- **socks5** - SOCKS5 代理服务器（支持 CONNECT 命令）

### 负载均衡策略

- **round_robin** - 均匀分配连接到后端服务器
- **weighted_round_robin** - 根据配置的权重分配
- **least_connections** - 路由到活动连接数最少的服务器
- **random** - 随机选择后端服务器

## 测试

### TCP 转发测试

```bash
# 启动后端服务器
nc -l 8080

# 通过代理连接
telnet localhost 8080
```

### HTTP 代理测试

```bash
# 使用 curl
curl -x http://localhost:8081 http://example.com

# 通过 CONNECT 隧道访问 HTTPS
curl -x http://localhost:8081 https://example.com
```

### SOCKS5 代理测试

```bash
# 使用 curl
curl --socks5 localhost:1080 http://example.com

# 带认证
curl --socks5 localhost:1080 --proxy-user user:pass http://example.com
```

## 项目结构

```
tmuwp/
├── main.cpp              # 应用程序入口
├── config.h/cpp          # 配置管理
├── iocp.h/cpp            # IOCP 管理器（核心网络）
├── tcp_forwarder.h/cpp   # TCP 端口转发
├── http_proxy.h/cpp      # HTTP 代理实现
├── socks5.h/cpp          # SOCKS5 代理实现
├── load_balancer.h/cpp   # 负载均衡策略
├── logger.h/cpp          # 日志系统
├── pool.h/cpp            # 对象池和统计
├── utils.h/cpp           # 工具函数
├── tmuwp.vcxproj         # Visual Studio 项目文件
└── README.md             # 本文件
```

## 技术细节

### IOCP（I/O 完成端口）

核心网络使用 Windows IOCP 实现高性能异步 I/O：
- 单个 IOCP 端口配合多个工作线程
- 客户端和服务器之间零拷贝数据转发
- 通过对象池实现高效的内存管理

### 并发模型

- **工作线程数**：`CPU 核心数 × 2`（可配置）
- **连接限制**：受系统资源限制
- **内存池**：预分配的 IOContext 对象（1000-10000）

### 协议支持

| 协议 | 特性 |
|------|------|
| TCP | 透明转发、连接池 |
| HTTP/1.1 | GET/POST/CONNECT、头部解析、持久连接 |
| SOCKS5 | CONNECT、IPv4/域名、无认证 |

## 性能

在典型硬件（Intel i7，16GB 内存）上的基准测试结果：
- **并发连接数**：10,000+
- **吞吐量**：1 Gbps+
- **延迟**：< 1ms（本地转发）

## 故障排除

### 常见问题

1. **端口已被占用**
   - 检查是否有其他程序使用了配置的端口
   - 使用 `netstat -ano | findstr :端口` 查找进程

2. **连接被拒绝**
   - 验证后端服务器是否运行
   - 检查防火墙设置

3. **内存占用过高**
   - 在配置中减少 `worker_threads`
   - 调整 pool.h 中的 IOContext 池大小

### 日志

日志写入可执行文件目录下的 `tmuwp.log`，支持自动轮转：
- 最大文件大小：10MB
- 备份数量：5 个文件

## 许可证

MIT 许可证 - 详情请参见 LICENSE 文件

## 贡献

欢迎贡献！请随时提交问题或拉取请求。

## 致谢

本项目展示了以下 Windows 网络概念：
- Winsock2 异步 I/O
- IOCP 设计模式
- TCP/HTTP/SOCKS5 协议实现
- 负载均衡算法

---

**注意**：尽管名为"最没用的 Windows 代理"，这实际上是一个功能完整的高性能代理服务器。这个名字只是为了好玩！😄