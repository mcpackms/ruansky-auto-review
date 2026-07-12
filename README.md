# ruansky_auto_review  
## Install dependencies  
### debian  
``` bash
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config \
    libncursesw5-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    libpcre2-dev \
    nlohmann-json3-dev \
    libtoml11-dev
```
### redhat  
``` bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y \
    cmake pkgconf-pkg-config \
    ncurses-devel \
    libcurl-devel \
    openssl-devel \
    pcre2-devel \
    json-devel
sudo dnf install -y toml11-devel
```
### archlinux  
``` bash
sudo pacman -Sy --needed \
    base-devel cmake pkgconf \
    ncurses \
    curl \
    openssl \
    pcre2 \
    nlohmann-json \
    toml11
```
## Compile

``` bash  
cmake -B build -G Ninja
ninja -C build -j4
```

## Run  

``` bash
chmod +x auto_review  
./auto_review --congfig /path/to/file
```
