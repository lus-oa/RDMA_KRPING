# RDMA_KRPING
**demo**

![RDMA FLOW](https://github.com/fusemen/RDMA_KRPING/assets/122666739/4dfb93be-e11a-4247-848f-ed2519112b01)

<br/>

### 代码测试方法

**1.安装驱动程序。执行`./init_client.sh`**

**2.client端执行`./run_client.sh`**
  - 双端会进行建链操作
  - server端执行对应的`./run_server.sh`。注意先运行`server端`，再运行`client端`。

**3.建链成功之后client端会被阻塞，等待系统中对SSD的读写操作信号flag。**<br/>
  - 利用函数wait_event_interruptible(cb->sem, flag == 1)将进程阻塞，等待条件flag=1满足继续执行。

**4.在client端运行程序`./write_data`**<br/>
  - `write_data.c`是对使用blk_ops的block_dev进行读写的程序,详细介绍在[这里](https://github.com/fusemen/REGISTER-BLOCK-DEVICE)。
  - flag为全局信号，在对块设备blockdev进行读写操作中，若检测到读写程序名为"write_data",则将该程序正在执行的request利用print_request(rq)函数截取，获取其中的读写信号/虚拟地址/数据长度等信息，利用虚拟地址将内存中的数据读取到缓冲区中。
  - 同时将flag置1，被阻塞的程序继续执行。
  
 
**5.将缓冲区中的数据通过rdma操作发送给server端。**

**6.利用`dmesg`打印系统日志查看是否运行成功。**




