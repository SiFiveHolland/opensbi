// SPDX-License-Identifier: BSD-2-Clause
/*
 * fdt_helper.c - Flat Device Tree manipulation helper routines
 * Implement helper routines on top of libfdt for OpenSBI usage
 *
 * Copyright (C) 2020 Bin Meng <bmeng.cn@gmail.com>
 */

#include <libfdt.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi_utils/fdt/fdt_helper.h>

#define DEFAULT_UART_FREQ		0
#define DEFAULT_UART_BAUD		115200
#define DEFAULT_UART_REG_SHIFT		0
#define DEFAULT_UART_REG_IO_WIDTH	1

#define DEFAULT_SIFIVE_UART_FREQ		0
#define DEFAULT_SIFIVE_UART_BAUD		115200
#define DEFAULT_SIFIVE_UART_REG_SHIFT		0
#define DEFAULT_SIFIVE_UART_REG_IO_WIDTH	4

const struct fdt_match *fdt_match_node(void *fdt, int nodeoff,
				       const struct fdt_match *match_table)
{
	int ret;

	if (!fdt || nodeoff < 0 || !match_table)
		return NULL;

	while (match_table->compatible) {
		ret = fdt_node_check_compatible(fdt, nodeoff,
						match_table->compatible);
		if (!ret)
			return match_table;
		match_table++;
	}

	return NULL;
}

int fdt_find_match(void *fdt, const struct fdt_match *match_table,
		   const struct fdt_match **out_match)
{
	int nodeoff;

	if (!fdt || !match_table)
		return SBI_ENODEV;

	while (match_table->compatible) {
		nodeoff = fdt_node_offset_by_compatible(fdt, -1,
						match_table->compatible);
		if (nodeoff >= 0) {
			if (out_match)
				*out_match = match_table;
			return nodeoff;
		}
		match_table++;
	}

	return SBI_ENODEV;
}

int fdt_get_node_addr_size(void *fdt, int node, unsigned long *addr,
			   unsigned long *size)
{
	int parent, len, i;
	int cell_addr, cell_size;
	const fdt32_t *prop_addr, *prop_size;
	uint64_t temp = 0;

	parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return parent;
	cell_addr = fdt_address_cells(fdt, parent);
	if (cell_addr < 1)
		return SBI_ENODEV;

	cell_size = fdt_size_cells(fdt, parent);
	if (cell_size < 0)
		return SBI_ENODEV;

	prop_addr = fdt_getprop(fdt, node, "reg", &len);
	if (!prop_addr)
		return SBI_ENODEV;
	prop_size = prop_addr + cell_addr;

	if (addr) {
		for (i = 0; i < cell_addr; i++)
			temp = (temp << 32) | fdt32_to_cpu(*prop_addr++);
		*addr = temp;
	}
	temp = 0;

	if (size) {
		for (i = 0; i < cell_size; i++)
			temp = (temp << 32) | fdt32_to_cpu(*prop_size++);
		*size = temp;
	}

	return 0;
}

int fdt_parse_hart_id(void *fdt, int cpu_offset, u32 *hartid)
{
	int len;
	const void *prop;
	const fdt32_t *val;

	if (!fdt || cpu_offset < 0)
		return SBI_EINVAL;

	prop = fdt_getprop(fdt, cpu_offset, "device_type", &len);
	if (!prop || !len)
		return SBI_EINVAL;
	if (sbi_strcmp(prop, "cpu"))
		return SBI_EINVAL;

	val = fdt_getprop(fdt, cpu_offset, "reg", &len);
	if (!val || len < sizeof(fdt32_t))
		return SBI_EINVAL;

	if (len > sizeof(fdt32_t))
		val++;

	if (hartid)
		*hartid = fdt32_to_cpu(*val);

	return 0;
}

int fdt_parse_max_hart_id(void *fdt, u32 *max_hartid)
{
	u32 hartid;
	int err, cpu_offset, cpus_offset;

	if (!fdt)
		return SBI_EINVAL;
	if (!max_hartid)
		return 0;

	*max_hartid = 0;

	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0)
		return cpus_offset;

	fdt_for_each_subnode(cpu_offset, fdt, cpus_offset) {
		err = fdt_parse_hart_id(fdt, cpu_offset, &hartid);
		if (err)
			continue;

		if (hartid > *max_hartid)
			*max_hartid = hartid;
	}

	return 0;
}

