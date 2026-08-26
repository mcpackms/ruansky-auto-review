#!/bin/bash
# ruansky-auto-review 一键安装脚本
# 支持 Debian/Ubuntu、RedHat/Fedora/CentOS、Arch Linux、Termux

set -e

REPO_URL="https://github.com/mcpackms/ruansky-auto-review.git"
PROXY_URL="https://gh-proxy.org/"
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
INSTALL_DIR="$HOME/ruansky-auto-review"

# --- 颜色定义 ---
C_RED='\033[0;31m'
C_GREEN='\033[0;32m'
C_YELLOW='\033[1;33m'
C_BLUE='\033[0;34m'
C_CYAN='\033[0;36m'
C_BOLD='\033[1m'
C_NC='\033[0m' # No Color

# --- 日志函数 ---
log_info()  { echo -e "${C_BLUE}[*]${C_NC} $1"; }
log_ok()    { echo -e "${C_GREEN}[+]${C_NC} $1"; }
log_err()   { echo -e "${C_RED}[-]${C_NC} $1" >&2; }
log_warn()  { echo -e "${C_YELLOW}[!]${C_NC} $1"; }
log_step()  { echo -e "\n${C_CYAN}==>${C_NC} ${C_BOLD}$1${C_NC}"; }

detect_os() {
    [[ -n "$TERMUX_VERSION" ]] && echo "termux" && return
    [[ -f /etc/os-release ]] && . /etc/os-release && echo "$ID" || echo "unknown"
}

setup_termux() {
    log_step "配置 Termux 环境"
    termux-setup-storage >/dev/null 2>&1 || true
    
    sed -i 's@^\(deb.*stable main\)$@#\1\ndeb https://mirrors.tuna.tsinghua.edu.cn/termux/termux-packages-24 stable main@' $PREFIX/etc/apt/sources.list 2>/dev/null || true
    apt update -qq
}

install_deps() {
    log_step "安装系统依赖"
    case "$1" in
        termux)
            setup_termux
            apt install -y build-essential cmake ninja ncurses \
                libcurl openssl pcre2 nlohmann-json toml11 nodejs git
            ;;
        debian|ubuntu)
            sudo apt update
            sudo apt install -y build-essential cmake ninja-build \
                libncursesw5-dev libcurl4-openssl-dev libssl-dev \
                libpcre2-dev nlohmann-json3-dev libtoml11-dev nodejs git
            ;;
        rhel|fedora|centos|rocky|almalinux)
            sudo dnf install -y cmake ninja-build ncurses-devel \
                libcurl-devel openssl-devel pcre2-devel json-devel \
                toml11-devel nodejs git 2>/dev/null || \
            sudo yum install -y cmake ninja-build ncurses-devel \
                libcurl-devel openssl-devel pcre2-devel json-devel \
                toml11-devel nodejs git
            ;;
        arch|manjaro)
            sudo pacman -Sy --needed --noconfirm base-devel cmake ninja ncurses \
                curl openssl pcre2 nlohmann-json toml11 nodejs git
            ;;
        *)
            log_err "未知系统: $1，请手动安装依赖"
            exit 1
            ;;
    esac
    log_ok "依赖安装完成"
}

choose_proxy() {
    log_step "选择 GitHub 克隆方式"
    echo "  1) 使用代理 (推荐国内用户)"
    echo "  2) 直连 (国外用户或网络良好)"
    echo "  3) 自动检测 (先直连，失败则切代理)"
    read -p "[?] 请输入选项 [1/2/3]: " choice
    
    case "$choice" in
        1)
            log_info "使用代理克隆..."
            git clone --depth 1 "${PROXY_URL}${REPO_URL}" "$INSTALL_DIR"
            ;;
        2)
            log_info "直连克隆..."
            git clone --depth 1 "$REPO_URL" "$INSTALL_DIR"
            ;;
        3|"")
            log_info "自动检测中..."
            git clone --depth 1 "$REPO_URL" "$INSTALL_DIR" 2>/dev/null || \
                git clone --depth 1 "${PROXY_URL}${REPO_URL}" "$INSTALL_DIR"
            ;;
        *)
            log_warn "无效选项，使用自动检测"
            git clone --depth 1 "$REPO_URL" "$INSTALL_DIR" 2>/dev/null || \
                git clone --depth 1 "${PROXY_URL}${REPO_URL}" "$INSTALL_DIR"
            ;;
    esac
}

clone_repo() {
    if [[ -d "$INSTALL_DIR" ]]; then
        log_warn "目录已存在: $INSTALL_DIR，跳过克隆"
        cd "$INSTALL_DIR"
        return
    fi
    
    choose_proxy
    cd "$INSTALL_DIR"
}

main() {
    clear
    echo -e "${C_BOLD}========================================${C_NC}"
    echo -e "${C_BOLD}    ruansky-auto-review 一键安装脚本    ${C_NC}"
    echo -e "${C_BOLD}========================================${C_NC}"
    
    OS=$(detect_os)
    log_info "系统: $OS | 目录: $INSTALL_DIR | 线程: $JOBS"
    
    install_deps "$OS"
    clone_repo
    
    log_step "编译项目"
    log_info "开始编译 (使用 $JOBS 线程)... 这可能需要长达5分钟的时间"
    cmake -B build -G Ninja
    ninja -C build -j "$JOBS"
    
    EXEC_PATH="$INSTALL_DIR/build/auto_review"
    
    echo ""
    log_ok "编译完成！"
    log_info "可执行文件: $EXEC_PATH"
    echo -e "\n${C_YELLOW}提示: 配置文件 settings.toml 默认放在项目根目录${C_NC}"
    echo -e "运行命令: ${C_GREEN}cd $INSTALL_DIR && ./build/auto_review --config ./settings.toml${C_NC}\n"
    echo -e "设置命令: ${C_GREEN}cd $INSTALL_DIR && ./build/auto_review --set-family${C_NC}\n"
}

main "$@"