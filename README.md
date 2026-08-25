# ruansky_auto_review

RUANAKY平台内容审核机器人
版本 1.3.5
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

### Debian

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

### Redhat

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

### Archlinux

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

## 运行时依赖（可选）

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

- 每个插件运行在独立 OS 进程中，崩溃不影响主程序
- **完整 Node.js 权限**：无沙箱、无模块封锁、无路径限制
- 可使用所有 Node.js 内置模块和任意 npm 包
- 内置便利 API（日志、HTTP 请求、KV 存储、文件 I/O）
- 自动扫描目录加载 `.js` 文件，无需手动注册
- 10 种 hook 回调（检查前/后、通过/拒绝、轮次开始/结束、错误、暂停/恢复）
- KV 键值存储自动持久化到 `plugins/data/<插件名>/store.json`

### 快速开始

将 `.js` 文件放入 `plugins/` 目录即可自动加载。详见 [`plugins/README.md`](plugins/README.md)。

### 插件许可证说明
本项目的插件机制通过定义良好的 API 与主程序通信。
插件作为独立作品，其许可证由插件作者自行选择，
不受主程序 AGPL 协议的约束。