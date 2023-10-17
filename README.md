# RDMA_KRPING
**demo**

## 架构图
![RDMA FLOW](https://github.com/fusemen/RDMA_KRPING/assets/122666739/4dfb93be-e11a-4247-848f-ed2519112b01)

<br/>

### 代码测试方法

**1.安装驱动程序。执行`./init_client.sh`**

**2.client端执行`./run_client.sh`**
  - 双端会进行建链操作
  - server端执行对应的`./run_server.sh`。🔔注意先运行`server端`，再运行`client端`。

**3.建链成功之后client端会被阻塞，等待系统中对SSD的读写操作信号flag。**<br/>
  - 利用函数 **wait_event_interruptible(cb->sem, flag == 1)** 将进程阻塞，等待条件flag=1满足继续执行。

**4.在client端运行程序`./write_data`**<br/>
  - `write_data.c`是对使用blk_ops注册的block_dev进行读写的程序,详细介绍在[这里](https://github.com/fusemen/REGISTER-BLOCK-DEVICE)。
  - flag为全局信号，在对块设备blockdev进行读写操作中，若检测到读写程序名为"write_data",则将该程序正在执行的request利用print_request(rq)函数截取,具体原理及方法在[这里](https://github.com/fusemen/BIO-to-RDMA)有详细介绍。
  - 获取正在执行的request的读写标志/虚拟地址/数据长度等信息，利用虚拟地址将内存中的数据读取到缓冲区中。
  - 同时将flag置1，被阻塞的程序继续执行。
  
**5.将缓冲区中的数据通过RDMA操作发送给server端。**
  - 被阻塞的进程通过 **wake_up_interruptible(&cb->sem)** 进行唤醒，继续执行开始RDMA读写操作。

**6.双端程序运行结束，利用`dmesg`打印系统日志查看读写是否成功。**



<br/>
<br/>

# Kernel Mode RDMA Ping Module

### 简介

krping模块是一个内核可加载模块，它利用了Open Fabric动词实现客户端和服务器之间的ping/pong程序。 

这个模块是作为与OFA项目的iwarp分支一起使用的测试工具实现的。



#### 程序目标

- 用于简单测试内核verbs: `connection setup`,`send`,
`recv`, `rdma read`, `rdma write`以及 `completion notifications`.

- Client/Server 双端操作。

- 通过IP地址识别对端。

- 利用RDMA CMA（RDMA通信管理器服务）进行传输独立的操作。

- 不需要用户态程序.

<br/>
该模块允许通过名为 `/proc/krping` 的 `/proc` 入口建立连接并运行 ping/pong 测试。这种简单的机制允许同时启动多个内核线程，无需用户空间应用程序。

krping 模块旨在利用所有主要的 DTO（数据传输操作）操作：send、recv、RDMA read和RDMA write。其目标是测试 API，因此不一定是高效的测试。

一旦建立连接，客户端和服务器开始进行 ping/pong 循环：

|Client| Server|
|:-----|:-------|
|SEND(ping source buffer rkey/addr/len)|                                            |
|				            |  RECV Completion with ping source info|
|				            |  RDMA READ from client source MR|
|				             | RDMA Read completion|
|				              |SEND .go ahead. to client|
|RECV Completion of .go ahead.|                                      |
|SEND (ping sink buffer rkey/addr/len)|                                |	
|				|RECV Completion with ping sink info|
|				|RDMA Write to client sink MR|
|				|RDMA Write completion|
|				|SEND .go ahead. to client|
|RECV Completion of .go ahead.|                               |
Validate data in source and sink buffers

<repeat the above loop>



## 加载krping模块
```shell
# cd krping
# make && make install
# modprobe rdma_krping

//或者使用脚本文件init_client.sh一键加载
# ./init_client.sh
```

## 如何运行

与用户空间的通信通过 `/proc` 文件系统完成。

Krping 导出了一个名为 `/proc/krping` 的文件。将ASCII格式的命令写入 `/proc/krping` 将在内核中启动 krping 线程。

执行写操作到 `/proc/krping` 的线程用于运行 krping 测试，因此它会在测试完成之前阻塞，或者直到用户中断写操作为止。

以下是一个使用 rdma_krping 模块启动 rping 测试的简单示例。
服务器的地址是 192.168.1.16。(服务器地址根据PC具体IP地址设定，使用ifconfig查看IP地址）客户端将连接到该地址的端口 9999 并发送 100 个 ping/pong 消息。
这个示例假定您有两台通过 IB（InfiniBand）连接的系统，而且IPoverIB设备已经按照192.168.69/24子网进行配置。

### Server端:

```
# modprobe rdma_krping
# echo "server,addr=192.168.1.16,port=9999" >/proc/krping
```
执行server端后会进入阻塞状态，等待client端的指令。可使用 <kbd>Ctrl</kbd>+<kbd>C</kbd>退出程序。


### client端:

```
# modprobe rdma_krping
# echo "client,addr=192.168.1.16,port=9999,count=1" >/proc/krping
```

client端也会进入阻塞状态，根据架构图描述的流程，client端会等待用户程序"write_data.c"的执行。

程序接收到"write_data.c"执行的信号后，client端继续执行，读写操作结束后双端程序退出。

### 操作码

|Opcode		|Operand Type|	Description|
|:------|:-------|:-------|
|client	|	none	|	启动一个客户端krping线程.|
|server	|	none	|	启动一个服务器端krping线程.|
|addr	|	string	|	服务器的IP地址，点分十进制格式。注意，服务器可以使用0.0.0.0绑定到所有设备。|								
|port	|	integer|		以主机字节顺序表示的服务器端口号。|				
|count	|	integer	|	在关闭测试之前要执行的循环迭代次数。如果未指定，计数是无限的。|								
|size	|	integer	|	ping数据的大小。krping的默认值是65字节。|				
|verbose	|	none|		Enables printk()s that dump the rping data. Use with caution!|				
|validate	|none	|	Enables validating the rping data on each iteration to detect data corruption.|							
|mem_mode|	string	|	Determines how memory will be registered.  Modes include dma,and reg.  Default is dma.|				
|server_inv |	none|		Valid only in reg mr mode, this option enables invalidating the client's reg mr via SEND_WITH_INVALIDATE messages from the server.|											
|local_dma_lkey|	none|		Use the local dma lkey for the source of writes and sends, and in recvs	|	
|read_inv|	none	|	Server will use READ_WITH_INV. Only valid in reg mem_mode.|
				
				
============
Memory Usage:
============

The krping client uses 4 memory areas:

start_buf - the source of the ping data.  This buffer is advertised to
the server at the start of each iteration, and the server rdma reads
the ping data from this buffer over the wire.

rdma_buf  - the sink of the ping data.  This buffer is advertised to the
server each iteration, and the server rdma writes the ping data that it
read from the start buffer into this buffer.  The start_buf and rdma_buf
contents are then compared if the krping validate option is specified.

recv_buf  - used to recv "go ahead" SEND from the server.  

send_buf  - used to advertise the rdma buffers to the server via SEND
messages.

The krping server uses 3 memory areas:

rdma_buf  - used as the sink of the RDMA READ to pull the ping data
from the client, and then used as the source of an RDMA WRITE to
push the ping data back to the client.

recv_buf  - used to receive rdma rkey/addr/length advertisements from
the client.

send_buf  - used to send "go ahead" SEND messages to the client.


============
Memory Registration Modes:
============

Each of these memory areas are registered with the RDMA device using
whatever memory mode was specified in the command line. The mem_mode
values include: dma, and reg (aka fastreg).  The default mode, if not
specified, is dma.

The dma mem_mode uses a single dma_mr for all memory buffers.

The reg mem_mode uses a reg mr on the client side for the
start_buf and rdma_buf buffers.  Each time the client will advertise
one of these buffers, it invalidates the previous registration and fast
registers the new buffer with a new key.   If the server_invalidate
option is on, then the server will do the invalidation via the "go ahead"
messages using the IB_WR_SEND_WITH_INV opcode.   Otherwise the client
invalidates the mr using the IB_WR_LOCAL_INV work request.

On the server side, reg mem_mode causes the server to use the
reg_mr rkey for its rdma_buf buffer IO.  Before each rdma read and
rdma write, the server will post an IB_WR_LOCAL_INV + IB_WR_REG_MR
WR chain to register the buffer with a new key.  If the krping read-inv
option is set then the server will use IB_WR_READ_WITH_INV to do the
rdma read and skip the IB_WR_LOCAL_INV wr before re-registering the
buffer for the subsequent rdma write operation.

============
Stats
============

While krping threads are executing, you can obtain statistics on the
thread by reading from the /proc/krping file.  If you cat /proc/krping,
you will dump IO statistics for each running krping thread.  The format
is one thread per line, and each thread contains the following stats
separated by white spaces:

Statistic		Description
---------------------------------------------------------------------
Name			krping thread number and device being used.
Send Bytes		Number of bytes transferred in SEND WRs.
Send Messages		Number of SEND WRs posted
Recv Bytes		Number of bytes received via RECV completions.
Recv Messages		Number of RECV WRs completed.
RDMA WRITE Bytes	Number of bytes transferred in RDMA WRITE WRs.
RDMA WRITE Messages	Number of RDMA WRITE WRs posted.
RDMA READ Bytes		Number of bytes transferred via RDMA READ WRs.
RDMA READ Messages	Number of RDMA READ WRs posted.

Here is an example of the server side output for 5 krping threads:

# cat /proc/krping
1-amso0 0 0 16 1 12583960576 192016 0 0
2-mthca0 0 0 16 1 60108570624 917184 0 0
3-mthca0 0 0 16 1 59106131968 901888 0 0
4-mthca1 0 0 16 1 101658394624 1551184 0 0
5-mthca1 0 0 16 1 100201922560 1528960 0 0
#

============
EXPERIMENTAL
============

There are other options that enable micro benchmarks to measure
the kernel rdma performance.  These include:

Opcode		Operand Type	Description
------------------------------------------------------------------------
wlat		none		Write latency test
rlat		none		read latency test
poll		none		enable polling vs blocking for rlat
bw		none		write throughput test
duplex		none		valid only with bw, this
				enables bidirectional mode
tx-depth	none		set the sq depth for bw tests


See the awkit* files to take the data logged in the kernel log
and compute RTT/2 or Gbps results.

Use these at your own risk.


END-OF-FILE


