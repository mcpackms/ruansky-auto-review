# ruansky_auto_review

基于 QuickJS-ng 嵌入 JS 插件引擎的自动内容审核机器人。

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
- **quickjs-ng >= 0.15.1** (JS 插件引擎)

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

## 编译 quickjs-ng

quickjs-ng 未被收录在主流发行版仓库中，需要从源码编译：

```bash
git clone --depth 1 --branch v0.15.1 https://github.com/quickjs-ng/quickjs-ng.git
cd quickjs-ng
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build build -j$(nproc)
sudo cmake --install build
sudo ldconfig  # Linux only
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

支持使用 JavaScript 编写审核插件，通过 QuickJS-ng 引擎嵌入式运行。

### 特性

- 每个插件独立 JSRuntime + JSContext 隔离
- 40+ 桥接 API（日志、HTTP、KV 存储、文件 I/O、编码、统计等）
- 自动扫描目录加载 .js 文件，无需手动注册
- 10 种 hook 回调（检查前/后、通过/拒绝、轮次开始/结束、错误、暂停/恢复）
- KV 键值存储自动持久化
- 沙箱隔离（移除 eval/Function/WebAssembly）
- 文件 I/O 限制在插件数据目录内

### 快速开始

将 `.js` 文件放入 `plugins/` 目录即可自动加载。详见 `plugins/README.md`。
