# Sonar \[C\]ernel Commander (SCC)

## Overview
Sonar \[C\]ernel Commander (SCC) is an advanced Linux kernel module that specializes in syscall hooking and event logging. Designed with performance and extensibility in mind, SCC offers a robust solution for system call monitoring and provides a seamless interface for user-space applications.

## Features
- **Advanced Syscall Hooking**: Efficiently monitor and log system call activities with minimal performance overhead.
- **Detailed Event Logging**: Capture comprehensive details of each syscall, including arguments, return values, and precise timestamps.
- **User-Space Interface**: Leverage a character device to facilitate direct and efficient communication between user space and the kernel module.
- **Dynamic Control**: Dynamically manage hooking behavior and logging mechanisms through simple read/write operations.

## Getting Started

### Prerequisites
- Linux kernel version 5.x or newer.
- Kernel development and compilation tools (e.g., `make`, `gcc`, `kernel headers`).

### Installation Instructions

1. **Clone the repository:**
   ```sh
   git clone https://github.com/25077667/sonar-cernel-commander
   cd sonar-cernel-commander
   ```

2. **Compile the module:**
   ```sh
   make
   ```

3. **Insert the module into the kernel:**
   ```sh
   sudo insmod scc.ko
   ```

4. **Verify the module is loaded:**
   ```sh
   lsmod | grep scc
   ```

## Usage Guide

### Interacting with SCC
The SCC module registers a character device named `scc`. You can interact with this device to control and monitor the module's behavior.

- **Reading Events**: Fetch logged syscall events with detailed information.
- **Writing Commands**: Send commands to control hooking behavior, toggle event logging, or configure module settings.

### Examples
- **Reading from the device:**
  ```sh
  cat /dev/scc
  ```
- **Writing to the device:**
  ```sh
  echo "command" > /dev/scc
  ```
- **Use Python**
See [/client/client.py](client/client.py) for an example of how to interact with the SCC module using Python.

## Contributing

We welcome contributions from the community. If you wish to contribute to SCC, please submit a pull request with a clear description of your changes, adhering to the coding standards and documentation practices of the project.

## License

SCC is distributed under the Dual BSD/GPL License.

## Acknowledgments

A heartfelt thank you to all individuals and organizations who have contributed to the development and maintenance of SCC.

## Contact

For support, feedback, or queries, please reach out to [open an issue](github.com/sonar-kernel-commander/scc/issues) on our GitHub repository.

## Project Status

SCC is currently in its alpha stage of development. It is under active development, and we advise against using it in critical production environments until it reaches a stable release.

