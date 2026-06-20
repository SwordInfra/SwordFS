<p align="center"><a href="https://github.com/MetanoiaLab/SwordFS"><img alt="SwordFS Logo" src="docs/en/images/swordfs-logo.svg" width="80%" /></a></p>
<p align="center">

# SwordFS

## What is SwordFS?
SwordFS is a modern, high-performance distributed file system. It is POSIX compliant and designed for the modern workloads of AI/ML applications. The aim of SwordFS is to be the de facto standard for distributed file systems in the AI/ML era.

The major differences between SwordFS and other distributed file systems are as follows:
- High Performance: Performance is the top priority in SwordFS's architecture and feature design. That's why SwordFS is built with C++20, a battle-tested system programming language.
- Client-heavy: SwordFS is designed with heavy logic on the client side, so that the server-side I/O path is minimal. This is a direct result of the performance-first philosophy.
- AI/ML-friendly: SwordFS integrates with GPUs and DPUs natively and seamlessly. It is purpose-built for AI/ML workloads.

## Architecture
SwordFS is a client-heavy file system where both metadata semantics and data plane operations are primarily handled on the client side, with the server mainly responsible for metadata and data distribution. Similar to JuiceFS, the open-source version of SwordFS does not implement a new distributed storage system for metadata or data. For metadata, we rely on mature existing key-value databases such as Redis and TiKV to achieve persistence and distributed scalability. For the data plane, our default storage system is object storage (e.g., AWS S3, Alibaba Cloud OSS). Given that we target AL/ML workloads with very high read/write throughput requirements, we recommend all-flash object storage (e.g., S3 Express One Zone).

Traditional object storage systems are clearly a performance bottleneck on the I/O path. Despite providing good scalability and throughput, performance in the AI era needs to be delivered at a much higher level. For instance, object storage is typically TCP-based, which is difficult to compare with RDMA-based transport that can move data to GPUs faster via GPUDirect Storage for training or inference. Furthermore, traditional object storage, due to its complex protocol semantics, often requires gateway-layer proxying for I/O forwarding, which significantly increases I/O path complexity and degrades performance. The USE (Ultimate Storage Engine) available in our enterprise edition is designed on a completely different philosophy: it implements the stateless logic layer of the data plane storage system on the client side and persists data by directly accessing remote JBOF (Just a Bunch of Flash) via NVMe over Fabric technology, achieving data plane distribution in the most efficient way possible. Moreover, unlike object storage systems, the USE storage engine supports random write semantics, eliminating the performance degradation caused by data fragmentation in heavy random-write scenarios and ensuring consistently high performance across all workloads.


## Build

### Prerequisites

- **CMake** >= 3.19
- **Ninja** build system
- **C++20** compatible compiler (GCC >= 11, Clang >= 14)

### Clone

```bash
git clone --recurse-submodules https://github.com/MetanoiaLab/SwordFS.git
cd SwordFS
```

### Build

```bash
# Debug build
cmake --preset default
cmake --build build

# Release build
cmake --preset release
cmake --build build
```

Or invoke Ninja directly:

```bash
cmake --preset default && ninja -C build
cmake --preset release && ninja -C build
```

## Acknowledgements
- [Folly](https://github.com/facebook/folly): A library of C++20 components designed with practicality and efficiency in mind, from Facebook.
- [JuiceFS](https://github.com/juicedata/juicefs): A high-performance POSIX file system designed for cloud-native environments. SwordFS draws significant inspiration from JuiceFS.
