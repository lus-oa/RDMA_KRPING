#include <linux/build-salt.h>
#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(.gnu.linkonce.this_module) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section(__versions) = {
	{ 0x839e3f9d, "module_layout" },
	{ 0x359bb5d3, "__blk_mq_end_request" },
	{ 0x90388c79, "kmalloc_caches" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0xeb3e1a4f, "blk_cleanup_queue" },
	{ 0x349cba85, "strchr" },
	{ 0x6c0f3d55, "single_open" },
	{ 0x3489689b, "param_ops_int" },
	{ 0x754d539c, "strlen" },
	{ 0x7fccc099, "__ubsan_handle_load_invalid_value" },
	{ 0x5831119d, "ib_dealloc_pd_user" },
	{ 0xde214f8f, "single_release" },
	{ 0x2479ab5d, "blk_mq_start_request" },
	{ 0x20000329, "simple_strtoul" },
	{ 0xdf566a59, "__x86_indirect_thunk_r9" },
	{ 0xc6be5df6, "seq_printf" },
	{ 0x56470118, "__warn_printk" },
	{ 0xb43f9365, "ktime_get" },
	{ 0x830a1a00, "remove_proc_entry" },
	{ 0xbc5307b3, "__rdma_create_id" },
	{ 0x16ad67bf, "rdma_destroy_id" },
	{ 0x409bcb62, "mutex_unlock" },
	{ 0x85df9b6c, "strsep" },
	{ 0x33bda228, "dma_free_attrs" },
	{ 0x5a96cdc6, "__ubsan_handle_builtin_unreachable" },
	{ 0x97651e6c, "vmemmap_base" },
	{ 0x953e1b9e, "ktime_get_real_seconds" },
	{ 0xa4afa7ce, "seq_read" },
	{ 0xe2d5255a, "strcmp" },
	{ 0x5c16557c, "rdma_connect" },
	{ 0xc82e2138, "ib_destroy_cq_user" },
	{ 0x9aa9d059, "__alloc_disk_node" },
	{ 0xd9a5ea54, "__init_waitqueue_head" },
	{ 0x7bc885c, "__rdma_accept" },
	{ 0x1ffb8dc1, "ib_destroy_qp_user" },
	{ 0x39fa905c, "__ib_create_cq" },
	{ 0xe5c51729, "__ubsan_handle_divrem_overflow" },
	{ 0xfa873479, "current_task" },
	{ 0xc5850110, "printk" },
	{ 0x94746641, "del_gendisk" },
	{ 0x73c9fcc9, "rdma_listen" },
	{ 0x4c9d28b0, "phys_base" },
	{ 0xa1c76e0a, "_cond_resched" },
	{ 0x9166fada, "strncpy" },
	{ 0x4f269fda, "dma_direct_map_page" },
	{ 0xeeeecba2, "dma_alloc_attrs" },
	{ 0x2ab7989d, "mutex_lock" },
	{ 0x71a50dbc, "register_blkdev" },
	{ 0xfda9581f, "prandom_u32" },
	{ 0x72b0387, "blk_update_request" },
	{ 0x19de7c8a, "init_net" },
	{ 0xfe487975, "init_wait_entry" },
	{ 0xee03fdfe, "ib_map_mr_sg" },
	{ 0xb5a459dc, "unregister_blkdev" },
	{ 0xdf2d5be1, "rdma_create_qp" },
	{ 0x7cd8d75e, "page_offset_base" },
	{ 0xfb26633c, "rdma_bind_addr" },
	{ 0xba814844, "module_put" },
	{ 0x4ab513a4, "dma_direct_unmap_page" },
	{ 0x7ced0815, "blk_mq_init_sq_queue" },
	{ 0xb6ee9e1c, "rdma_resolve_route" },
	{ 0xdecd0b29, "__stack_chk_fail" },
	{ 0x1000e51, "schedule" },
	{ 0x8ddd8aad, "schedule_timeout" },
	{ 0xb8b9f817, "kmalloc_order_trace" },
	{ 0xac5fcec0, "in4_pton" },
	{ 0xc9f75f28, "put_disk" },
	{ 0x2ea2c95c, "__x86_indirect_thunk_rax" },
	{ 0x329f9e36, "ib_alloc_mr_user" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xb6ab78cb, "rdma_disconnect" },
	{ 0x95c4db30, "__dev_get_by_name" },
	{ 0x7f5d7378, "kmem_cache_alloc_trace" },
	{ 0x3eeb2322, "__wake_up" },
	{ 0x8c26d495, "prepare_to_wait_event" },
	{ 0x4e4cd2a1, "ib_dereg_mr_user" },
	{ 0x4f0affca, "seq_lseek" },
	{ 0x37a0cba, "kfree" },
	{ 0x69acdf38, "memcpy" },
	{ 0x8e62faed, "rdma_set_service_type" },
	{ 0x609bcd98, "in6_pton" },
	{ 0xa5f28e68, "rdma_resolve_addr" },
	{ 0x92540fbf, "finish_wait" },
	{ 0xb06bdb49, "__ib_alloc_pd" },
	{ 0xa476e7fc, "device_add_disk" },
	{ 0x5e6956c0, "proc_create" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0xf199d370, "dma_ops" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0x1d230637, "try_module_get" },
};

MODULE_INFO(depends, "ib_core,rdma_cm");


MODULE_INFO(srcversion, "3101F0E37E5F189C9632E8F");
