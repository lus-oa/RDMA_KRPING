#!/bin/bash
sudo modprobe rdma_cm
sudo rmmod rdma_krping
sudo make clean
sudo make
sudo insmod rdma_krping.ko