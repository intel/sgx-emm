Intel(R) Software Guard Extensions Enclave Memory Manager
=============================================================
Introduction
---------------------------------
This is the implementation of the Enclave Memory Manager proposed in [this design doc](design_docs/SGX_EMM.md).

The EMM public APIs defined in [sgx_mm.h](include/sgx_mm.h) are intended to encapsulate low level details
of managing the basic EDMM flows for dynamically allocating/deallocating EPC pages, changing EPC page
permissions and page types. The typical target users of these APIs are intermediate level components
in SGX runtimes: heap, stack managers with dynamic expansion capabilities, mmap/mprotect/pthread API
implementations for enclaves, dynamic code loader and JIT compilers, etc.
 
This implementation aims to be reusable in any SGX runtime that provides a minimal C runtime and
implements the abstraction layer APIs defined in [sgx_mm_rt_abstraction.h](include/sgx_mm_rt_abstraction.h).
To port and integrate the EMM module into any SGX runtime, follow the [porting guide](design_docs/SGX_EMM.md#porting-emm-to-different-runtimes) in the design document.

The build instructions provided here are for developing and testing the EMM functionality with the Intel SDK and PSW build environment.

**Note:**  The main-line kernel has builtin EDMM support since release v6.0.
The original patches were reviewed on LKML in [this thread](https://lore.kernel.org/lkml/YnrllJ2OqmcqLUuv@kernel.org/T/).

Prerequisites
-------------------------------

#### Build and install kernel with EDMM support
On Ubuntu 18.04/20.04/22.04, follow the general instructions from [here](https://wiki.ubuntu.com/KernelTeam/GitKernelBuild) with these changes.

- For step 1, clone the kernel repo and checkout a branch/tag with sgx EDMM support (v6.0 or later)
```
$ git clone https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/
$ cd linux
$ git checkout v6.0
```

- For step 6, modify .config to set "CONFIG_X86_SGX=y".

**Note:** on Ubuntu 20.04,  ensure that /dev does not have noexec set:
```
mount | grep "/dev .*noexec"
```
If so, remount it executable:
```
sudo mount -o remount,exec /dev
```

#### Verify kernel build and EDMM support
At the root of the kernel source repo,
```
$ cd tools/testing/selftests/sgx/ && make
#./test_sgx
```
#### Add udev rules to map sgx device nodes and set right permissions
Download [10-sgx.rules](https://github.com/intel/SGXDataCenterAttestationPrimitives/blob/master/driver/linux/10-sgx.rules) and activate it as follows.
```
$ sudo cp  10-sgx.rules /etc/udev/rules.d
$ sudo groupadd sgx_prv
$ sudo udevadm trigger
```
Build and Install SDK and PSW
------------------------------

#### Clone linux-sgx repo and checkout edmm branch
```
$ git clone https://github.com/intel/linux-sgx.git $repo_root
$ cd $repo_root
```
Following steps assume $repo_root is the top directory of the linux-sgx repo you cloned.

#### To build and install SDK with EDMM support
```
$ cd $repo_root
$ make preparation
$ make sdk_install_pkg_no_mitigation
$ cd linux/installer/bin
$ ./sgx_linux_x64_sdk_2.15.100.3.bin
# follow its prompt to set SDK installation destination directory, $SGX_SDK
$ source $SGX_SDK/environment
```

#### To build and setup libsgx_enclave_common and libsgx_urts
To test EMM functionalities without involving remote attestation, we only need libsgx_enclave_common and libsgx_urts built and point LD_LIBRARY_PATH to them.

```
$ cd $repo_root/psw/urts/linux
$ make
$ cd <repo_root>/build/linux
$ ln -s libsgx_enclave_common.so libsgx_enclave_common.so.1
$ export LD_LIBRARY_PATH=<repo_root>/build/linux/
```

#### To build and run API tests
```
$ cd $repo_root/external/sgx-emm/api_tests/
$ make
$ ./test_mm_api
# or run tests in loop in background
$ nohup bash ./test_loop.sh 1000 &
# check results in nohup log:
$ tail -f nohup.out
```

**Note:** On Ubuntu 22.04 or any distro with systemd v248 or later, /dev/sgx_enclave is only accessible by users in the group "sgx". The test or any enclave app should be run with a uid in the sgx group.
```
# check systemd version:
$ systemctl --version
# add sgx group to user if it's 248 or above:
$ sudo usermod -a -G sgx <user name>
```

Limitations of current implementation
---------------------------------------
1. The EMM holds a global recursive mutex for the whole duration of each API invocation.
	- No support for concurrent operations (modify type/permissions, commit and commit_data) on different regions.
2. The EMM internally uses a separate dynamic allocator (emalloc) to manage its internal memory allocation for EMA objects and bitmaps of the regions.
    - During initialization, the EMM emalloc will create an initial reserve region from the user range (given by RTS, see below). And it may add more reserves later also from the user range if needed.
    - RTS and SDK signing tools can estimate this overhead with (total size of all RTS regions and user regions)/2^14. And account for it when calculating the enclave size.
	- Before calling any EMM APIs, the RTS needs initialize EMM by calling sgx_mm_init pass in an address range [user_start, user_end) for user allocation.
        - The EMM allocates all user requested region(via sgx_mm_alloc API) in this range only.
3. Allocations created by the RTS enclave loader at fixed address ranges can be reserved with SGX_EMA_SYSTEM flag after EMM initializations.
	- For example, for a heap region to be dynamically expanded:
		- The RTS calls mm_init_ema to create region for the static heap (EADDed), and mm_alloc to reserve COMMIT_ON_DEMAND for dynamic heap.
	- Stack expansion should be done in 1st phase exception handler and use a reserved static stack so that stack is not overrun in sgx_mm API calls during stack expansion.
4. The EMM relies on vDSO interface to guarantee that fault handler is called on the same OS thread where fault happened.
	- This is due to the use of the global recursive mutex. If fault handler comes in from different thread while the mutex is held, it will deadlock.
	- Note a #PF could happen when more stack is needed inside EMM functions while the mutex is locked.
		- vDSO user handler should ensure it re-enters enclave with the original TCS and on the same OS thread.
		- To avoid potential deadlocks, no other mutex/lock should be used in this path from user handler to first phase exception handler inside enclave.
5. Not optimized for performance

Notes on Intel SDK specific implementation
-----------------------------------------
1. 	Intel SDK RTS abstraction layer mutex implementation is a spinlock because there is no built-in OCalls for wait/wake on OS event.
2. 	Currently API tests are built with Intel SDK thus located in [Intel SDK repo](https://github.com/intel/linux-sgx/external/sgx-emm/api_tests). Though most of tests are RTS independent, the TCS related tests use hardcoded Intel thread context layout info. The random allocation test cases use Intel SDK sgx_read_rand for random number generation.


