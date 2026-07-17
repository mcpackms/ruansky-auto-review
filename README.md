# ruansky_auto_review

基于 Node.js 子进程 JS 插件引擎的自动内容审核机器人。

## 安全公告

> **v1.5.0+** 插件引擎已迁移至 `vm.createContext` 沙箱执行，封锁了 `child_process`、`net`、`tls`、`vm` 等高危模块。
> 插件只能通过白名单内置模块（`crypto`、`path`、`url` 等）和注入的安全 API（HTTP、KV、文件 I/O）进行开发。
> 详情见 [`plugins/README.md`](plugins/README.md)。

## 依赖

### 编译依赖

- CMake >= 3.14
- C++17 编译器
- Ninja（推荐）或 Make
- ncursesw (TUI)
- libcurl (HTTP)
- OpenSSL (加密)
- libpcre2 (正则)
- nlohmann-json3
- toml11
- **Node.js >= 18** (JS 插件引擎，运行时依赖)

### debian

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config \
    ninja-build \
    libncursesw5-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libpcre2-dev \
    nlohmann-json3-dev \
    libtoml11-dev
```

### redhat

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y \
    cmake pkgconf-pkg-config \
    ninja-build \
    ncurses-devel \
    libcurl-devel \
    openssl-devel \
    pcre2-devel \
    json-devel
sudo dnf install -y toml11-devel
```

### archlinux

```bash
sudo pacman -Sy --needed \
    base-devel cmake pkgconf \
    ncurses \
    ninja \
    curl \
    openssl \
    pcre2 \
    nlohmann-json \
    toml11
```

## 运行时依赖

插件系统依赖 **Node.js >= 18**。大部分发行版可直接安装：

```bash
# Debian/Ubuntu
sudo apt install nodejs

# Red Hat/Fedora
sudo dnf install nodejs

# Arch Linux
sudo pacman -S nodejs
```

## 编译

```bash
cmake -B build -G Ninja
ninja -C build -j4
```

## 运行

```bash
cd build
./auto_review --config /path/to/settings.toml
```

## JS 插件系统

支持使用 JavaScript 编写审核插件，通过 **Node.js 子进程 + `vm.createContext` 沙箱**运行。

### 特性

- 每个插件运行在独立操作系统中，崩溃不影响主程序
- **沙箱隔离**：通过 `vm.createContext` 执行，封锁高危 Node.js 模块
- 白名单安全内置模块（`crypto`、`path`、`url`、`util` 等）
- 内置安全 API（日志、HTTP 请求、KV 存储、受限文件 I/O）
- 自动扫描目录加载 `.js` 文件，无需手动注册
- 10 种 hook 回调（检查前/后、通过/拒绝、轮次开始/结束、错误、暂停/恢复）
- KV 键值存储自动持久化到 `plugins/data/<插件名>/store.json`

### 快速开始

将 `.js` 文件放入 `plugins/` 目录即可自动加载。详见 [`plugins/README.md`](plugins/README.md)。
