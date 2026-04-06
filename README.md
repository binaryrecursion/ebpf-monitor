#ebpf-monitor

## Setup & Running

### 1. Clone the repo
```bash
git clone https://github.com/<your-username>/ebpf-monitor.git
cd ebpf-monitor
```

### 2. Install dependencies
```bash
sudo apt update
sudo apt install -y clang llvm libbpf-dev libelf-dev make gcc
```

### 3. build
```bash
make clean
make
```

### 4. run
```bash
sudo ./ebpf-monitor
```