int fdt_parse_sifive_uart_node(void *fdt, int nodeoffset,
			       struct platform_uart_data *uart)
{
	int len, rc;
	const fdt32_t *val;
	unsigned long reg_addr, reg_size;

	if (nodeoffset < 0 || !uart || !fdt)
		return SBI_ENODEV;

	rc = fdt_get_node_addr_size(fdt, nodeoffset, &reg_addr, &reg_size);
	if (rc < 0 || !reg_addr || !reg_size)
		return SBI_ENODEV;
	uart->addr = reg_addr;

	/**
	 * UART address is mandaotry. clock-frequency and current-speed
	 * may not be present. Don't return error.
	 */
	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "clock-frequency", &len);
	if (len > 0 && val)
		uart->freq = fdt32_to_cpu(*val);
	else
		uart->freq = DEFAULT_SIFIVE_UART_FREQ;

	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "current-speed", &len);
	if (len > 0 && val)
		uart->baud = fdt32_to_cpu(*val);
	else
		uart->baud = DEFAULT_SIFIVE_UART_BAUD;

	/* For SiFive UART, the reg-shift and reg-io-width are fixed .*/
	uart->reg_shift = DEFAULT_SIFIVE_UART_REG_SHIFT;
	uart->reg_io_width = DEFAULT_SIFIVE_UART_REG_IO_WIDTH;

	return 0;
}

int fdt_parse_uart8250_node(void *fdt, int nodeoffset,
			    struct platform_uart_data *uart)
{
	int len, rc;
	const fdt32_t *val;
	unsigned long reg_addr, reg_size;

	if (nodeoffset < 0 || !uart || !fdt)
		return SBI_ENODEV;

	rc = fdt_get_node_addr_size(fdt, nodeoffset, &reg_addr, &reg_size);
	if (rc < 0 || !reg_addr || !reg_size)
		return SBI_ENODEV;
	uart->addr = reg_addr;

	/**
	 * UART address is mandaotry. clock-frequency and current-speed
	 * may not be present. Don't return error.
	 */
	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "clock-frequency", &len);
	if (len > 0 && val)
		uart->freq = fdt32_to_cpu(*val);
	else
		uart->freq = DEFAULT_UART_FREQ;

	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "current-speed", &len);
	if (len > 0 && val)
		uart->baud = fdt32_to_cpu(*val);
	else
		uart->baud = DEFAULT_UART_BAUD;

	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "reg-shift", &len);
	if (len > 0 && val)
		uart->reg_shift = fdt32_to_cpu(*val);
	else
		uart->reg_shift = DEFAULT_UART_REG_SHIFT;

	val = (fdt32_t *)fdt_getprop(fdt, nodeoffset, "reg-io-width", &len);
	if (len > 0 && val)
		uart->reg_io_width = fdt32_to_cpu(*val);
	else
		uart->reg_io_width = DEFAULT_UART_REG_IO_WIDTH;

	return 0;
}

int fdt_parse_uart8250(void *fdt, struct platform_uart_data *uart,
		   const char *compatible)
{
	int nodeoffset;

	if (!compatible || !uart || !fdt)
		return SBI_ENODEV;

	nodeoffset = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (nodeoffset < 0)
		return nodeoffset;

	return fdt_parse_uart8250_node(fdt, nodeoffset, uart);
}

int fdt_parse_plic(void *fdt, struct platform_plic_data *plic,
		   const char *compatible)
{
	int nodeoffset, len, rc;
	const fdt32_t *val;
	unsigned long reg_addr, reg_size;

	nodeoffset = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (nodeoffset < 0)
		return nodeoffset;

	rc = fdt_get_node_addr_size(fdt, nodeoffset, &reg_addr, &reg_size);
	if (rc < 0 || !reg_addr || !reg_size)
		return SBI_ENODEV;
	plic->addr = reg_addr;

	val = fdt_getprop(fdt, nodeoffset, "riscv,ndev", &len);
	if (len > 0)
		plic->num_src = fdt32_to_cpu(*val);

	return 0;
}

int fdt_parse_compat_addr(void *fdt, unsigned long *addr,
			  const char *compatible)
{
	int nodeoffset, rc;

	nodeoffset = fdt_node_offset_by_compatible(fdt, -1, compatible);
	if (nodeoffset < 0)
		return nodeoffset;

	rc = fdt_get_node_addr_size(fdt, nodeoffset, addr, NULL);
	if (rc < 0 || !addr)
		return SBI_ENODEV;

	return 0;
}
